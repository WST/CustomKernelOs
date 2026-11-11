/* Exercise real firmware/NVIDIA backend with PCI fixtures, no privileged I/O. */
#include "../drivers/video/firmware.c"
#include "../drivers/video/nvidia.c"
extern int printf(const char *,...);
extern void exit(int);
extern int strcmp(const char *,const char *);
#define CHECK(x) do { if (!(x)) { printf("video driver: line %d: %s\n",__LINE__,#x); exit(1); } } while (0)
static bool present=true;
static u16 device=0x21c4;
static u32 bar=0xe000000cu,high;
void kprintf(const char *format,...) { (void)format; }
bool pci_find_display_vendor(u16 vendor,struct pci_dev *out) {
    CHECK(vendor==0x10de);
    if (!present) return false;
    *out=(struct pci_dev){.bus=5,.dev=2,.func=1,.vendor=vendor,.device=device,.class_code=3};
    return true;
}
u32 pci_read(u8 bus,u8 dev,u8 func,u8 offset) {
    CHECK(bus==5 && dev==2 && func==1);
    CHECK(offset==0x14 || offset==0x18);
    return offset==0x14?bar:high;
}
static struct boot_info good(void) {
    return (struct boot_info){.magic=BOOT_INFO_MAGIC,.version=1,.font_phys=0x5000,
        .fb_phys=0xe0000000,.width=1920,.height=1080,.bpp=32,.pitch=8192};
}
int main(void) {
    struct boot_info b=good();struct fb_info info;
    CHECK(video_firmware_init(&b,&info));
    CHECK(info.pitch==8192 && info.vram==8192*1080 && !(info.caps&FB_CAP_GPU));
    nvidia_firmware_identify(&info);
    CHECK(!strcmp(info.driver,"NVIDIA 1660 SUPER VBE"));
    for (int test=0;test<6;test++) {
        b=good();bar=0xe000000c;high=0;present=true;device=0x21c4;
        CHECK(video_firmware_init(&b,&info));
        if(test==0) present=false;
        if(test==1) bar=0xd000000c; /* hybrid GPU is not the scanout owner */
        if(test==2) high=1;       /* do not truncate 64-bit BAR */
        if(test==3) bar=0xe0000001; /* I/O BAR */
        if(test==4) bar=0;
        if(test==5) bar=0xe0000002; /* reserved memory BAR type */
        nvidia_firmware_identify(&info);
        CHECK(!strcmp(info.driver,"BIOS VBE framebuffer"));
    }
    b=good();bar=0xe000000c;high=0;present=true;device=0x1234;
    CHECK(video_firmware_init(&b,&info));nvidia_firmware_identify(&info);
    CHECK(!strcmp(info.driver,"NVIDIA BIOS VBE"));
    for(int test=0;test<11;test++) {
        b=good();
        if(test==0)b.magic=0;
        if(test==1)b.version=2;
        if(test==2)b.font_phys=0;
        if(test==3)b.fb_phys++;
        if(test==4)b.bpp=24;
        if(test==5)b.width=2561;
        if(test==6)b.height=0;
        if(test==7)b.pitch=4;
        if(test==8)b.pitch++;
        if(test==9)b.pitch=0x01000000;
        if(test==10)b.fb_phys=0xfffff000;
        CHECK(!video_firmware_init(&b,&info));
    }
    b=good();b.width=2560;b.height=1600;b.pitch=10240;
    CHECK(video_firmware_init(&b,&info));
    CHECK(!video_firmware_init(0,&info) && !video_firmware_init(&b,0));
    printf("Firmware geometry/pitch/overflow + NVIDIA 1660 SUPER/BAR/hybrid attribution: OK\n");
    return 0;
}
