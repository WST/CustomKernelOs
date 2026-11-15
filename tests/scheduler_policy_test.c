/* Exercise the real queues/policy without interrupt/FPU assembly. */
#define HOST_TEST
#include "../kernel/scheduler.c"
int printf(const char *,...);int puts(const char *);void exit(int);
#define CHECK(c) do { if(!(c)) { printf("SCHED POLICY FAIL line %d: %s\n",__LINE__,#c);exit(1); } } while(0)
struct cpu_state cpu_states[SMP_MAX_CPUS];
static struct task pool[32],idles[SMP_MAX_CPUS];
static struct regs frames[32];
static u32 now,ncpu=4,cpu;
u32 timer_ticks(void){return now;}
u32 smp_count(void){return ncpu;}
u32 smp_cpu(void){return cpu;}
bool smp_task_active(struct task *t){for(u32 i=0;i<ncpu;i++)if(cpu_states[i].running==t)return true;return false;}
void smp_wake(u32 target){cpu_states[target].resched=true;}
static void reset(void) {
    memset(queues,0,sizeof(queues));memset(stats,0,sizeof(stats));memset(pool,0,sizeof(pool));
    memset(cpu_states,0,sizeof(cpu_states));memset(frames,0,sizeof(frames));memset(idles,0,sizeof(idles));
    placement_cursor=last_maintenance=now=cpu=0;ncpu=4;
    for(u32 i=0;i<ncpu;i++){idles[i].cpu=i;idles[i].cpu_idle=true;idles[i].state=TASK_RUNNING;cpu_states[i].idle=cpu_states[i].running=&idles[i];}
}
static struct task *task(unsigned index,u32 owner,u32 level) {
    struct task *t=&pool[index];t->id=index+1;t->cpu=owner;t->user=true;t->state=TASK_READY;
    frames[index].cs=0x1b;t->esp=(u32)&frames[index];scheduler_init_task(t,0);t->sched_level=level;
    scheduler_enqueue(t,true);return t;
}
static void invariant(void) {
    unsigned seen[32]={0};
    for(u32 c=0;c<ncpu;c++)for(u32 level=0;level<SCHED_LEVELS;level++) {
        struct run_queue *q=&queues[c][level];struct task *previous=0;unsigned count=0;
        for(struct task *t=q->head;t;t=t->rq_next) {
            CHECK(t>=pool && t<pool+32 && !seen[t-pool]++);
            CHECK(t->rq_queued && t->rq_prev==previous && t->cpu==c && t->sched_level==level && t->state==TASK_READY);
            previous=t;CHECK(++count<=32);
        }
        CHECK(q->tail==previous && q->count==count);
    }
    for(unsigned i=0;i<32;i++)CHECK((bool)seen[i]==pool[i].rq_queued);
}
int main(void) {
    reset();struct task *a=task(0,0,1),*b=task(1,0,1),*c=task(2,0,0);
    scheduler_enqueue(a,true);invariant();CHECK(scheduler_pick(0)==c);CHECK(scheduler_pick(0)==a);CHECK(scheduler_pick(0)==b);
    scheduler_remove(a);invariant();
    reset();a=task(0,0,1);scheduler_remove(a);scheduler_dispatch(&idles[0],a);
    CHECK(a->sched_left==2);scheduler_charge(a);CHECK(a->sched_used==1 && a->sched_left==1);
    for(int i=0;i<10;i++){scheduler_enqueue(a,true);CHECK(scheduler_pick(0)==a);scheduler_dispatch(a,a);}
    CHECK(a->sched_used==1 && a->sched_left==1);
    scheduler_charge(a);scheduler_charge(a);scheduler_charge(a);CHECK(a->sched_level==2 && !a->sched_used);
    scheduler_dispatch(a,a);CHECK(a->sched_left==4);
    struct task child={0};scheduler_init_task(&child,a);CHECK(child.sched_level==2);
    reset();a=task(0,0,2);scheduler_remove(a);a->sched_used=5;now=10;scheduler_block(a);a->state=TASK_BLOCKED;
    now=11;scheduler_wake(a);CHECK(a->sched_level==2 && a->sched_used==5);scheduler_remove(a);
    scheduler_block(a);a->state=TASK_BLOCKED;now=13;scheduler_wake(a);CHECK(!a->sched_level && !a->sched_used);scheduler_remove(a);
    a->sched_level=2;a->sched_used=5;a->sched_active=5;scheduler_block(a);a->state=TASK_BLOCKED;now=15;scheduler_wake(a);
    CHECK(a->sched_level==2 && a->sched_used==5);scheduler_remove(a);
    scheduler_block(a);a->state=TASK_SLEEPING;now=25;scheduler_wake(a);CHECK(a->sched_level==0);invariant();
    scheduler_remove(a);a->sched_level=2;a->sched_active=1;scheduler_block(a);a->state=TASK_BLOCKED;now=28;
    scheduler_wake(a);CHECK(a->sched_level==0);invariant();
    reset();a=task(0,0,2);now=100;scheduler_maintenance();CHECK(a->sched_level==1 && a->ready_since==0);
    now=110;scheduler_maintenance();CHECK(a->sched_level==1);now=200;scheduler_maintenance();CHECK(!a->sched_level);invariant();
    reset();a=task(0,0,1);CHECK(scheduler_choose_cpu()==1);b=task(1,1,1);CHECK(scheduler_choose_cpu()==2);
    reset();a=task(0,0,1);b=task(1,0,1);b->user=false;
    c=task(2,0,1);c->in_syscall=true;struct mutex lock={0};task(3,0,1)->holding=&lock;
    task(4,0,1)->kill_pending=true;struct task *active=task(5,0,1);cpu_states[0].running=active;
    task(6,0,1)->user_context=false;
    CHECK(scheduler_pick(1)==a && a->cpu==1 && stats[0].migrations_out==1);
    CHECK(scheduler_pick(2)==&idles[2]);CHECK(b->cpu==0 && c->cpu==0 && active->cpu==0);invariant();
    struct task *movable=task(7,0,2);now=20;scheduler_maintenance();CHECK(movable->cpu!=0);invariant();
    for(unsigned i=0;i<32;i++){scheduler_remove(&pool[i]);scheduler_remove(&pool[i]);}invariant();
    puts("Scheduler policy: FIFO/priority, CPU budgets, wake boosts, aging, placement, migration guards and queue integrity: PASS");
    return 0;
}
