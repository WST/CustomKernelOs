#include "myos.h"
#include "gfx.h"
static void clock_line(const char *name,unsigned mhz) {
    if (mhz) printf("%s: %u MHz\n",name,mhz); else printf("%s: unknown\n",name);
}
int main(void) {
    struct system_info s;
    int r=sysinfo(&s,sizeof(s));
    if (r<0) { printf("sysinfo: error %d\n",r); return 1; }
    printf("%s %s (%s)\nCommit: %s\nBuilt: %s\n",s.os,s.version,s.arch,s.commit,s.build_time);
    printf("CPU: %s\nVendor: %s; family %u model %u stepping %u\n",s.cpu_brand,s.cpu_vendor,s.cpu_family,s.cpu_model,s.cpu_stepping);
    clock_line("CPU base (nominal)",s.cpu_base_mhz); clock_line("CPU maximum (reported)",s.cpu_max_mhz); clock_line("TSC",s.tsc_mhz);
    printf("CPUs online: %u; logical CPUs per package (reported): %u\n",s.cpu_online,s.cpu_logical_package);
    if (*s.hypervisor) printf("Hypervisor: %s\n",s.hypervisor);
    printf("CPU feature bits: EDX=%08x ECX=%08x\n",s.cpu_features_edx,s.cpu_features_ecx);
    printf("RAM usable: %u KB; used: %u KB; free: %u KB\nUptime: %u s\n",s.ram_total_kb,s.ram_used_kb,s.ram_free_kb,s.uptime_seconds);
    struct gfx_info2 g;
    if (gfx_info2(&g)==0) printf("Graphics: %s, %ux%u %u bpp, VRAM %u KB\n",g.driver,g.width,g.height,g.bpp,g.vram_kb);
    struct block_info disk;
    for (unsigned i=0; blockinfo(i,&disk)>0; i++)
        printf("Block: %s, %u sectors (%u MB)%s\n",disk.name,disk.sectors,disk.sectors/2048,disk.partition ? " partition" : "");
    struct net_info net;
    if (netinfo(&net)==0 && net.up)
        printf("Network: e1000, MAC %02x:%02x:%02x:%02x:%02x:%02x\n",net.mac[0],net.mac[1],net.mac[2],net.mac[3],net.mac[4],net.mac[5]);
    return 0;
}
