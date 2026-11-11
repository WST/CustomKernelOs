#ifndef KLOG_H
#define KLOG_H
#include "types.h"
#define KLOG_CAPACITY 16384u
#define KLOG_OLDEST 1u
#define KLOG_END 2u
/* 64-bit byte sequence split into words for the i386 userspace ABI.
 * Each reader owns its cursor. Reads never drain the shared ring. */
struct klog_cursor { u32 lo,hi,dropped,flags; };
_Static_assert(sizeof(struct klog_cursor)==16,"kernel log cursor ABI");
void klog_write(const char *text,u32 length);
int klog_read(void *buffer,u32 capacity,struct klog_cursor *cursor);
void klogf(const char *fmt,...);
#endif
