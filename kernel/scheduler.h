#ifndef KERNEL_SCHEDULER_H
#define KERNEL_SCHEDULER_H
#include "task.h"
/* Serialized by the existing BKL; callers also disable local interrupts. */
void scheduler_init_task(struct task *t,const struct task *parent);
void scheduler_enqueue(struct task *t,bool fresh);
void scheduler_remove(struct task *t);
void scheduler_wake(struct task *t);
void scheduler_block(struct task *t);
void scheduler_charge(struct task *t);
struct task *scheduler_pick(u32 cpu);
u32 scheduler_choose_cpu(void);
void scheduler_maintenance(void);
void scheduler_dispatch(struct task *previous,struct task *next);
bool scheduler_preempt(u32 cpu,const struct task *running);
void scheduler_snapshot(struct scheduler_cpu_info *out);
#endif
