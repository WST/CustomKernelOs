/* Драйвер жёсткого диска ATA в режиме PIO (опрос портов, без DMA и прерываний).
 *
 * Primary-канал IDE: регистры 0x1F0..0x1F7, управление 0x3F6.
 * Работаем с ведомым (slave) диском — на master лежит загрузочный образ,
 * а на slave — диск с файловой системой (disk.img). */
#include "ata.h"

#define ATA_DATA      0x1F0
#define ATA_ERROR     0x1F1
#define ATA_COUNT     0x1F2
#define ATA_LBA0      0x1F3
#define ATA_LBA1      0x1F4
#define ATA_LBA2      0x1F5
#define ATA_DRIVE     0x1F6
#define ATA_STATUS    0x1F7   /* чтение */
#define ATA_COMMAND   0x1F7   /* запись */
#define ATA_CONTROL   0x3F6

#define ST_ERR  0x01
#define ST_DRQ  0x08
#define ST_DF   0x20
#define ST_BSY  0x80

#define CMD_READ     0x20
#define CMD_WRITE    0x30
#define CMD_FLUSH    0xE7
#define CMD_IDENTIFY 0xEC

#define DRIVE_SLAVE  0x10     /* бит 4 регистра DRIVE */

static bool present;
static u32  total_sectors;
static char model[41];

/* ~400 нс: четыре чтения альтернативного статуса */
static void delay400(void) { for (int i = 0; i < 4; i++) inb(ATA_CONTROL); }

static int wait_ready(bool need_drq) {
    for (u32 i = 0; i < 1000000; i++) {
        u8 st = inb(ATA_STATUS);
        if (st & ST_BSY) continue;
        if (st & (ST_ERR | ST_DF)) return -1;
        if (!need_drq || (st & ST_DRQ)) return 0;
    }
    return -2;  /* таймаут */
}

bool ata_init(void) {
    outb(ATA_CONTROL, 0x02);               /* nIEN: без прерываний, только опрос */
    outb(ATA_DRIVE, 0xA0 | DRIVE_SLAVE);
    delay400();
    outb(ATA_COUNT, 0);
    outb(ATA_LBA0, 0);
    outb(ATA_LBA1, 0);
    outb(ATA_LBA2, 0);
    outb(ATA_COMMAND, CMD_IDENTIFY);
    if (inb(ATA_STATUS) == 0) return false;          /* диска нет */

    while (inb(ATA_STATUS) & ST_BSY) ;
    if (inb(ATA_LBA1) || inb(ATA_LBA2)) return false; /* это не ATA (ATAPI и т.п.) */
    if (wait_ready(true) < 0) return false;

    u16 id[256];
    for (int i = 0; i < 256; i++) id[i] = inw(ATA_DATA);

    total_sectors = id[60] | ((u32)id[61] << 16);    /* число секторов LBA28 */
    for (int i = 0; i < 20; i++) {                   /* модель: байты в словах переставлены */
        model[i * 2]     = id[27 + i] >> 8;
        model[i * 2 + 1] = id[27 + i] & 0xFF;
    }
    model[40] = 0;
    for (int i = 39; i >= 0 && model[i] == ' '; i--) model[i] = 0;

    present = total_sectors > 0;
    return present;
}

u32 ata_sectors(void) { return total_sectors; }
const char *ata_model(void) { return model; }

static int setup(u32 lba, u8 cmd) {
    if (!present || lba >= total_sectors) return -1;
    if (wait_ready(false) < 0) return -1;
    outb(ATA_DRIVE, 0xE0 | DRIVE_SLAVE | ((lba >> 24) & 0x0F));  /* режим LBA */
    delay400();
    outb(ATA_COUNT, 1);
    outb(ATA_LBA0, lba & 0xFF);
    outb(ATA_LBA1, (lba >> 8) & 0xFF);
    outb(ATA_LBA2, (lba >> 16) & 0xFF);
    outb(ATA_COMMAND, cmd);
    delay400();
    return wait_ready(true);
}

int ata_read(u32 lba, void *buf) {
    if (setup(lba, CMD_READ) < 0) return -1;
    u16 *w = buf;
    for (int i = 0; i < 256; i++) w[i] = inw(ATA_DATA);
    return 0;
}

int ata_write(u32 lba, const void *buf) {
    if (setup(lba, CMD_WRITE) < 0) return -1;
    const u16 *w = buf;
    for (int i = 0; i < 256; i++) outw(ATA_DATA, w[i]);
    outb(ATA_COMMAND, CMD_FLUSH);          /* сбросить кэш диска на «пластины» */
    return wait_ready(false);
}
