#ifndef NVME_H
#define NVME_H
#include "types.h"
#define NVME_MMIO_BASE 0xe6000000u
/* Unit 0 is the installed root; units 3/4 are explicit Live targets. */
bool nvme_init(void);
int nvme_installer_probe(void);
bool nvme_present(int unit);
u32 nvme_sectors(int unit);
const char *nvme_model(int unit);
int nvme_read(int unit,u32 lba,void *buf);
int nvme_write(int unit,u32 lba,const void *buf,u32 count);
int nvme_flush(int unit);
void nvme_shutdown(void); /* after fs_sync, immediately before reset/poweroff */
#endif
