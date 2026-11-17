/* Real NVMe command/queue code against a deterministic MMIO + DMA model. */
#define HOST_TEST
#define MYOS_HARDWARE
#include "../kernel/nvme.c"
int printf(const char*,...);void exit(int);
#define CHECK(c) do{if(!(c)){printf("NVME FAIL line %d: %s\n",__LINE__,#c);exit(1);}}while(0)
static u32 regs[0x1100/4],now,pci_command,allocated,pci_writes,io_reads,io_writes,flushes;
static u32 io_sq,io_cq;
static u8 memory[7][4096] __attribute__((aligned(4096)));
static u8 disk[2][4096*512],buffer[16384];
static struct boot_info boot;
static unsigned lba_shift=9,metadata,format;
enum { NONE, IO_ERROR, FLUSH_ERROR, TIMEOUT, BAD_CID, BAD_SQID, FATAL };
static int fault;
static bool no_enable,no_disable,no_shutdown;
const struct boot_info *nvme_test_boot(void){return &boot;}
u32 timer_ticks(void){return now;}
void task_sleep_ticks(u32 n){now+=n;}
void kprintf(const char *fmt,...){(void)fmt;}
void mutex_lock(struct mutex *m){(void)m;}void mutex_unlock(struct mutex *m){(void)m;}
bool ahci_present(int u){(void)u;return false;}
u32 pmm_alloc(void){CHECK(allocated<7);return (u32)memory[allocated++];}
int vmm_map(u32 v,u32 p,u32 flags){(void)v;CHECK(p>=0xf0000000u && (flags&PG_WRITE));return 0;}
bool pci_next(unsigned cursor,struct pci_dev *out) {
    if(cursor>32)return false;
    memset(out,0,sizeof(*out));out->dev=4;out->class_code=1;out->subclass=8;return true;
}
u32 pci_read(u8 b,u8 d,u8 f,u8 off) {
    (void)b;(void)d;(void)f;
    if(off==4)return pci_command;
    if(off==8)return 0x01080200;
    if(off==0x10)return 0xf0000004; /* 64-bit BAR below 4 GiB */
    if(off==0x14)return 0;
    CHECK(false);return 0;
}
void pci_write(u8 b,u8 d,u8 f,u8 off,u32 value){(void)b;(void)d;(void)f;CHECK(off==4);pci_command=value;pci_writes++;}
u32 nvme_test_read(struct nvme_controller *c,u32 off){(void)c;CHECK(off<sizeof(regs));return regs[off/4];}
static void transfer(u32 *command,u8 *bytes,unsigned n,bool to_prp) {
    unsigned first=n>4096?4096:n;
    if(to_prp){memcpy((void *)command[6],bytes,first);if(n>first)memcpy((void *)command[8],bytes+first,n-first);}
    else{memcpy(bytes,(void *)command[6],first);if(n>first)memcpy(bytes+first,(void *)command[8],n-first);}
}
void nvme_test_write(struct nvme_controller *c,u32 off,u32 value) {
    CHECK(off<sizeof(regs));regs[off/4]=value;
    if(off==CC){if(value&1){if(!no_enable)regs[CSTS/4]=1;if(((value>>14)&3)==1 && !no_shutdown)regs[CSTS/4]|=2u<<2;}else if(!no_disable)regs[CSTS/4]=0;return;}
    if(off!=0x1000 && off!=0x1008)return;
    unsigned qid=off==0x1008;struct nvme_queue *q=&c->queue[qid];
    u32 sq=qid?io_sq:regs[ASQ/4],cq=qid?io_cq:regs[ACQ/4];
    u32 *command=(void *)(sq+((value+q->entries-1)%q->entries)*64);
    u32 opcode=command[0]&255;bool error=false;
    if(qid && fault==TIMEOUT)return;
    if(qid && fault==FATAL){regs[CSTS/4]=3;return;}
    if(!qid) {
        if(opcode==6) {
            u8 *id=(void *)command[6];memset(id,0,4096);
            if(command[10]==1){memset(id+24,' ',40);memcpy(id+24,"MODEL TEST",10);id[512]=0x66;id[513]=0x44;*(u32 *)(id+516)=19;}
            else if(command[10]==2){((u32 *)id)[0]=7;((u32 *)id)[1]=19;}
            else if(command[10]==0) {
                CHECK(command[1]==7 || command[1]==19);
                *(u32 *)id=200000;*(u32 *)(id+8)=200000;
                id[25]=format;id[26]=(format&15)|((format&0x30)<<1);
                id[128+4*format]=metadata;id[130+4*format]=lba_shift;
            } else CHECK(false);
        } else if(opcode==5){CHECK(command[10]==(1u|15u<<16) && command[11]==1);io_cq=command[6];}
        else if(opcode==1){CHECK(command[10]==(1u|15u<<16) && command[11]==(1u|1u<<16));io_sq=command[6];}
        else CHECK(opcode==9 && command[10]==7 && !command[11]);
    } else {
        unsigned ns=command[1]==7?0:1;CHECK(command[1]==7 || command[1]==19);
        if(opcode==0){flushes++;error=fault==FLUSH_ERROR;}
        else {
            CHECK(opcode==1 || opcode==2);unsigned count=(command[12]&65535)+1;
            CHECK(count<=16 && !command[11] && command[10]+count<=4096);
            error=fault==IO_ERROR;
            if(opcode==1){io_writes++;if(!error)transfer(command,disk[ns]+command[10]*512,count*512,false);}
            else{io_reads++;if(!error)transfer(command,disk[ns]+command[10]*512,count*512,true);}
        }
    }
    u32 *entry=(void *)(cq+q->head*16);memset(entry,0,16);
    entry[2]=value|((qid+(qid && fault==BAD_SQID))<<16);
    entry[3]=(command[0]>>16)+(qid && fault==BAD_CID);
    entry[3]|=(u32)q->phase<<16;if(error)entry[3]|=1u<<17;
}
static void reset(void) {
    memset(controllers,0,sizeof(controllers));memset(namespaces,0,sizeof(namespaces));attempted=false;
    memset(regs,0,sizeof(regs));memset(memory,0,sizeof(memory));memset(disk,0,sizeof(disk));
    regs[CAP/4]=15;regs[(CAP+4)/4]=1u<<5;regs[VS/4]=0x10400;
    now=allocated=pci_writes=io_reads=io_writes=flushes=io_sq=io_cq=0;pci_command=6;
    no_enable=no_disable=no_shutdown=false;fault=NONE;lba_shift=9;metadata=format=0;
    memset(&boot,0,sizeof(boot));boot.version=2;boot.root_phys=LIVE_ROOT_PHYS;boot.root_bytes=64u*1048576;
}
static void mbr(unsigned ns) {
    u8 *p=disk[ns];*(u32 *)(p+440)=0x12345678;
    *(u32 *)(p+454)=2048;*(u32 *)(p+458)=131072;p[510]=0x55;p[511]=0xaa;
}
int main(void) {
    reset();CHECK(!nvme_init() && !pci_writes && !allocated); /* Live stays read-only. */
    CHECK(!nvme_installer_probe());CHECK(nvme_present(3)&&nvme_present(4)&&!nvme_present(0));
    CHECK(namespaces[3].id==7 && namespaces[4].id==19 && nvme_sectors(3)==200000);
    CHECK(!strcmp(nvme_model(3),"NVMe MODEL TEST"));
    for(unsigned i=0;i<sizeof(buffer);i++)buffer[i]=(i*13)^(i>>8);
    CHECK(!nvme_write(3,8,buffer,32)&&io_writes==2);
    CHECK(!memcmp(disk[0]+8*512,buffer,sizeof(buffer)));
    for(unsigned i=0;i<sizeof(disk[1]);i++)CHECK(!disk[1][i]);
    u8 sector[512];CHECK(!nvme_read(3,8,sector)&&!memcmp(sector,buffer,512));
    CHECK(!nvme_flush(3)&&flushes==1);
    unsigned reads=io_reads;CHECK(nvme_read(3,200000,sector)<0&&io_reads==reads);
    CHECK(nvme_write(3,199999,buffer,2)<0);
    for(int i=0;i<40;i++)CHECK(!nvme_read(3,0,sector)); /* CQ phase + SQ tail wrap. */
    fault=IO_ERROR;CHECK(nvme_write(3,0,buffer,1)<0 && !controllers[0].failed);
    fault=FLUSH_ERROR;CHECK(nvme_flush(3)<0 && !controllers[0].failed);
    fault=NONE;CHECK(!nvme_flush(3));
    nvme_shutdown();CHECK(!controllers[0].ready && ((regs[CSTS/4]>>2)&3)==2);
    reset();CHECK(!nvme_installer_probe());no_shutdown=true;nvme_shutdown();
    CHECK(controllers[0].failed && !(pci_command&4));
    for(int f=TIMEOUT;f<=FATAL;f++) {
        reset();CHECK(!nvme_installer_probe());fault=f;
        memset(sector,0xcc,sizeof(sector));CHECK(nvme_read(3,0,sector)<0);
        CHECK(controllers[0].failed && !(pci_command&4) && nvme_present(3));
        for(unsigned i=0;i<sizeof(sector);i++)CHECK(sector[i]==0xcc);
        u8 before[512];memcpy(before,memory[4],512);
        CHECK(nvme_write(3,0,buffer,1)<0 && !memcmp(before,memory[4],512));
    }
    reset();lba_shift=12;CHECK(nvme_installer_probe()<0 && !nvme_present(3)&&!io_writes);
    reset();metadata=8;CHECK(nvme_installer_probe()<0 && !nvme_present(3)&&!io_writes);
    reset();format=16;CHECK(!nvme_installer_probe()&&nvme_present(3));
    reset();no_enable=true;CHECK(nvme_installer_probe()<0 && !(pci_command&4));
    reset();no_disable=true;regs[CC/4]=regs[CSTS/4]=1;
    CHECK(nvme_installer_probe()<0 && !allocated && !(pci_command&4));
    reset();boot.root_disk_id=0x12345678;mbr(0);
    CHECK(!nvme_installer_probe()&&namespaces[3].id==19&&!nvme_present(4));
    reset();boot.version=3;boot.root_phys=boot.root_bytes=0;boot.storage_flags=BOOT_DISK_ROOT;
    boot.root_disk_id=0x12345678;boot.root_lba=2048;boot.root_sectors=131072;mbr(1);
    CHECK(nvme_init()&&namespaces[0].id==19&&namespaces[3].id==7);
    reset();boot.version=3;boot.root_phys=boot.root_bytes=0;boot.storage_flags=BOOT_DISK_ROOT;
    boot.root_disk_id=0x12345678;boot.root_lba=2048;boot.root_sectors=131072;mbr(0);mbr(1);
    CHECK(!nvme_init()&&!nvme_present(0));
    printf("NVMe: admin/I/O queues, sparse NSIDs, PRP1/2, phase wrap, namespace isolation, flush, errors/timeouts, DMA quarantine, 4Kn/metadata refusal and boot ID selection: PASS\n");return 0;
}
