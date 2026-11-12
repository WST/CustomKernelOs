#ifndef ATA_H
#define ATA_H
#include "types.h"

#define ATA_SECTOR 512

bool ata_init(void);                         /* ищет диск на primary slave */
u32  ata_sectors(void);                      /* размер диска в секторах */
const char *ata_model(void);
int  ata_read(u32 lba, void *buf);           /* 0 = успех, <0 = ошибка */
int  ata_write(u32 lba, const void *buf);
/* unit 0: primary slave (system), unit 1: secondary master (data). */
u32 ata_unit_sectors(int unit);
int ata_unit_read(int unit,u32 lba,void *buf);
int ata_unit_write(int unit,u32 lba,const void *buf);
int ata_unit_read_sectors(int unit,u32 lba,void *buf,u32 count);
int ata_unit_write_sectors(int unit,u32 lba,const void *buf,u32 count);

#endif
