#ifndef KERNEL_SYSINFO_H
#define KERNEL_SYSINFO_H
#include "types.h"
#include "../user/lib/sysinfo.h"
void sysinfo_init(void);
void sysinfo_get(struct system_info *out);
void sysinfo_print(void);
#endif
