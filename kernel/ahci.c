/* AHCI DMA. The HBA interrupt is left disabled: completion is polled, then
 * the task sleeps, so a stuck line cannot spin the PIC. Buffers are physical
 * pages (identity-mapped), not kernel-heap addresses. */
#include "ahci.h"
#include "pci.h"
#include "paging.h"
#include "pmm.h"
#include "string.h"
#include "timer.h"
#include "task.h"
#include "vga.h"
#include "bootinfo.h"

#define MMIO_VIRT 0xe2800000u /* FB FIFO owns 0xe2000000, virtgpu starts at 0xe3000000 */
#define MMIO_PAGES 8
#define HBA_GHC 0x04
#define HBA_PI  0x0c
#define PORT_STRIDE 0x80
#define PORT_BASE 0x100
#define CMD_ST  (1u<<0)
#define CMD_FRE (1u<<4)
#define CMD_FR  (1u<<14)
#define CMD_CR  (1u<<15)
#define IS_FATAL ((1u<<30)|(1u<<29)|(1u<<28)|(1u<<27))

static volatile u8 *mmio;
static u32 data_page[4];
static struct {
    int index;
    u32 page;
    u32 sectors;
    char model[41];
} unit[2];

static u32 rd(u32 off) { return *(volatile u32 *)(mmio + off); }
static void wr(u32 off, u32 value) { *(volatile u32 *)(mmio + off) = value; }
static u32 port_off(int port) { return PORT_BASE + (u32)port * PORT_STRIDE; }

bool ahci_present(int u) { return u >= 0 && u < 2 && unit[u].index >= 0; }
u32 ahci_sectors(int u) { return ahci_present(u) ? unit[u].sectors : 0; }
const char *ahci_model(void) { return unit[0].model; }

static bool wait_cmd(int port, u32 mask, bool set, u32 ticks) {
    u32 began = timer_ticks();
    u32 off = port_off(port) + 0x18;
    while ((i32)(timer_ticks() - began) < (i32)ticks) {
        if ((bool)(rd(off) & mask) == set) return true;
        task_sleep_ticks(1);
    }
    return (bool)(rd(off) & mask) == set;
}

static bool stop_port(int port) {
    u32 off = port_off(port) + 0x18;
    wr(off, rd(off) & ~CMD_ST);
    if (!wait_cmd(port, CMD_CR, false, TIMER_HZ)) return false;
    wr(off, rd(off) & ~CMD_FRE);
    return wait_cmd(port, CMD_FR, false, TIMER_HZ);
}

static bool start_port(int port) {
    u32 base = port_off(port);
    wr(base + 0x30, 0xffffffffu);
    wr(base + 0x10, 0xffffffffu);
    u32 cmd = rd(base + 0x18) | CMD_FRE;
    wr(base + 0x18, cmd);
    u32 began = timer_ticks();
    while (rd(base + 0x20) & 0x88) {
        if ((i32)(timer_ticks() - began) >= (i32)TIMER_HZ) return false;
        task_sleep_ticks(1);
    }
    wr(base + 0x18, rd(base + 0x18) | CMD_ST);
    return true;
}

static int issue(int slot, u8 command, u32 lba, u32 count, bool to_device) {
    u32 base = port_off(unit[slot].index);
    volatile u8 *ct = (volatile u8 *)(unit[slot].page + 1280);
    for (int i = 0; i < 256; i++) ct[i] = 0;
    ct[0] = 0x27;
    ct[1] = 0x80;
    ct[2] = command;
    ct[4] = (u8)lba;
    ct[5] = (u8)(lba >> 8);
    ct[6] = (u8)(lba >> 16);
    ct[7] = 0x40;
    ct[8] = (u8)(lba >> 24);
    ct[12] = (u8)count;
    ct[13] = (u8)(count >> 8);
    u32 left = count * 512, prds = 0;
    for (int i = 0; i < 4 && left; i++) {
        u32 n = left > 4096 ? 4096 : left;
        volatile u32 *prd = (volatile u32 *)(ct + 0x80 + prds * 16);
        prd[0] = data_page[i];
        prd[1] = 0;
        prd[2] = 0;
        prd[3] = n - 1;
        left -= n;
        prds++;
    }
    volatile u32 *hdr = (volatile u32 *)unit[slot].page;
    hdr[0] = 5u | (to_device ? (1u << 6) : 0) | (prds << 16);
    hdr[1] = 0;
    hdr[2] = unit[slot].page + 1280;
    hdr[3] = 0;
    wr(base + 0x10, 0xffffffffu);
    wr(base + 0x38, 1);
    u32 began = timer_ticks();
    for (int i = 0; i < 256 && (rd(base + 0x38) & 1); i++)
        ;
    while (rd(base + 0x38) & 1) {
        if (rd(base + 0x10) & IS_FATAL) return -1;
        if ((i32)(timer_ticks() - began) >= 30 * (i32)TIMER_HZ) return -1;
        task_sleep_ticks(1);
    }
    if ((rd(base + 0x20) & 0x89) || (rd(base + 0x10) & IS_FATAL)) return -1;
    return 0;
}

static void copy_out(const void *src, u32 bytes) {
    const u8 *p = src;
    for (int i = 0; i < 4 && bytes; i++) {
        u32 n = bytes > 4096 ? 4096 : bytes;
        memcpy((void *)data_page[i], p, n);
        p += n;
        bytes -= n;
    }
}
static void copy_in(void *dst, u32 bytes) {
    u8 *p = dst;
    for (int i = 0; i < 4 && bytes; i++) {
        u32 n = bytes > 4096 ? 4096 : bytes;
        memcpy(p, (void *)data_page[i], n);
        p += n;
        bytes -= n;
    }
}

static u16 ident_word(const u8 *id, int i) { return id[i * 2] | (u16)id[i * 2 + 1] << 8; }

static bool setup_port(int port, int slot) {
    if (!stop_port(port)) return false;
    u32 page = pmm_alloc();
    if (!page) return false;
    memset((void *)page, 0, PAGE_SIZE);
    unit[slot].index = port;
    unit[slot].page = page;
    u32 base = port_off(port);
    wr(base + 0x00, page);
    wr(base + 0x04, 0);
    wr(base + 0x08, page + 1024);
    wr(base + 0x0c, 0);
    if (!start_port(port)) return false;
    if (issue(slot, 0xec, 0, 1, false)) return false;
    u8 *id = (u8 *)data_page[0];
    bool lba48 = ident_word(id, 83) & (1u << 10);
    u32 n = lba48 ? ident_word(id, 100) | (u32)ident_word(id, 101) << 16
                  : ident_word(id, 60) | (u32)ident_word(id, 61) << 16;
    if (lba48 && (ident_word(id, 102) || ident_word(id, 103))) n = 0x10000000u;
    if (!n) return false;
    if (n > 0x10000000u) n = 0x10000000u;
    unit[slot].sectors = n;
    for (int i = 0; i < 20; i++) {
        unit[slot].model[2 * i] = id[2 * (27 + i) + 1];
        unit[slot].model[2 * i + 1] = id[2 * (27 + i)];
    }
    unit[slot].model[40] = 0;
    for (int i = 39; i >= 0 && unit[slot].model[i] == ' '; i--) unit[slot].model[i] = 0;
    return true;
}

bool ahci_init(void) {
    for (int i = 0; i < 2; i++) unit[i].index = -1;
    const struct boot_info *live = boot_info_get();
    if (live) return false;
#ifdef MYOS_HARDWARE
    return false;
#endif
    struct pci_dev dev;
    /* This QEMU reports programming interface 2 for an AHCI 1.0 register map.
     * The version register, not the interface byte, decides whether we drive it. */
    if (!pci_find_class(0x01, 0x06, &dev)) return false;
    u32 bar = pci_read(dev.bus, dev.dev, dev.func, 0x24);
    if ((bar & 1) || ((bar & 6) == 4 && pci_read(dev.bus, dev.dev, dev.func, 0x28))) return false;
    u32 phys = bar & ~0xfu;
    if (!phys) return false;
    for (u32 off = 0; off < MMIO_PAGES * PAGE_SIZE; off += PAGE_SIZE)
        if (vmm_map(MMIO_VIRT + off, phys + off, PG_WRITE | 0x18)) return false;
    mmio = (volatile u8 *)MMIO_VIRT;
    u32 cmd = pci_read(dev.bus, dev.dev, dev.func, 0x04);
    pci_write(dev.bus, dev.dev, dev.func, 0x04, (cmd & 0xffff) | 0x06 | (1u << 10));
    wr(HBA_GHC, rd(HBA_GHC) | (1u << 31));
    if ((rd(0x10) >> 16) != 1) return false;
    for (int i = 0; i < 4; i++) {
        data_page[i] = pmm_alloc();
        if (!data_page[i]) return false;
        memset((void *)data_page[i], 0, PAGE_SIZE);
    }
    u32 pi = rd(HBA_PI);
    int slot = 0;
    for (int port = 0; port < 32 && slot < 2; port++) {
        if (!(pi & (1u << port))) continue;
        u32 ssts = rd(port_off(port) + 0x28);
        if ((ssts & 0xf) == 1) {
            u32 began = timer_ticks();
            while ((ssts & 0xf) == 1 && (i32)(timer_ticks() - began) < (i32)TIMER_HZ) {
                task_sleep_ticks(1);
                ssts = rd(port_off(port) + 0x28);
            }
        }
        if ((ssts & 0xf) != 3) continue;
        u32 sig = rd(port_off(port) + 0x24);
        if (sig && sig != 0x00000101u) continue;
        if (!setup_port(port, slot)) {
            kprintf("AHCI port %d: identify failed\n", port);
            unit[slot].index = -1;
            continue;
        }
        kprintf("AHCI port %d: %s, %u KB\n", port, unit[slot].model, unit[slot].sectors / 2);
        slot++;
    }
    return unit[0].index >= 0;
}

int ahci_read(int slot, u32 lba, void *buf) {
    if (!ahci_present(slot) || lba >= unit[slot].sectors) return -1;
    if (issue(slot, 0x25, lba, 1, false)) return -1;
    copy_in(buf, 512);
    return 0;
}

int ahci_write(int slot, u32 lba, const void *buf, u32 count) {
    if (!count || count > 32 || !ahci_present(slot) || lba >= unit[slot].sectors ||
        count > unit[slot].sectors - lba)
        return -1;
    copy_out(buf, count * 512);
    return issue(slot, 0x35, lba, count, true);
}
