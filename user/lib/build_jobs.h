#ifndef MYOS_BUILD_JOBS_H
#define MYOS_BUILD_JOBS_H
/* Auto mode is bounded by online CPUs and a 64 MiB/job free-RAM estimate.
 * Explicit limits are deliberate overrides; they never create unbounded jobs. */
#define BUILD_JOBS_MAX 16
static inline int build_jobs_parse(const char *s) {
    if(!strcmp(s,"auto")) return 0;
    unsigned n=0;
    if(!*s) return -1;
    for(;*s;s++) {
        if(*s<'0' || *s>'9') return -1;
        n=n*10+(*s-'0'); if(n>BUILD_JOBS_MAX) return -1;
    }
    return n?(int)n:-1;
}
static inline int build_jobs_for_info(const struct system_info *info) {
    unsigned count=info->cpu_online?info->cpu_online:1,memory=info->ram_free_kb/65536u;
    if(count>BUILD_JOBS_MAX) count=BUILD_JOBS_MAX;
    if(!memory) memory=1;
    return count<memory?count:memory;
}
static inline int build_jobs_auto(void) {
    struct system_info info;
    return sysinfo(&info,sizeof(info))<0?1:build_jobs_for_info(&info);
}
#endif
