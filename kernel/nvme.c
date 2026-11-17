/* PCIe NVM Express, NVM command set, 512-byte LBAs without metadata.
 * Two controllers, one polled I/O queue per controller; physically allocated
 * PRP bounce pages keep userspace and heap addresses away from DMA.
 * Register/command layouts: NVM Express Base 1.4 and NVM Command Set.
 */
#include "nvme.h"
#include "pci.h"
#include "pmm.h"
#include "paging.h"
#include "task.h"
#include "timer.h"
#include "string.h"
#include "vga.h"
#include "bootinfo.h"
#include "ahci.h"

#define CONTROLLERS 2
#define MMIO_WINDOW 0x100000u
#define DEPTH 16u
#define MAX_SECTORS 0x10000000u
enum { CAP=0, VS=8, INTMS=12, CC=20, CSTS=28, AQA=36, ASQ=40, ACQ=48 };
struct nvme_queue {u32 sq,cq;u16 tail,head,entries,cid;u8 phase;};
struct nvme_controller {
    volatile u8 *mmio;
    struct pci_dev pci;
    struct mutex lock;
    struct nvme_queue queue[2];
    u32 pages[7],stride,ready_ticks,namespace_count;
    bool failed,ready;
    char model[48];
};
struct nvme_namespace {struct nvme_controller *controller;u32 id,sectors;};
static struct nvme_controller controllers[CONTROLLERS];
static struct nvme_namespace namespaces[5];
static bool attempted;

#ifdef HOST_TEST
u32 nvme_test_read(struct nvme_controller*,u32);
void nvme_test_write(struct nvme_controller*,u32,u32);
const struct boot_info *nvme_test_boot(void);
static u32 rd(struct nvme_controller *c,u32 off){return nvme_test_read(c,off);}
static void wr(struct nvme_controller *c,u32 off,u32 value){nvme_test_write(c,off,value);}
#define nvme_boot nvme_test_boot
#else
static u32 rd(struct nvme_controller *c,u32 off){return *(volatile u32 *)(c->mmio+off);}
static void wr(struct nvme_controller *c,u32 off,u32 value){*(volatile u32 *)(c->mmio+off)=value;}
#define nvme_boot boot_info_get
#endif
static u32 le32(const u8 *p){return p[0]|(u32)p[1]<<8|(u32)p[2]<<16|(u32)p[3]<<24;}
static u16 le16(const u8 *p){return p[0]|(u16)p[1]<<8;}
static void barrier(void){__sync_synchronize();}
static int quarantine(struct nvme_controller *c,const char *reason) {
    c->failed=true;
    u32 cmd=pci_read(c->pci.bus,c->pci.dev,c->pci.func,4);
    pci_write(c->pci.bus,c->pci.dev,c->pci.func,4,(cmd&0xffffu)&~4u);
    (void)pci_read(c->pci.bus,c->pci.dev,c->pci.func,4);
    /* Keep every DMA page and binding. Never fall back to another disk after
     * a timeout: the outstanding command may still reference these pages. */
    kprintf("NVMe %u:%u.%u: %s; controller DMA disabled\n",c->pci.bus,c->pci.dev,c->pci.func,reason);
    return -1;
}
static bool ready_wait(struct nvme_controller *c,bool enabled) {
    u32 start=timer_ticks();
    do {
        u32 status=rd(c,CSTS);
        if(enabled && (status&2))return false;
        if((bool)(status&1)==enabled)return true;
        task_sleep_ticks(1);
    }while((u32)(timer_ticks()-start)<c->ready_ticks);
    return false;
}
/* Exactly one outstanding command. Caller owns c->lock and bounce pages. */
static int submit(struct nvme_controller *c,unsigned qid,u32 command[16]) {
    if(c->failed || !c->ready || (rd(c,CSTS)&3)!=1)return quarantine(c,"controller not ready");
    struct nvme_queue *q=&c->queue[qid];u16 cid=++q->cid;
    command[0]=(command[0]&0xffffu)|((u32)cid<<16);
    memcpy((void *)(q->sq+q->tail*64),command,64);
    barrier();q->tail=(q->tail+1)%q->entries;
    wr(c,0x1000+(2*qid)*c->stride,q->tail);
    volatile u32 *completion=(volatile u32 *)(q->cq+q->head*16);
    u32 start=timer_ticks();
    for(;;) {
        u32 status=completion[3];
        if(((status>>16)&1)==q->phase) {
            barrier();
            if((u16)status!=cid || (completion[2]>>16)!=qid || (completion[2]&65535)>=q->entries)
                return quarantine(c,"invalid completion");
            q->head++;
            if(q->head==q->entries){q->head=0;q->phase^=1;}
            barrier();wr(c,0x1000+(2*qid+1)*c->stride,q->head);
            return (status>>17)?-1:0;
        }
        if(rd(c,CSTS)&2)return quarantine(c,"fatal controller status");
        if((u32)(timer_ticks()-start)>=10u*TIMER_HZ)return quarantine(c,"command timeout");
        /* Fast completions avoid a whole timer tick per small read. */
        for(unsigned spin=0;spin<1000;spin++) {
            if(((completion[3]>>16)&1)==q->phase)break;
            __asm__ volatile("pause");
        }
        if(((completion[3]>>16)&1)!=q->phase)task_sleep_ticks(1);
    }
}
static int identify(struct nvme_controller *c,u32 nsid,u32 cns,u32 page) {
    u32 command[16]={0};memset((void *)page,0,PAGE_SIZE);
    command[0]=6;command[1]=nsid;command[6]=page;command[10]=cns;
    return submit(c,0,command);
}
static int raw_io(struct nvme_controller *c,u32 nsid,u32 lba,void *buf,u32 count,bool write) {
    if(!count || count>16 || c->failed)return -1;
    u32 n=count*512,first=n>PAGE_SIZE?PAGE_SIZE:n;
    if(write) {
        memcpy((void *)c->pages[4],buf,first);
        if(n>first)memcpy((void *)c->pages[5],(const u8 *)buf+first,n-first);
    }
    u32 command[16]={0};command[0]=write?1:2;command[1]=nsid;
    command[6]=c->pages[4];if(n>PAGE_SIZE)command[8]=c->pages[5];
    command[10]=lba;command[12]=count-1;
    if(submit(c,1,command))return -1;
    if(!write) {
        memcpy(buf,(void *)c->pages[4],first);
        if(n>first)memcpy((u8 *)buf+first,(void *)c->pages[5],n-first);
    }
    return 0;
}
static bool controller_start(struct nvme_controller *c,const struct pci_dev *dev,unsigned number) {
    c->pci=*dev;
    u32 bar=pci_read(dev->bus,dev->dev,dev->func,0x10);
    if((bar&1) || (bar&6)==2)return false;
    if((bar&6)==4 && pci_read(dev->bus,dev->dev,dev->func,0x14)) {
        kprintf("NVMe %u:%u.%u: MMIO above 4 GiB unsupported; disable Above 4G Decoding in firmware\n",dev->bus,dev->dev,dev->func);
        return false;
    }
    u32 physical=bar&~15u,base=physical&~4095u;
    if(!physical || base>0xffffffffu-MMIO_WINDOW)return false;
    /* Firmware assigned BARs only. No resource relocation or BAR-size probing. */
    u32 pci_cmd=pci_read(dev->bus,dev->dev,dev->func,4)&65535u;
    pci_write(dev->bus,dev->dev,dev->func,4,(pci_cmd|2u)&~4u);
    u32 virt=NVME_MMIO_BASE+number*MMIO_WINDOW;
    if(vmm_map(virt,base,PG_WRITE|0x18))return false;
    c->mmio=(volatile u8 *)(virt+(physical&4095));
    u32 low=rd(c,CAP),high=rd(c,CAP+4);
    if(low==0xffffffffu || !(high&(1u<<5)) || ((high>>16)&15) || (low&65535)<1)return false;
    c->stride=4u<<(high&15);
    u32 end=(physical&4095)+0x1000+3*c->stride+4;
    if(end>MMIO_WINDOW)return false;
    for(u32 off=PAGE_SIZE;off<end;off+=PAGE_SIZE)
        if(vmm_map(virt+off,base+off,PG_WRITE|0x18))return false;
    unsigned units=(low>>24);if(!units)units=1;
    c->ready_ticks=units*((TIMER_HZ+1)/2);
    wr(c,INTMS,0xffffffffu);wr(c,CC,rd(c,CC)&~1u);
    if(!ready_wait(c,false)){quarantine(c,"disable timeout");return false;}
    for(unsigned i=0;i<7;i++) {
        c->pages[i]=pmm_alloc();
        if(!c->pages[i])return false; /* DMA is disabled; pages retained. */
        memset((void *)c->pages[i],0,PAGE_SIZE);
    }
    u16 entries=(low&65535)+1u;if(entries>DEPTH || !entries)entries=DEPTH;
    for(unsigned i=0;i<2;i++) {
        c->queue[i].sq=c->pages[2*i];c->queue[i].cq=c->pages[2*i+1];
        c->queue[i].entries=entries;c->queue[i].phase=1;
    }
    wr(c,AQA,(entries-1)|((u32)(entries-1)<<16));
    wr(c,ASQ,c->pages[0]);wr(c,ASQ+4,0);wr(c,ACQ,c->pages[1]);wr(c,ACQ+4,0);
    pci_write(dev->bus,dev->dev,dev->func,4,pci_cmd|6u|(1u<<10));
    barrier();wr(c,CC,1u|(6u<<16)|(4u<<20));
    if(!ready_wait(c,true)){quarantine(c,"enable timeout");return false;}
    c->ready=true;
    if(identify(c,0,1,c->pages[4]))return false;
    const u8 *id=(void *)c->pages[4];
    if((id[512]&15)>6 || (id[512]>>4)<6 || (id[513]&15)>4 || (id[513]>>4)<4)return false;
    strcpy(c->model,"NVMe ");memcpy(c->model+5,id+24,40);c->model[45]=0;
    for(int i=44;i>=5 && c->model[i]==' ';i--)c->model[i]=0;
    c->namespace_count=le32(id+516);
    u32 command[16]={0};command[0]=9;command[10]=7; /* one SQ/CQ pair */
    if(submit(c,0,command))return false;
    memset(command,0,sizeof(command));command[0]=5;command[6]=c->pages[3];
    command[10]=1u|((u32)(entries-1)<<16);command[11]=1; /* CQ: contiguous, no IRQ */
    if(submit(c,0,command))return false;
    memset(command,0,sizeof(command));command[0]=1;command[6]=c->pages[2];
    command[10]=1u|((u32)(entries-1)<<16);command[11]=1u|(1u<<16); /* SQ -> CQ1 */
    return !submit(c,0,command);
}
static u32 namespace_size(const u8 *id) {
    unsigned format=id[26]&15;
    if(id[25]>=16)format|=(id[26]&0x60)>>1;
    if(format>id[25] || format>=64 || (id[26]&0x80) || (id[29]&7))return 0;
    const u8 *lba=id+128+format*4;
    /* Never reformat a namespace to make it compatible with MyFS. */
    if(le16(lba) || lba[2]!=9 || (!le32(id+8) && !le32(id+12)))return 0;
    u32 n=le32(id);if(le32(id+4) || n>MAX_SECTORS)n=MAX_SECTORS;
    return n;
}
static bool start(bool installer) {
    if(attempted)return nvme_present(0)||nvme_present(3)||nvme_present(4);
    attempted=true;const struct boot_info *boot=nvme_boot();
    bool installed=boot_disk_root(boot);unsigned root_matches=0;
    struct pci_dev dev;unsigned cursor=0,number=0;
    while(number<CONTROLLERS && pci_next(cursor,&dev)) {
        cursor=(((unsigned)dev.bus<<8)|((unsigned)dev.dev<<3)|dev.func)+1;
        if(dev.class_code!=1 || dev.subclass!=8 || ((pci_read(dev.bus,dev.dev,dev.func,8)>>8)&255)!=2)continue;
        struct nvme_controller *c=&controllers[number];
        if(!controller_start(c,&dev,number++))continue;
        /* Active NSIDs need not start at 1 or be contiguous. Keep the list on
         * a separate page while Identify Namespace reuses the data page. */
        if(identify(c,0,2,c->pages[6])) {
            if(c->failed)continue;
            memset((void *)c->pages[6],0,PAGE_SIZE);
            unsigned count=c->namespace_count;if(count>1024)count=1024;
            for(unsigned i=0;i<count;i++)((u32 *)c->pages[6])[i]=i+1;
        }
        const u32 *ids=(void *)c->pages[6];
        for(unsigned i=0;i<1024 && ids[i] && !c->failed;i++) {
            u32 nsid=ids[i];if(nsid==0xffffffffu)continue;
            if(identify(c,nsid,0,c->pages[4]))continue;
            u32 size=namespace_size((void *)c->pages[4]);
            if(!size){kprintf("NVMe %u ns %u: unsupported/empty LBA format (need 512 bytes, no metadata)\n",number-1,nsid);continue;}
            u8 mbr[512];if(raw_io(c,nsid,0,mbr,1,false))continue;
            bool match=boot && boot->root_disk_id && le32(mbr+440)==boot->root_disk_id && mbr[510]==0x55 && mbr[511]==0xaa;
            if(installer && match)continue; /* Preserve the Live boot medium. */
            int slot=0;
            if(installed && match && !installer) {
                if(++root_matches>1){namespaces[0].controller=NULL;continue;}
                if(ahci_present(0))continue; /* SATA already owns the root. */
                if(le32(mbr+454)!=boot->root_lba || le32(mbr+458)!=boot->root_sectors ||
                   boot->root_lba>=size || boot->root_sectors>size-boot->root_lba)continue;
            } else {
                slot=!nvme_present(3)?3:!nvme_present(4)?4:-1;
                if(slot<0)continue;
            }
            namespaces[slot]=(struct nvme_namespace){c,nsid,size};
            kprintf("NVMe %u:%u.%u ns %u: %s, %u KB (unit %d)\n",dev.bus,dev.dev,dev.func,nsid,c->model,size/2,slot);
        }
    }
    if(root_matches>1){namespaces[0].controller=NULL;kprintf("NVMe: duplicate boot disk IDs; refusing root selection\n");}
    return nvme_present(0)||nvme_present(3)||nvme_present(4);
}
bool nvme_init(void) {
#ifdef MYOS_HARDWARE
    /* Live discovery happens only after the explicit privileged probe. */
    if(!boot_disk_root(nvme_boot()))return false;
#else
    return false;
#endif
    start(false);return nvme_present(0);
}
int nvme_installer_probe(void) {
    const struct boot_info *boot=nvme_boot();
    if(!boot_root_valid(boot) || boot_disk_root(boot))return -1;
    return start(true)?0:-1;
}
bool nvme_present(int u){return u>=0 && u<5 && namespaces[u].controller;}
u32 nvme_sectors(int u){return nvme_present(u)?namespaces[u].sectors:0;}
const char *nvme_model(int u){return nvme_present(u)?namespaces[u].controller->model:"";}
int nvme_read(int u,u32 lba,void *buf) {
    if(!nvme_present(u) || lba>=namespaces[u].sectors)return -1;
    struct nvme_namespace *ns=&namespaces[u];mutex_lock(&ns->controller->lock);
    int r=raw_io(ns->controller,ns->id,lba,buf,1,false);
    mutex_unlock(&ns->controller->lock);return r;
}
int nvme_write(int u,u32 lba,const void *buf,u32 count) {
    if(!nvme_present(u) || !count || count>32 || lba>=namespaces[u].sectors || count>namespaces[u].sectors-lba)return -1;
    struct nvme_namespace *ns=&namespaces[u];mutex_lock(&ns->controller->lock);int r=0;
    while(count) {
        u32 n=count>16?16:count;
        r=raw_io(ns->controller,ns->id,lba,(void *)buf,n,true);if(r)break;
        count-=n;lba+=n;buf=(const u8 *)buf+n*512;
    }
    mutex_unlock(&ns->controller->lock);return r;
}
int nvme_flush(int u) {
    if(!nvme_present(u))return -1;
    struct nvme_namespace *ns=&namespaces[u];mutex_lock(&ns->controller->lock);
    u32 command[16]={0};command[1]=ns->id; /* NVM Flush opcode 0 */
    int r=submit(ns->controller,1,command);
    mutex_unlock(&ns->controller->lock);return r;
}
void nvme_shutdown(void) {
    for(unsigned i=0;i<CONTROLLERS;i++) {
        struct nvme_controller *c=&controllers[i];
        if(!c->ready || c->failed)continue;
        mutex_lock(&c->lock);
        barrier();wr(c,CC,(rd(c,CC)&~(3u<<14))|(1u<<14));
        u32 start=timer_ticks();
        while(((rd(c,CSTS)>>2)&3)!=2) {
            if((rd(c,CSTS)&2) || (u32)(timer_ticks()-start)>=10u*TIMER_HZ) {
                quarantine(c,"shutdown failed");break;
            }
            task_sleep_ticks(1);
        }
        c->ready=false;mutex_unlock(&c->lock);
    }
}
