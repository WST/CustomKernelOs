#ifndef MYOS_KERNELINFO_H
#define MYOS_KERNELINFO_H
#define KERNELINFO_ABI 1u
struct kernel_info {
    unsigned abi,size,cpus,bkl_acquires,bkl_contended;
    unsigned fast_calls,leaf_sections,parallel_peak,overlap_with_bkl;
};
/* Size in bytes; returns 0 or negative errno. Counters wrap modulo 2^32.
 * Each field is atomic; the multi-field snapshot is observational, not a
 * globally frozen sample. Peak counts concurrent bounded leaf/fast entries. */
int kernelinfo(struct kernel_info *out,unsigned size);
#endif
