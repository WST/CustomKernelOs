#ifndef AHCI_H
#define AHCI_H
#include "types.h"
#include "hwinfo.h"
int ahci_port_info(unsigned index, struct hw_sata_info *out);

/* SATA AHCI. A present port replaces the IDE disk of the same index:
 * port 0 is /dev/sdb, port 1 is /dev/sdc. IDE remains the fallback. */
/* 16 страниц DMA по 4 КиБ: 64 КиБ за одну команду. Больше за раз ATA PIO не умеет,
 * там потолок 32 сектора и он остаётся в ata.c. */
#define AHCI_MAX_SECTORS 128
bool ahci_init(void);
int ahci_installer_probe(void);
const char *ahci_unit_model(int unit);
bool ahci_present(int unit);
u32  ahci_sectors(int unit);
const char *ahci_model(void);
int ahci_flush(int unit);
int  ahci_read(int unit, u32 lba, void *buf);
int  ahci_read_sectors(int unit, u32 lba, void *buf, u32 count);
int  ahci_write(int unit, u32 lba, const void *buf, u32 count);

#endif
