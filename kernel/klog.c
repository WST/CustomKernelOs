#include "klog.h"
#include "syscall.h"
#include "string.h"

static char ring[KLOG_CAPACITY];
static u64 end_sequence;
static volatile u32 guard;

static u32 lock(void) {
    u32 flags=irq_save();
    while(__sync_lock_test_and_set(&guard,1))__asm__ volatile("pause");
    return flags;
}
static void unlock(u32 flags) { __sync_lock_release(&guard);irq_restore(flags); }
void klog_write(const char *text,u32 length) {
    u32 flags=lock();
    for(u32 i=0;i<length;i++)ring[end_sequence++%KLOG_CAPACITY]=text[i];
    unlock(flags);
}
/* Trusted kernel buffers only; the syscall copies through a bounce buffer so
 * no userspace page faults or filesystem I/O occur while holding the lock. */
int klog_read(void *buffer,u32 capacity,struct klog_cursor *cursor) {
    if(!cursor || capacity>KLOG_CAPACITY || (capacity && !buffer) ||
        cursor->flags>KLOG_END)return -EINVAL;
    u32 flags=lock();
    u64 first=end_sequence>KLOG_CAPACITY?end_sequence-KLOG_CAPACITY:0;
    u64 pos=((u64)cursor->hi<<32)|cursor->lo;
    if(cursor->flags==KLOG_OLDEST)pos=first;
    else if(cursor->flags==KLOG_END)pos=end_sequence;
    if(pos>end_sequence) { unlock(flags);return -EINVAL; }
    u64 skipped=pos<first?first-pos:0;
    if(pos<first)pos=first;
    u32 count=end_sequence-pos>capacity?capacity:(u32)(end_sequence-pos);
    for(u32 i=0;i<count;i++)((char *)buffer)[i]=ring[(pos+i)%KLOG_CAPACITY];
    pos+=count;
    cursor->lo=(u32)pos;cursor->hi=pos>>32;
    cursor->dropped=skipped>0xffffffffu?0xffffffffu:(u32)skipped;
    cursor->flags=0;
    unlock(flags);return count;
}
