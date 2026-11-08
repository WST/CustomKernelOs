#include "sysinfo.h"
#include "string.h"
#include "pmm.h"
#include "timer.h"
#include "vga.h"
#include "build_info.h"

static struct system_info identity;
static void cpuid(u32 leaf, u32 sub, u32 r[4]) {
    __asm__ volatile("cpuid" : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3]) : "a"(leaf), "c"(sub));
}
static bool have_cpuid(void) {
    u32 before, after;
    __asm__ volatile("pushfl; popl %0" : "=r"(before));
    u32 changed = before ^ (1u << 21);
    __asm__ volatile("pushl %0; popfl; pushfl; popl %1; pushl %2; popfl"
                     : "+r"(changed), "=&r"(after) : "r"(before) : "cc", "memory");
    return ((before ^ after) & (1u << 21)) != 0;
}
/* Freestanding kernel: avoid a dependency on libgcc __udivdi3. */
static u64 divide64(u64 n, u64 d) {
    u64 q=0, rem=0;
    for (int i=63; i>=0; i--) {
        rem=(rem<<1)|((n>>i)&1);
        if (rem>=d) { rem-=d; q|=(u64)1<<i; }
    }
    return q;
}
void sysinfo_init(void) {
    struct system_info *s = &identity;
    memset(s, 0, sizeof(*s));
    s->abi_version = MYOS_SYSINFO_ABI; s->size = sizeof(*s);
    strcpy(s->os, "MyOS"); strcpy(s->version, MYOS_VERSION);
    strcpy(s->commit, MYOS_COMMIT); strcpy(s->build_time, MYOS_BUILD_TIME);
    strcpy(s->arch, "i386"); strcpy(s->cpu_brand, "unknown");
    strcpy(s->cpu_vendor, "unknown");
    s->cpu_online = 1; /* Current scheduler is uniprocessor. */
    if (!have_cpuid()) return;
    u32 r[4]; cpuid(0, 0, r); u32 max = r[0];
    memcpy(s->cpu_vendor, &r[1], 4); memcpy(s->cpu_vendor+4, &r[3], 4); memcpy(s->cpu_vendor+8, &r[2], 4);
    if (max >= 1) {
        cpuid(1, 0, r);
        u32 family = (r[0] >> 8) & 15;
        s->cpu_family = family + (family == 15 ? (r[0] >> 20) & 255 : 0);
        s->cpu_model = ((r[0] >> 4) & 15) | ((family == 6 || family == 15) ? (r[0] >> 12) & 240 : 0);
        s->cpu_stepping = r[0] & 15;
        s->cpu_features_edx = r[3]; s->cpu_features_ecx = r[2];
        s->cpu_logical_package = r[3] & (1u << 28) ? (r[1] >> 16) & 255 : 1;
        if (r[2] & (1u << 31)) {
            cpuid(0x40000000, 0, r);
            memcpy(s->hypervisor, &r[1], 4); memcpy(s->hypervisor+4, &r[2], 4); memcpy(s->hypervisor+8, &r[3], 4);
        }
    }
    cpuid(0x80000000, 0, r);
    if (r[0] >= 0x80000004) {
        char brand[49] = {0};
        for (u32 i=0; i<3; i++) { cpuid(0x80000002+i, 0, r); memcpy(brand+i*16, r, 16); }
        char *b=brand; while (*b==' ') b++;
        u32 len=strlen(b); while (len && b[len-1]==' ') b[--len]=0;
        if (*b) strcpy(s->cpu_brand, b);
    }
    /* Intel SDM CPUID.16H reports nominal base/max, never live frequency. */
    if (!strcmp(s->cpu_vendor, "GenuineIntel") && max >= 0x16) {
        cpuid(0x16, 0, r); s->cpu_base_mhz=r[0]&65535; s->cpu_max_mhz=r[1]&65535;
    }
    if (!strcmp(s->cpu_vendor, "GenuineIntel") && max >= 0x15) {
        cpuid(0x15, 0, r);
        if (r[0] && r[1] && r[2]) {
            u64 mhz=divide64((u64)r[2]*r[1], (u64)r[0]*1000000);
            if (mhz <= 0xffffffffu) s->tsc_mhz=(u32)mhz;
        }
    }
}
void sysinfo_get(struct system_info *out) {
    *out=identity;
    struct pmm_stats mem;
    u32 flags=irq_save(); pmm_stats(&mem); irq_restore(flags);
    out->ram_total_kb=mem.total_frames*4;
    out->ram_used_kb=mem.used_frames*4;
    out->ram_free_kb=(mem.total_frames-mem.used_frames)*4;
    out->uptime_seconds=timer_ticks()/TIMER_HZ;
}
void sysinfo_print(void) {
    struct system_info s; sysinfo_get(&s);
    kprintf("%s %s (%s)\nBuild: %s; %s\nCPU: %s [%s]\n",s.os,s.version,s.arch,s.commit,s.build_time,s.cpu_brand,s.cpu_vendor);
    if (s.cpu_base_mhz) kprintf("CPU base: %u MHz\n",s.cpu_base_mhz);
    else kprintf("CPU base: unknown\n");
    kprintf("Online CPUs: %u; RAM: %u KB usable, %u KB free\n",s.cpu_online,s.ram_total_kb,s.ram_free_kb);
}
