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

u32 pci_read(u8 bus, u8 dev, u8 func, u8 offset) {
    outl(0xCF8, 0x80000000u | ((u32)bus << 16) | ((u32)dev << 11) | ((u32)func << 8) | (offset & 0xFC));
    return inl(0xCFC);
}

void pci_write(u8 bus, u8 dev, u8 func, u8 offset, u32 value) {
    outl(0xCF8, 0x80000000u | ((u32)bus << 16) | ((u32)dev << 11) | ((u32)func << 8) | (offset & 0xFC));
    outl(0xCFC, value);
}

bool pci_find_device(u16 vendor, u16 device, struct pci_dev *out) {
    struct pci_dev d;
    for (u8 dev = 0; dev < 32; dev++)
        if (probe(0, dev, 0, &d) && d.vendor == vendor && d.device == device) { *out = d; return true; }
    return false;
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

int pci_list(struct pci_dev *out, int max) {
    int n = 0;
    for (u8 dev = 0; dev < 32 && n < max; dev++)
        if (probe(0, dev, 0, &out[n])) n++;
    return n;
}

bool pci_find_class(u8 class_code, u8 subclass, struct pci_dev *out) {
    struct pci_dev d;
    for (u8 dev = 0; dev < 32; dev++)
        if (probe(0, dev, 0, &d) && d.class_code == class_code && d.subclass == subclass) {
            *out = d;
            return true;
        }
    return false;
}
