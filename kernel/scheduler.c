/* MLFQ: FIFO per-CPU ready queues, persistent CPU allotments, bounded wake
 * boosts, aging, and migration of fully saved ring-3 contexts only. */
#include "scheduler.h"
#include "timer.h"
#include "string.h"

#define AGE_TICKS 100u
#define BOOST_INTERVAL 20u
#define BALANCE_INTERVAL 10u
struct run_queue { struct task *head,*tail; u32 count; };
static struct run_queue queues[SMP_MAX_CPUS][SCHED_LEVELS];
static struct scheduler_cpu_info stats[SMP_MAX_CPUS];
static u32 placement_cursor,last_maintenance;
static const u32 quantum[SCHED_LEVELS]={1,2,4};
static const u32 allotment[SCHED_LEVELS]={2,4,8};

void scheduler_init_task(struct task *t,const struct task *parent) {
    t->sched_level=1; t->sched_used=0; t->sched_left=0; t->sched_active=0;
    if(parent && parent->user && parent->sched_level>=1) {
        t->sched_level=parent->sched_level; t->sched_used=parent->sched_used;
    }
    t->rq_prev=t->rq_next=NULL;t->rq_queued=false;
    t->last_boost=timer_ticks()-BOOST_INTERVAL;
    t->user_context=t->user && t->esp && (((struct regs *)t->esp)->cs&3);
}
void scheduler_remove(struct task *t) {
    if(!t || !t->rq_queued) return;
    struct run_queue *q=&queues[t->cpu][t->sched_level];
    if(t->rq_prev)t->rq_prev->rq_next=t->rq_next;else q->head=t->rq_next;
    if(t->rq_next)t->rq_next->rq_prev=t->rq_prev;else q->tail=t->rq_prev;
    q->count--;t->rq_prev=t->rq_next=NULL;t->rq_queued=false;
}
void scheduler_enqueue(struct task *t,bool fresh) {
    if(!t || t->rq_queued || t->cpu_idle || t->state!=TASK_READY) return;
    struct run_queue *q=&queues[t->cpu][t->sched_level];
    if(fresh)t->ready_since=t->age_since=timer_ticks();
    t->rq_prev=q->tail;t->rq_next=NULL;
    if(q->tail)q->tail->rq_next=t;else q->head=t;
    q->tail=t;q->count++;t->rq_queued=true;
}
static u32 ready_count(u32 cpu) {
    u32 n=0;for(u32 level=0;level<SCHED_LEVELS;level++)n+=queues[cpu][level].count;
    return n;
}
static u32 load(u32 cpu) {
    struct task *t=cpu_states[cpu].running;
    return ready_count(cpu)+(t && !t->cpu_idle && t->state==TASK_RUNNING?1:0);
}
u32 scheduler_choose_cpu(void) {
    u32 count=smp_count(),best=placement_cursor%count,best_load=0xffffffffu;
    for(u32 i=0;i<count;i++) {
        u32 cpu=(placement_cursor+i)%count,score=load(cpu);
        if(score<best_load) { best=cpu;best_load=score; }
    }
    placement_cursor=(best+1)%count;return best;
}
void scheduler_block(struct task *t) {
    scheduler_remove(t);t->blocked_since=timer_ticks();
}
void scheduler_wake(struct task *t) {
    if(t->state!=TASK_BLOCKED && t->state!=TASK_SLEEPING)return;
    u32 now=timer_ticks();
    u32 waited=now-t->blocked_since;
    /* Frequent wakeups remain interactive when the preceding CPU burst was
     * no longer than the wait. CPU-heavy bursts retain the boost cooldown.
     * Sub-20ms waits and yield never reset the measured burst or allotment. */
    if(waited>=10 || (waited>=2 &&
       (waited>=t->sched_active || now-t->last_boost>=BOOST_INTERVAL))) {
        t->sched_level=0;t->sched_used=0;t->sched_left=0;t->last_boost=now;
    }
    if(waited>=2)t->sched_active=0;
    t->state=TASK_READY;scheduler_enqueue(t,true);stats[t->cpu].wakeups++;
    smp_wake(t->cpu);
}
void scheduler_charge(struct task *t) {
    if(t->cpu_idle)return;
    t->sched_active++;
    if(t->sched_left)t->sched_left--;
    if(++t->sched_used>=allotment[t->sched_level]) {
        if(t->sched_level+1<SCHED_LEVELS)t->sched_level++;
        t->sched_used=0;t->sched_left=0;
    }
}
static bool migratable(const struct task *t) {
    return t->user && !t->cpu_idle && t->state==TASK_READY && t->user_context &&
           !t->in_syscall && !t->holding && !t->kill_pending && !smp_task_active((struct task *)t);
}
static struct task *migration_candidate(u32 cpu) {
    for(int level=SCHED_LEVELS-1;level>=0;level--)
        for(struct task *t=queues[cpu][level].tail;t;t=t->rq_prev)if(migratable(t))return t;
    return NULL;
}
static void move_task(struct task *t,u32 cpu) {
    u32 old=t->cpu;scheduler_remove(t);t->cpu=cpu;
    stats[old].migrations_out++;stats[cpu].migrations_in++;
}
struct task *scheduler_pick(u32 cpu) {
    for(u32 level=0;level<SCHED_LEVELS;level++) {
        struct task *t=queues[cpu][level].head;
        if(t){scheduler_remove(t);return t;}
    }
    /* Idle CPUs steal saved user contexts, never active/kernel continuations. */
    bool tried[SMP_MAX_CPUS]={false};tried[cpu]=true;
    for(u32 pass=1;pass<smp_count();pass++) {
        u32 victim=cpu,best=0;
        for(u32 i=0;i<smp_count();i++)if(!tried[i] && ready_count(i)>best){victim=i;best=ready_count(i);}
        if(victim==cpu)break;
        tried[victim]=true;
        struct task *t=migration_candidate(victim);
        if(t){move_task(t,cpu);return t;}
    }
    return cpu_states[cpu].idle;
}
void scheduler_maintenance(void) {
    u32 now=timer_ticks();
    if(now-last_maintenance<BALANCE_INTERVAL)return;
    last_maintenance=now;
    for(u32 cpu=0;cpu<smp_count();cpu++)for(u32 level=1;level<SCHED_LEVELS;level++) {
        struct task *t=queues[cpu][level].head;
        while(t) {
            struct task *next=t->rq_next;
            if(now-t->age_since>=AGE_TICKS) {
                scheduler_remove(t);t->sched_level--;t->sched_used=t->sched_left=0;t->age_since=now;
                scheduler_enqueue(t,false);smp_wake(cpu);
            }
            t=next;
        }
    }
    u32 low=0,high=0;
    for(u32 cpu=1;cpu<smp_count();cpu++){if(load(cpu)<load(low))low=cpu;if(load(cpu)>load(high))high=cpu;}
    if(load(high)>load(low)+1) {
        struct task *t=migration_candidate(high);
        if(t){move_task(t,low);scheduler_enqueue(t,false);smp_wake(low);}
    }
}
bool scheduler_preempt(u32 cpu,const struct task *running) {
    if(!running || running->cpu_idle)return ready_count(cpu)!=0;
    for(u32 level=0;level<running->sched_level;level++)if(queues[cpu][level].count)return true;
    return false;
}
void scheduler_dispatch(struct task *previous,struct task *next) {
    u32 cpu=smp_cpu();
    if(previous!=next)stats[cpu].switches++;
    if(next->cpu_idle)return;
    u32 wait=timer_ticks()-next->ready_since,bucket=0;
    static const u32 bounds[SCHED_WAIT_BUCKETS-1]={0,1,2,4,8,16,32};
    while(bucket<SCHED_WAIT_BUCKETS-1 && wait>bounds[bucket])bucket++;
    stats[cpu].dispatches++;stats[cpu].wait_ticks_total+=wait;stats[cpu].wait_hist[bucket]++;
    if(wait>stats[cpu].wait_ticks_max)stats[cpu].wait_ticks_max=wait;
    if(!next->sched_left)next->sched_left=quantum[next->sched_level];
}
void scheduler_snapshot(struct scheduler_cpu_info *out) {
    for(u32 cpu=0;cpu<smp_count();cpu++) {
        out[cpu]=stats[cpu];out[cpu].cpu=cpu;
        struct task *t=cpu_states[cpu].running;
        out[cpu].running_pid=t?t->id:0;
        out[cpu].running_priority=t && !t->cpu_idle?t->sched_level:SCHED_LEVELS;
        for(u32 level=0;level<SCHED_LEVELS;level++)out[cpu].ready[level]=queues[cpu][level].count;
    }
}
