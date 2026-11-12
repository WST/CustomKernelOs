#ifndef BOOTINFO_H
#define BOOTINFO_H
#include "types.h"
#define BOOT_INFO_MAGIC 0x534f594du
/* Shared live-root contract; the image builder passes these limits to NASM. */
#define LIVE_ROOT_PHYS 0x01000000u
#define LIVE_ROOT_MIN_MB 64
#define LIVE_ROOT_MAX_MB 512
#define LIVE_ROOT_RESERVE_MB 64
struct boot_info {
    u32 magic, version, root_phys, root_bytes, font_phys;
    u32 fb_phys, width, height, bpp, pitch;
    u32 bios_snapshot_magic; /* 0x49564256: page zero saved at 0x2000 */
};
static inline bool boot_root_valid(const struct boot_info *b) {
    return b && b->root_phys==LIVE_ROOT_PHYS &&
           b->root_bytes>=LIVE_ROOT_MIN_MB*1024u*1024u &&
           b->root_bytes<=LIVE_ROOT_MAX_MB*1024u*1024u && !(b->root_bytes&511u);
}
/* Only the separate hardware build trusts the low-memory boot contract. */
static inline const struct boot_info *boot_info_get(void) {
#ifdef MYOS_HARDWARE
    const struct boot_info *b=(const void *)0x6000;
    return b->magic==BOOT_INFO_MAGIC && b->version==1 ? b : NULL;
#else
    return NULL;
#endif
}
#endif
