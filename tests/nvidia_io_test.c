/* Test the actual VBE callback with mocked I/O, not host port access. */
#define HOST_TEST
#define MYOS_HARDWARE
#include "../drivers/video/nvidia_io.c"
#include "../drivers/video/runtime.c"
extern int printf(const char *,...);
extern void exit(int);
#define CHECK(c) do{if(!(c)){printf("NVIDIA I/O test line %d: %s\n",__LINE__,#c);exit(1);}}while(0)
static u32 model_bar1=0xfd00000c,model_high=0,model_bar5=0xe001,model_command=3;
static u32 writes,reads,seen_port,seen_value,seen_bytes;
u32 pci_read(u8 bus,u8 dev,u8 func,u8 offset) {
    CHECK(bus==5 && dev==0 && func==0);
    if(offset==0x14)return model_bar1;
    if(offset==0x18)return model_high;
    if(offset==0x24)return model_bar5;
    CHECK(offset==4);return model_command;
}
void kprintf(const char *format,...) {(void)format;}
void x86emu_stop(x86emu_t *e) {(void)e;}
u32 video_bios_port_read(u16 port,u32 bytes) {reads++;seen_port=port;seen_bytes=bytes;return 0x2469fdb9;}
void video_bios_port_write(u16 port,u32 value,u32 bytes) {writes++;seen_port=port;seen_bytes=bytes;seen_value=value;}
static void check_callback(u32 base) {
    nv_io=base;failed=false;u32 value=1;
    CHECK(access(&cpu,base,&value,0x402)==0); /* user's exact OUT DWORD case */
    CHECK(!failed && seen_port==base && seen_value==1 && seen_bytes==4);
    CHECK(access(&cpu,base,&value,0x302)==0);
    CHECK(value==0x2469fdb9 && seen_port==base && seen_bytes==4);
    CHECK(access(&cpu,base+0x7c,&value,0x402)==0);
    u32 before=writes;
    CHECK(access(&cpu,base+0x7d,&value,0x402)!=0 && failed && writes==before);
    failed=false;
    CHECK(access(&cpu,base+0x80,&value,0x402)!=0 && failed && writes==before);
    nv_io=0;failed=false;
    CHECK(access(&cpu,base,&value,0x402)!=0 && failed && writes==before);
    failed=false;CHECK(access(&cpu,0x20,&value,0x400)!=0 && failed && writes==before);
}
int main(void) {
    struct pci_dev gpu={.bus=5,.dev=0,.func=0,.vendor=0x10de,.device=0x21c4,.class_code=3};
    u32 base=0;
    CHECK(nvidia_firmware_io_base(&gpu,&base) && base==0xe000);
    check_callback(base);
    model_bar5=0xc001;
    CHECK(nvidia_firmware_io_base(&gpu,&base) && base==0xc000);
    check_callback(base);
    model_bar5=0xff81;
    CHECK(nvidia_firmware_io_base(&gpu,&base) && base==0xff80);
    check_callback(base);
    /* Regression: a different LFB/VRAM aperture must not disable PCI-owned I/O.
     * The screenshot's scanout base is 0xf1000000; no size assumptions or BAR
     * writes are needed to validate an independently assigned BAR5 window. */
    model_bar5=0xe001;model_bar1=0xf000000c;model_high=0;
    CHECK(nvidia_firmware_io_base(&gpu,&base) && base==0xe000);
    model_bar1=0xd000000c;model_high=1;
    CHECK(nvidia_firmware_io_base(&gpu,&base) && base==0xe000);
    for(u32 i=0;i<5;i++) {
        model_bar5=0xe001;model_command=3;
        if(i==0)model_bar5=0xe000; /* memory BAR */
        if(i==1)model_bar5=0x10001; /* outside x86 port space */
        if(i==2)model_bar5=0xe005; /* not a 128-byte aligned window */
        if(i==3)model_bar5=1;
        if(i==4)model_command=2; /* firmware left I/O decoding disabled */
        base=123;CHECK(!nvidia_firmware_io_base(&gpu,&base) && base==0);
    }
    model_bar1=0xfd00000c;model_high=0;model_bar5=0xe001;model_command=3;
    gpu.vendor=0x1234;CHECK(!nvidia_firmware_io_base(&gpu,&base));
    gpu.vendor=0x10de;gpu.device=0x1234;CHECK(!nvidia_firmware_io_base(&gpu,&base));
    CHECK(!nvidia_firmware_io_contains(0xe000,0xffffffff,4));
    CHECK(!nvidia_firmware_io_contains(0xe000,0xdfff,1));
    CHECK(!nvidia_firmware_io_contains(0xe000,0xe000,8));
    CHECK(!nvidia_firmware_io_contains(0xe000,0xe000,0));
    CHECK(!nvidia_firmware_io_contains(0xffffe000,0xe000,4));
    CHECK(reads==3 && writes==6);
    printf("NVIDIA BAR5: exact e000/type=402 regression + relocation + bounds + independent scanout aperture + PCI ownership: OK\n");
    return 0;
}
