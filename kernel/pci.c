/* Минимальный драйвер PCI: читаем конфигурационное пространство устройств.
 *
 * Адрес, который пишем в 0xCF8:
 *   бит 31     — «включить»
 *   биты 23-16 — шина, 15-11 — устройство, 10-8 — функция, 7-2 — номер регистра
 * После этого в 0xCFC появляется 32-битное значение регистра. */
#include "pci.h"

static inline void outl(u16 port, u32 v) { __asm__ volatile ("outl %0, %1" : : "a"(v), "Nd"(port)); }
static inline u32  inl(u16 port) { u32 v; __asm__ volatile ("inl %1, %0" : "=a"(v) : "Nd"(port)); return v; }

static bool probe(u8 bus, u8 dev, u8 func, struct pci_dev *d);

/* CF8/CFC form one shared transaction, including on SMP. */
static volatile u32 config_guard;
static u32 config_lock(void) {
    u32 flags=irq_save();
    while (__sync_lock_test_and_set(&config_guard,1)) __asm__ volatile ("pause");
    return flags;
}
static void config_unlock(u32 flags) { __sync_lock_release(&config_guard); irq_restore(flags); }
u32 pci_read(u8 bus, u8 dev, u8 func, u8 offset) {
    u32 flags=config_lock(), value;
#ifdef HOST_TEST
    extern u32 pci_test_read(u8,u8,u8,u8);
    value=pci_test_read(bus,dev,func,offset);
#else
    outl(0xCF8, 0x80000000u | ((u32)bus << 16) | ((u32)dev << 11) | ((u32)func << 8) | (offset & 0xFC));
    value=inl(0xCFC);
#endif
    config_unlock(flags); return value;
}
void pci_write(u8 bus, u8 dev, u8 func, u8 offset, u32 value) {
    u32 flags=config_lock();
#ifdef HOST_TEST
    extern void pci_test_write(u8,u8,u8,u8,u32);
    pci_test_write(bus,dev,func,offset,value);
#else
    outl(0xCF8, 0x80000000u | ((u32)bus << 16) | ((u32)dev << 11) | ((u32)func << 8) | (offset & 0xFC));
    outl(0xCFC, value);
#endif
    config_unlock(flags);
}

static bool probe(u8 bus, u8 dev, u8 func, struct pci_dev *d) {
    u32 id = pci_read(bus, dev, func, 0x00);
    if ((id & 0xFFFF) == 0xFFFF) return false;          /* никого нет */
    u32 cls = pci_read(bus, dev, func, 0x08);
    d->bus = bus; d->dev = dev; d->func = func;
    d->vendor = id & 0xFFFF;
    d->device = id >> 16;
    d->class_code = cls >> 24;
    d->subclass = (cls >> 16) & 0xFF;
    d->bar0 = pci_read(bus, dev, func, 0x10);
    return true;
}

bool pci_next(unsigned cursor, struct pci_dev *out) {
    while (cursor<65536u) {
        u8 bus=cursor>>8, dev=(cursor>>3)&31, func=cursor&7;
        u32 id=pci_read(bus,dev,0,0);
        if ((id&65535)==65535 ||
            (func && !(pci_read(bus,dev,0,12)&0x800000))) {
            cursor=(cursor+8)&~7u; continue;
        }
        if (probe(bus,dev,func,out)) return true;
        cursor++;
    }
    return false;
}
int pci_list(struct pci_dev *out, int max) {
    int n=0; unsigned cursor=0;
    while (n<max && pci_next(cursor,&out[n])) {
        cursor=((unsigned)out[n].bus<<8)|((unsigned)out[n].dev<<3)|out[n].func;
        cursor++; n++;
    }
    return n;
}
bool pci_find_device(u16 vendor, u16 device, struct pci_dev *out) {
    struct pci_dev d; unsigned cursor=0;
    while (pci_next(cursor,&d)) {
        if (d.vendor==vendor && d.device==device) { *out=d; return true; }
        cursor=(((unsigned)d.bus<<8)|((unsigned)d.dev<<3)|d.func)+1;
    }
    return false;
}
bool pci_find_class(u8 class_code, u8 subclass, struct pci_dev *out) {
    struct pci_dev d; unsigned cursor=0;
    while (pci_next(cursor,&d)) {
        if (d.class_code==class_code && d.subclass==subclass) { *out=d; return true; }
        cursor=(((unsigned)d.bus<<8)|((unsigned)d.dev<<3)|d.func)+1;
    }
    return false;
}
bool pci_find_display_vendor(u16 vendor, struct pci_dev *out) {
    struct pci_dev d; unsigned cursor=0;
    while (pci_next(cursor,&d)) {
        if (d.vendor==vendor && d.class_code==3) { *out=d; return true; }
        cursor=(((unsigned)d.bus<<8)|((unsigned)d.dev<<3)|d.func)+1;
    }
    return false;
}
