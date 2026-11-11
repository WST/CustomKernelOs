#ifndef PCI_H
#define PCI_H
#include "types.h"

/* Шина PCI: конфигурационное пространство через порты 0xCF8 (адрес) и 0xCFC (данные). */

struct pci_dev {
    u8  bus, dev, func;
    u16 vendor, device;
    u8  class_code, subclass;
    u32 bar0;
};

u32  pci_read(u8 bus, u8 dev, u8 func, u8 offset);
void pci_write(u8 bus, u8 dev, u8 func, u8 offset, u32 value);
bool pci_find_device(u16 vendor, u16 device, struct pci_dev *out);
bool pci_find_class(u8 class_code, u8 subclass, struct pci_dev *out);   /* первое такое устройство */
bool pci_find_display_vendor(u16 vendor, struct pci_dev *out);
int  pci_list(struct pci_dev *out, int max);                             /* все устройства шины 0 */

#endif
