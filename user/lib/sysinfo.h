#ifndef MYOS_SYSINFO_H
#define MYOS_SYSINFO_H
/* ABI v1: fixed-width 32-bit fields; zero means unavailable for CPU clocks.
 * cpu_logical_package is CPUID capacity, NOT the online CPU count.
 * ram_* is PMM-managed usable memory, not installed DIMM capacity. */
#define MYOS_SYSINFO_ABI 1
struct system_info {
    unsigned abi_version, size;
    char os[16], version[32], commit[64], build_time[32], arch[16];
    char cpu_vendor[16], cpu_brand[64], hypervisor[16];
    unsigned cpu_family, cpu_model, cpu_stepping;
    unsigned cpu_features_edx, cpu_features_ecx;
    unsigned cpu_base_mhz, cpu_max_mhz, tsc_mhz;
    unsigned cpu_logical_package, cpu_online;
    unsigned ram_total_kb, ram_used_kb, ram_free_kb, uptime_seconds;
};
#endif
