#ifndef AHCI_H
#define AHCI_H
#include "types.h"

/* SATA AHCI. A present port replaces the IDE disk of the same index:
 * port 0 is /dev/sdb, port 1 is /dev/sdc. IDE remains the fallback. */
bool ahci_init(void);
bool ahci_present(int unit);
u32  ahci_sectors(int unit);
const char *ahci_model(void);
int  ahci_read(int unit, u32 lba, void *buf);
int  ahci_write(int unit, u32 lba, const void *buf, u32 count);

#endif
