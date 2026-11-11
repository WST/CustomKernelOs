/* Exercise the real syscall boundary in ring 3. */
#include "myos.h"
#define CHECK(x) do { if(!(x)) { printf("klog syscall check failed at %d\n",__LINE__);return 1; } } while(0)
static char first[KLOG_CAPACITY],second[KLOG_CAPACITY];
int main(void) {
    struct dirent_info st;
    CHECK(dirstat("/var/log/kernel.log",&st)==-ENOENT);
    struct klog_cursor a={.flags=KLOG_OLDEST},b=a;
    int n=klog_read(first,sizeof(first),&a);CHECK(n>0);
    int m=klog_read(second,sizeof(second),&b);CHECK(m>=n && !memcmp(first,second,n));
    CHECK(klog_read(NULL,1,&a)==-1 && errno==EFAULT);
    CHECK(klog_read(first,1,NULL)==-1 && errno==EFAULT);
    CHECK(klog_read(first,KLOG_CAPACITY+1,&a)==-1 && errno==EINVAL);
    a.flags=3;CHECK(klog_read(first,1,&a)==-1 && errno==EINVAL);
    a=(struct klog_cursor){.lo=~0u,.hi=~0u};CHECK(klog_read(first,1,&a)==-1 && errno==EINVAL);
    a=(struct klog_cursor){.flags=KLOG_END};CHECK(klog_read(NULL,0,&a)==0);
    CHECK(dirstat("/var/log/kernel.log",&st)==-ENOENT);
    printf("Kernel log syscall: independent snapshots and invalid pointer/length/cursor checks: PASS\n");return 0;
}
