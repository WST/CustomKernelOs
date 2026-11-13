#include "hwinfo.h"
#include "pci.h"
#include "xhci.h"
#include "ahci.h"
#include "string.h"

unsigned hwinfo_record_size(unsigned kind) {
    switch (kind) {
    case HWINFO_PCI: return sizeof(struct hw_pci_info);
    case HWINFO_USB: return sizeof(struct hw_usb_info);
    case HWINFO_USB_DEVICE: return sizeof(struct hw_usb_device_info);
    case HWINFO_SATA: return sizeof(struct hw_sata_info);
    default: return 0;
    }
}
int hwinfo_read(unsigned kind, unsigned index, void *out) {
    if (kind == HWINFO_USB) return xhci_port_info(index, out);
    if (kind == HWINFO_USB_DEVICE) return xhci_device_info(index, out);
    if (kind == HWINFO_SATA) return ahci_port_info(index, out);
    if (kind != HWINFO_PCI || index > 65536u) return -22;
    struct pci_dev dev;
    if (!pci_next(index, &dev)) return 0;
    struct hw_pci_info *p = out;
    memset(p, 0, sizeof(*p));
    p->bus=dev.bus; p->device=dev.dev; p->function=dev.func;
    p->vendor_id=dev.vendor; p->device_id=dev.device;
    u32 cls=pci_read(dev.bus,dev.dev,dev.func,8);
    p->class_code=cls>>24; p->subclass=(cls>>16)&255;
    p->prog_if=(cls>>8)&255; p->revision=cls&255;
    p->header_type=(pci_read(dev.bus,dev.dev,dev.func,12)>>16)&255;
    p->irq=pci_read(dev.bus,dev.dev,dev.func,0x3c)&255;
    p->subsystem_vendor=p->subsystem_device=0xffff;
    unsigned type=p->header_type&127;
    if (type==0) {
        u32 sub=pci_read(dev.bus,dev.dev,dev.func,0x2c);
        p->subsystem_vendor=sub&65535; p->subsystem_device=sub>>16;
    }
    unsigned bars=type==0?6:type==1?2:0;
    for (unsigned i=0;i<bars;i++) p->bar[i]=pci_read(dev.bus,dev.dev,dev.func,0x10+i*4);
    return 1;
}
