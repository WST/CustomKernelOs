#ifndef PMM_H
#define PMM_H
#include "types.h"

/* Physical Memory Manager: раздаёт физические страницы (фреймы) по 4 КБ. */

#define PAGE_SIZE   4096
#define E820_ADDR   0x8000          /* куда загрузчик положил карту памяти */

enum { E820_USABLE = 1, E820_RESERVED = 2, E820_ACPI = 3, E820_NVS = 4, E820_BAD = 5 };

struct e820_entry {
    u64 base;
    u64 length;
    u32 type;
    u32 acpi;
} __attribute__((packed));

struct pmm_stats {
    u32 total_frames;               /* фреймов свободной RAM по карте E820 */
    u32 used_frames;
    u32 managed_limit;              /* физ. адрес, до которого работает PMM */
};

void pmm_init(void);
u32  pmm_alloc(void);               /* физ. адрес фрейма или 0, если память кончилась */
void pmm_free(u32 addr);
void pmm_stats(struct pmm_stats *st);

u32  e820_count(void);
const struct e820_entry *e820_get(u32 i);
const char *e820_type_name(u32 type);

#endif
