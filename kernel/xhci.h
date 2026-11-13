#ifndef XHCI_H
#define XHCI_H
#include "types.h"
#include "hwinfo.h"
int xhci_port_info(unsigned index, struct hw_usb_info *out);
int xhci_device_info(unsigned index,struct hw_usb_device_info *out);
/* One xHCI controller, hotplug, USB2 hubs and HID keyboards/mice. */
bool xhci_init(void);
#endif
