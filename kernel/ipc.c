/* Process-scoped IPC capabilities; only the kernel supplies sender identities. */
#include "ipc.h"
#include "task.h"
#include "paging.h"
#include "pmm.h"
#include "heap.h"
#include "string.h"
#include "timer.h"
#include "syscall.h"
#include "fb.h"
#define GRANT_MAX 16
struct grant {u32 pid,uid,epoch,rights;bool authenticated;struct grant *next;};
static bool live(struct task *t){return t && t->user && t->state!=TASK_ZOMBIE && !t->kill_pending;}
static bool matches(struct grant *g,struct task *t){return live(t) && g->pid==t->id && g->uid==t->uid && g->authenticated==t->authenticated && g->epoch==t->ipc_epoch;}
static void grants_free(struct grant **head){while(*head){struct grant *g=*head;*head=g->next;kfree(g);}}
static void grants_prune(struct grant **head){while(*head){struct grant *g=*head;struct task *t=task_process(task_by_id(g->pid));if(!matches(g,t)){*head=g->next;kfree(g);}else head=&g->next;}}
static int grants_set(struct grant **head,struct task *target,u32 rights) {
    grants_prune(head);struct grant **at=head;unsigned count=0;
    while(*at && (*at)->pid!=target->id){count++;at=&(*at)->next;}
    if(!rights){if(*at){struct grant *g=*at;*at=g->next;kfree(g);}return 0;}
    if(!*at){if(count>=GRANT_MAX)return -ENOSPC;*at=kcalloc(1,sizeof(**at));if(!*at)return -ENOMEM;}
    struct grant *g=*at;g->pid=target->id;g->uid=target->uid;g->epoch=target->ipc_epoch;g->authenticated=target->authenticated;g->rights=rights;return 0;
}
static u32 grants_rights(struct grant **head,struct task *t){grants_prune(head);for(struct grant *g=*head;g;g=g->next)if(matches(g,t))return g->rights;return 0;}

struct shm {bool used,owner_authenticated;u32 handle,npages,*frames,owner_pid,owner_uid,owner_epoch;int refs;struct grant *grants;};
static struct shm shms[SHM_MAX];
static struct shm *shm_find(int id){if(id<=0)return NULL;struct shm *s=&shms[((u32)id-1)%SHM_MAX];return s->used && s->handle==(u32)id?s:NULL;}
static bool shm_owner(struct shm *s,struct task *p){return s->owner_pid==p->id && s->owner_uid==p->uid && s->owner_authenticated==p->authenticated && s->owner_epoch==p->ipc_epoch;}
static void shm_destroy(struct shm *s){for(u32 i=0;i<s->npages;i++)pmm_free(s->frames[i]);kfree(s->frames);s->frames=NULL;grants_free(&s->grants);s->used=false;}
static void shm_put(struct shm *s){if(--s->refs==0)shm_destroy(s);}
int shm_alloc(u32 size) {
    if(!size || size>16*1024*1024)return -EINVAL;
    u32 n=(size+PAGE_SIZE-1)/PAGE_SIZE,*frames=kcalloc(n,sizeof(u32));if(!frames)return -ENOMEM;
    for(u32 i=0;i<n;i++){frames[i]=pmm_alloc();if(!frames[i]){for(u32 j=0;j<i;j++)pmm_free(frames[j]);kfree(frames);return -ENOMEM;}memset((void*)frames[i],0,PAGE_SIZE);}
    struct task *p=task_process(task_current());
    for(unsigned i=0;i<SHM_MAX;i++){struct shm *s=&shms[i];if(s->used || s->handle>0x7fffffffu-SHM_MAX)continue;
        s->handle=s->handle?s->handle+SHM_MAX:i+1;s->used=true;s->npages=n;s->frames=frames;s->refs=0;s->grants=NULL;
        s->owner_pid=p->id;s->owner_uid=p->uid;s->owner_authenticated=p->authenticated;s->owner_epoch=p->ipc_epoch;return s->handle;
    }
    for(u32 i=0;i<n;i++)pmm_free(frames[i]);
    kfree(frames);return -ENOMEM;
}
int shm_grant(int id,u32 pid,u32 rights) {
    if(rights!=0 && rights!=1 && rights!=3)return -EINVAL;
    struct shm *s=shm_find(id);if(!s)return -EINVAL;
    if(!shm_owner(s,task_process(task_current())))return -EPERM;
    struct task *to=task_process(task_by_id(pid));if(!live(to))return -ESRCH;
    return grants_set(&s->grants,to,rights);
}
int shm_discard(int id){struct shm *s=shm_find(id);if(!s)return -EINVAL;if(!shm_owner(s,task_process(task_current())))return -EPERM;if(s->refs)return -EBUSY;shm_destroy(s);return 0;}
static bool mapped(struct task *p,int id){for(int i=0;i<SHM_PER_TASK;i++)if(p->shm[i].id==id)return true;return false;}
int shm_size(int id){struct shm *s=shm_find(id);if(!s)return -EINVAL;struct task *p=task_process(task_current());if(!mapped(p,id) && !shm_owner(s,p) && !grants_rights(&s->grants,p))return -EACCES;return s->npages*PAGE_SIZE;}
int shm_map(int id,u32 *addr) {
    struct task *p=task_process(task_current());struct shm *s=shm_find(id);if(!s)return -EINVAL;
    int slot=-1;
    for(int i=0;i<SHM_PER_TASK;i++){if(p->shm[i].id==id){*addr=p->shm[i].addr;return 0;}if(!p->shm[i].id && slot<0)slot=i;}
    u32 rights=shm_owner(s,p)?3:grants_rights(&s->grants,p);if(!rights)return -EACCES;if(slot<0)return -EMFILE;
    u32 size=s->npages*PAGE_SIZE,va=0;
    for(int c=-1;c<SHM_PER_TASK && !va;c++){
        u32 cand=SHM_BASE;if(c>=0){if(!p->shm[c].id)continue;struct shm *prev=shm_find(p->shm[c].id);if(!prev)return -EIO;cand=p->shm[c].addr+prev->npages*PAGE_SIZE;}
        if(cand+size>SHM_END || cand+size<cand)continue;
        bool ok=true;
        for(int i=0;i<SHM_PER_TASK && ok;i++)if(p->shm[i].id){struct shm *other=shm_find(p->shm[i].id);if(!other)return -EIO;u32 a=p->shm[i].addr,e=a+other->npages*PAGE_SIZE;if(cand<e && a<cand+size)ok=false;}
        if(ok)va=cand;
    }
    if(!va)return -ENOMEM;
    for(u32 i=0;i<s->npages;i++)if(vmm_map_in(p->page_dir,va+i*PAGE_SIZE,s->frames[i],PG_USER|((rights&2)?PG_WRITE:0))){for(u32 j=0;j<i;j++)vmm_unmap_in(p->page_dir,va+j*PAGE_SIZE);return -ENOMEM;}
    s->refs++;p->shm[slot].id=id;p->shm[slot].addr=va;*addr=va;return 0;
}
static void unmap_slot(struct task *p,u32 dir,int i){struct shm *s=shm_find(p->shm[i].id);if(s){for(u32 n=0;n<s->npages;n++)vmm_unmap_in(dir,p->shm[i].addr+n*PAGE_SIZE);}p->shm[i].id=0;p->shm[i].addr=0;if(s)shm_put(s);}
int shm_unmap(u32 addr){struct task *p=task_process(task_current());for(int i=0;i<SHM_PER_TASK;i++)if(p->shm[i].id && p->shm[i].addr==addr){struct shm *s=shm_find(p->shm[i].id);if(!s)return -EINVAL;if(task_stack_busy(p->page_dir,addr,addr+s->npages*PAGE_SIZE))return -EBUSY;unmap_slot(p,p->page_dir,i);return 0;}return -EINVAL;}
void shm_release_all(struct task *p,u32 dir){for(int i=0;i<SHM_PER_TASK;i++)if(p->shm[i].id)unmap_slot(p,dir,i);for(int i=0;i<SHM_MAX;i++){struct shm *s=&shms[i];if(s->used && s->owner_pid==p->id){s->owner_pid=0;grants_free(&s->grants);if(!s->refs)shm_destroy(s);}}}

struct mbox {u8 msgs[MBOX_LEN][MSG_SIZE];int head,count;bool input_pending;struct grant *grants;};
static int mailbox(struct task *p){if(p->mbox)return 0;p->mbox=kcalloc(1,sizeof(struct mbox));return p->mbox?0:-ENOMEM;}
void ipc_identity_changed(struct task *p){p=task_process(p);p->ipc_epoch++;if(p->mbox){p->mbox->head=p->mbox->count=0;p->mbox->input_pending=false;grants_free(&p->mbox->grants);}}
int msg_allow(u32 pid,bool allow){struct task *p=task_process(task_current()),*from=task_process(task_by_id(pid));if(!live(from))return -ESRCH;int ret=mailbox(p);return ret?ret:grants_set(&p->mbox->grants,from,allow?1:0);}
void msg_input_notify(u32 pid){struct task *p=task_process(task_by_id(pid));if(!p || !p->mbox)return;p->mbox->input_pending=true;task_wakeup(p->mbox);}
int msg_send(u32 pid,const void *msg){struct task *p=task_process(task_current()),*to=task_process(task_by_id(pid));if(!live(to))return -ESRCH;
    if(p->user && !task_same_identity(p,to) && (!to->mbox || !grants_rights(&to->mbox->grants,p)))return -EPERM;
    int ret=mailbox(to);if(ret)return ret;struct mbox *m=to->mbox;u32 flags=irq_save();
    if(m->count==MBOX_LEN){irq_restore(flags);return -EAGAIN;}u8 *slot=m->msgs[(m->head+m->count)%MBOX_LEN];memcpy(slot,msg,MSG_SIZE);*(u32*)slot=p->id;m->count++;irq_restore(flags);task_wakeup(m);return 0;
}
int msg_recv(void *msg,int timeout_ms){struct task *p=task_process(task_current());int ret=mailbox(p);if(ret)return ret;struct mbox *m=p->mbox;u32 deadline=timer_ticks()+(timeout_ms>0?((u32)timeout_ms+9)/10:0);
    for(;;){u32 flags=irq_save();if(m->count){memcpy(msg,m->msgs[m->head],MSG_SIZE);m->head=(m->head+1)%MBOX_LEN;m->count--;irq_restore(flags);return 0;}
        if(m->input_pending){m->input_pending=false;irq_restore(flags);return -EAGAIN;}if(!timeout_ms){irq_restore(flags);return -EAGAIN;}
        if(timeout_ms>0){i32 left=(i32)(deadline-timer_ticks());if(left<=0){irq_restore(flags);return -ETIMEDOUT;}task_wait_timeout(m,left);}else task_wait(m);
        irq_restore(flags);if(task_current()->kill_pending)return -EINTR;
    }
}
void msg_free(struct task *t){if(t->mbox)grants_free(&t->mbox->grants);kfree(t->mbox);t->mbox=NULL;}
#define SVC_MAX 8
static struct {char name[16];u32 pid;} svcs[SVC_MAX];
int svc_register(const char *name){struct task *p=task_process(task_current());if(!*name)return -EINVAL;
    if(!p->authenticated && (strcmp(name,"desktop") || !fb_is_owner(p->id)))return -EPERM;
    int slot=-1;
    for(int i=0;i<SVC_MAX;i++){struct task *owner=task_process(task_by_id(svcs[i].pid));if(!live(owner)){svcs[i].pid=0;if(slot<0)slot=i;continue;}
        if(!strcmp(name,svcs[i].name) && task_same_identity(p,owner)){if(owner!=p)return -EEXIST;return 0;}
    }
    if(slot<0)return -ENOMEM;
    strcpy(svcs[slot].name,name);svcs[slot].pid=p->id;return 0;
}
int svc_find(const char *name){struct task *p=task_process(task_current());for(int i=0;i<SVC_MAX;i++)if(svcs[i].pid && !strcmp(name,svcs[i].name)){struct task *owner=task_process(task_by_id(svcs[i].pid));if(!live(owner)){svcs[i].pid=0;continue;}if(task_same_identity(p,owner))return owner->id;}return 0;}

/* ------------------------------------------------------------ часы (CMOS RTC) */

static u8 cmos(u8 reg) { outb(0x70, reg); return inb(0x71); }

void rtc_read(struct rtc_time *t) {
    u8 s, m, h, d, mo, y, b;
    u32 f = irq_save();
    while (cmos(0x0A) & 0x80) {}                    /* не читать во время обновления */
    s = cmos(0x00); m = cmos(0x02); h = cmos(0x04);
    d = cmos(0x07); mo = cmos(0x08); y = cmos(0x09);
    b = cmos(0x0B);
    irq_restore(f);
    if (!(b & 0x04)) {                              /* числа в двоично-десятичном виде */
        #define BCD(x) (((x) & 0x0F) + ((x) >> 4) * 10)
        s = BCD(s); m = BCD(m); h = BCD(h & 0x7F) | (h & 0x80); d = BCD(d); mo = BCD(mo); y = BCD(y);
    }
    if (!(b & 0x02) && (h & 0x80)) h = ((h & 0x7F) + 12) % 24;   /* 12-часовой формат */
    t->year = 2000 + y; t->month = mo; t->day = d;
    t->hour = h & 0x7F; t->min = m; t->sec = s;
}
