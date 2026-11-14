#ifndef MYOS_SCHEDULER_ABI_H
#define MYOS_SCHEDULER_ABI_H
#define SCHED_LEVELS 3u
#define SCHED_WAIT_BUCKETS 8u
/* Read-only snapshot; counters wrap modulo 2^32. Wait histogram upper bounds
 * in 10ms ticks: 0, 1, 2, 4, 8, 16, 32, then overflow. Priority 3 is idle. */
struct scheduler_cpu_info {
    unsigned cpu,running_pid,running_priority,ready[SCHED_LEVELS];
    unsigned switches,dispatches,migrations_in,migrations_out,wakeups;
    unsigned wait_ticks_total,wait_ticks_max,wait_hist[SCHED_WAIT_BUCKETS];
};
int schedinfo(struct scheduler_cpu_info *out,unsigned capacity);
#endif
