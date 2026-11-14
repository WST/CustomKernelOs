#include "myos.h"
int main(void) {
    struct kernel_info s;
    if(kernelinfo(&s,sizeof(s))<0) { puts("kernelstat: snapshot failed");return 1; }
    printf("CPUs: %u; BKL acquisitions: %u; contended: %u\n",s.cpus,s.bkl_acquires,s.bkl_contended);
    printf("Fast syscalls: %u; leaf sections: %u; parallel peak: %u; overlap with BKL: %u\n",
        s.fast_calls,s.leaf_sections,s.parallel_peak,s.overlap_with_bkl);
    puts("Counters since boot; filesystem, VM and scheduling still use the BKL.");return 0;
}
