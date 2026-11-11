#ifndef SMP_H
#define SMP_H
#include "types.h"
#define SMP_MAX_CPUS 8
struct regs;
struct task;
struct cpu_state {
    u32 index; /* GS:0; keep first for constant-time CPU lookup */
    u32 tlb_generation;
    struct task *running, *idle;
    u32 slice, nesting;
    u32 total_ticks, idle_ticks; /* Per-logical-CPU scheduler accounting. */
    bool resched;
};
extern struct cpu_state cpu_states[SMP_MAX_CPUS];
u32 smp_cpu(void);
u32 smp_count(void);
void smp_init(void);
void smp_early_init(void); /* before paging: BIOS data area is in page zero */
void smp_enter(void);
void smp_user_tlb_flush(u32 dir);
void smp_kernel_mapping_changed(void);
void smp_leave(struct regs *r);
void smp_idle(void);
void smp_tick(void);
void smp_wake(u32 cpu);
void smp_eoi(void);
bool smp_task_active(struct task *t);
#endif
