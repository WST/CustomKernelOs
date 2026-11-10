/* Absolute VirtIO tablet: avoid host relative-pointer capture/warp behavior.
 * Wire layouts follow the VirtIO specification and Linux virtio_input.h.
 * All device state is private; the timer consumes and replenishes eventq. */
#include "virtinput.h"
#include "pci.h"
#include "paging.h"
#include "pmm.h"
#include "mouse.h"
#include "idt.h"
#include "string.h"

#define QN 64
#define MMIO 0xE3200000u
#define BARRIER() __asm__ volatile("mfence" : : : "memory")
struct common {
    u32 feature_sel, feature, guest_sel, guest;
    u16 msix, queues;
    u8 status, generation;
    u16 queue_sel, queue_size, queue_msix, queue_enable, notify_off;
    u32 desc_lo, desc_hi, avail_lo, avail_hi, used_lo, used_hi;
} __attribute__((packed));
struct desc { u64 addr; u32 len; u16 flags, next; } __attribute__((packed));
struct event { u16 type, code; i32 value; } __attribute__((packed));
struct config { u8 select, subsel, size, reserved[5]; u32 data[32]; } __attribute__((packed));
static struct pci_dev device;
static volatile struct common *cfg;
static volatile struct config *input;
static volatile u16 *notify, *avail, *used;
static volatile struct event *events;
static u32 mmio_next, buttons, axis[2], minimum[2], maximum[2];
static i32 wheel;
static u16 consumed;
static bool ready, interrupt_mode;
static volatile u8 *isr;
static void input_irq(struct regs *r) {
    (void)r;
    if (*isr & 1) virtinput_poll();
}
void virtinput_tick(void) { if (!interrupt_mode) virtinput_poll(); }
bool virtinput_interrupts(void) { return interrupt_mode; }

static u32 read(u32 off) { return pci_read(device.bus, device.dev, device.func, off); }
static u32 byte(u32 off) { return (read(off) >> ((off & 3) * 8)) & 255; }
static void *map(u32 cap) {
    u32 bar = byte(cap + 4);
    if (bar > 5) return NULL;
    u32 base = read(0x10 + bar * 4), off = read(cap + 8), len = read(cap + 12);
    if (!base || (base & 1) || ((base & 6) == 4 && read(0x14 + bar * 4))) return NULL;
    base &= ~15u;
    if (!len || len > 0x10000 || off > 0xffffffffu - base) return NULL;
    u32 addr = base + off, phys = addr & ~4095u;
    if (addr > 0xffffffffu - len) return NULL;
    u32 bytes = ((addr & 4095) + len + 4095) & ~4095u, virt = mmio_next;
    if (virt > MMIO + 0x100000u - bytes) return NULL;
    for (u32 i = 0; i < bytes; i += 4096)
        if (vmm_map(virt + i, phys + i, PG_WRITE | 0x18)) return NULL;
    mmio_next += bytes;
    return (void *)(virt + (addr & 4095));
}

bool virtinput_init(void) {
    if (!pci_find_device(0x1af4, 0x1052, &device)) return false;
    mmio_next = MMIO;
    u32 mult = 0, notify_len = 0;
    for (u32 cap = byte(0x34) & ~3u, guard = 0; cap >= 0x40 && guard++ < 48; cap = byte(cap + 1) & ~3u) {
        if (byte(cap) != 9 || byte(cap + 2) < 16) continue;
        u32 type = byte(cap + 3), len = read(cap + 12);
        if (type == 1 && len >= sizeof(*cfg)) cfg = map(cap);
        if (type == 2 && byte(cap + 2) >= 20) { notify = map(cap); mult = read(cap + 16); notify_len = len; }
        if (type == 4 && len >= sizeof(*input)) input = map(cap);
        if (type == 3 && len >= 1) isr = map(cap);
    }
    if (!cfg || !notify || !input) return false;
    pci_write(device.bus, device.dev, device.func, 4, (read(4) & 0xffff) | 6 | (1u << 10));
    cfg->status = 0;
    for (u32 n = 1000000; cfg->status && --n;) __asm__ volatile("pause");
    if (cfg->status) return false;
    cfg->status = 3;
    cfg->feature_sel = 1;
    if (!(cfg->feature & 1)) return false; /* VIRTIO_F_VERSION_1 */
    cfg->guest_sel = 0; cfg->guest = 0;
    cfg->guest_sel = 1; cfg->guest = 1;
    cfg->status = 11;
    if (!(cfg->status & 8)) return false;
    input->select = 0x11; input->subsel = 3; BARRIER(); /* EV_BITS, EV_ABS */
    if (!input->size || (input->data[0] & 3) != 3) return false;
    for (u32 i = 0; i < 2; i++) {
        input->select = 0x12; input->subsel = i; BARRIER();
        if (input->size < 20) return false;
        minimum[i] = input->data[0]; maximum[i] = input->data[1];
        if (minimum[i] > maximum[i] || maximum[i] - minimum[i] < 1 || maximum[i] - minimum[i] > 65535) return false;
        axis[i] = minimum[i] + (maximum[i] - minimum[i]) / 2;
    }
    cfg->queue_sel = 0;
    if (cfg->queue_size < QN || notify_len < 2 ||
        (cfg->notify_off && mult > (notify_len - 2) / cfg->notify_off)) return false;
    u32 page = pmm_alloc();
    if (!page) return false;
    memset((void *)page, 0, 4096);
    volatile struct desc *descs = (void *)page;
    avail = (void *)(page + 1024); used = (void *)(page + 1160); events = (void *)(page + 2048);
    for (u32 i = 0; i < QN; i++) {
        descs[i] = (struct desc){ page + 2048 + i * 8, 8, 2, 0 };
        avail[2 + i] = i;
    }
    u32 irq = byte(0x3c);
    interrupt_mode = isr && irq >= 3 && irq < 16 && irq_try_install(irq, input_irq);
    avail[0] = interrupt_mode ? 0 : 1; avail[1] = QN;
    if (interrupt_mode) pci_write(device.bus, device.dev, device.func, 4, (read(4) & 0xffff) & ~(1u << 10));
    cfg->queue_size = QN;
    cfg->desc_lo = page; cfg->desc_hi = 0;
    cfg->avail_lo = page + 1024; cfg->avail_hi = 0;
    cfg->used_lo = page + 1160; cfg->used_hi = 0;
    notify = (void *)((u32)notify + cfg->notify_off * mult);
    cfg->queue_enable = 1;
    BARRIER(); cfg->status = 15;
    ready = true;
    mouse_absolute_enable(true);
    mouse_absolute_report(0, 16384, 16384, 0);
    *notify = 0;
    return true;
}

void virtinput_poll(void) {
    if (!ready) return;
    u16 posted = avail[1];
    unsigned count = 0;
    while (consumed != used[1] && count++ < QN) {
        BARRIER();
        volatile u32 *entry = (void *)((u32)used + 4 + (consumed % QN) * 8);
        u32 id = entry[0], len = entry[1];
        if (id >= QN || len != sizeof(struct event)) {
            ready = false; cfg->status = 0; mouse_absolute_enable(false); return;
        }
        struct event e = events[id];
        if (e.type == 3 && e.code < 2) {
            u32 v = e.value < 0 ? 0 : (u32)e.value;
            if (v < minimum[e.code]) v = minimum[e.code];
            if (v > maximum[e.code]) v = maximum[e.code];
            axis[e.code] = v;
        } else if (e.type == 1 && e.code >= 0x110 && e.code <= 0x112) {
            u32 mask = 1u << (e.code - 0x110);
            if (e.value) buttons |= mask; else buttons &= ~mask;
        } else if (e.type == 2 && e.code == 8) {
            if (e.value >= -127 && e.value <= 127) wheel += e.value;
        } else if (e.type == 0 && e.code == 0) {
            u32 x = (axis[0] - minimum[0]) * 32767 / (maximum[0] - minimum[0]);
            u32 y = (axis[1] - minimum[1]) * 32767 / (maximum[1] - minimum[1]);
            mouse_absolute_report(buttons, x, y, wheel); wheel = 0;
        }
        consumed++;
        avail[2 + (posted++ % QN)] = id;
    }
    if (posted != avail[1]) { BARRIER(); avail[1] = posted; BARRIER(); *notify = 0; }
}
