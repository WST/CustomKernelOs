#ifndef KERNEL_HWINFO_H
#define KERNEL_HWINFO_H
#include "types.h"
#include "../user/lib/hwinfo.h"
unsigned hwinfo_record_size(unsigned kind);
int hwinfo_read(unsigned kind, unsigned index, void *out);
#endif
