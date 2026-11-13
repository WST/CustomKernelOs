#define sysinfo policy_sysinfo
#include "myos.h"
#include "build_jobs.h"
static struct system_info sample;
static int available;
int policy_sysinfo(struct system_info *out,unsigned size) {
    (void)size; if(!available) return -ENOSYS; *out=sample; return 0;
}
#define CHECK(c) do { if(!(c)) { printf("BUILD JOBS FAIL line %u\n",__LINE__); return 1; } } while(0)
int main(void) {
    CHECK(build_jobs_parse("auto")==0 && build_jobs_parse("1")==1 && build_jobs_parse("16")==16);
    CHECK(build_jobs_parse("0")<0 && build_jobs_parse("17")<0 && build_jobs_parse("99999999999999")<0);
    CHECK(build_jobs_parse("")<0 && build_jobs_parse("4x")<0 && build_jobs_parse("-1")<0);
    CHECK(build_jobs_auto()==1); available=1;sample.cpu_online=8;sample.ram_free_kb=512*1024;
    CHECK(build_jobs_auto()==8);sample.ram_free_kb=130*1024;CHECK(build_jobs_auto()==2);
    sample.ram_free_kb=1024;CHECK(build_jobs_auto()==1);
    sample.cpu_online=64;sample.ram_free_kb=0xffffffffu;CHECK(build_jobs_auto()==16);
    puts("Build jobs: CPU/RAM auto limits, explicit bounds and syscall fallback: PASS"); return 0;
}
