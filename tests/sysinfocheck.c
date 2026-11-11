#include "myos.h"
static int failures;
#define CHECK(x) do { if (!(x)) { printf("SYSINFO FAIL: %s\n",#x); failures++; } } while(0)
int main(void) {
    struct { struct system_info info; unsigned guard; } b;
    memset(&b,0,sizeof(b)); b.guard=0x12345678;
    CHECK(sysinfo(&b.info,sizeof(b))==0);
    CHECK(b.guard==0x12345678);
    CHECK(b.info.abi_version==MYOS_SYSINFO_ABI);
    CHECK(b.info.size==sizeof(b.info));
    CHECK(!strcmp(b.info.os,"MyOS"));
    CHECK(b.info.version[0] && b.info.commit[0] && b.info.build_time[0]);
    CHECK(b.info.cpu_brand[0]);
    CHECK(b.info.cpu_online==1);
    CHECK(b.info.ram_total_kb>0 && b.info.ram_used_kb>0);
    CHECK(b.info.ram_used_kb+b.info.ram_free_kb==b.info.ram_total_kb);
    CHECK(sysinfo(&b.info,sizeof(b.info)-1)==-EINVAL);
    CHECK(sysinfo(0,sizeof(b.info))==-EFAULT);
    CHECK(sysinfo((struct system_info *)0x1000,sizeof(b.info))==-EFAULT);
    CHECK(sysinfo((struct system_info *)0xbffffff0,sizeof(b.info))==-EFAULT);
    CHECK(sysinfo((struct system_info *)"read only",sizeof(b.info))==-EFAULT);
    if (failures) return 1;
    puts("SYSINFO ABI PASS"); return 0;
}
