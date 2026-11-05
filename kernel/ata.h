#ifndef ATA_H
#define ATA_H
#include "types.h"

#define ATA_SECTOR 512

bool ata_init(void);                         /* ищет диск на primary slave */
u32  ata_sectors(void);                      /* размер диска в секторах */
const char *ata_model(void);
int  ata_read(u32 lba, void *buf);           /* 0 = успех, <0 = ошибка */
int  ata_write(u32 lba, const void *buf);

#endif
