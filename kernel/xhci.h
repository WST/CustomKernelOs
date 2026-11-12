#ifndef XHCI_H
#define XHCI_H
#include "types.h"
#include "hwinfo.h"
int xhci_port_info(unsigned index, struct hw_usb_info *out);
/* One xHCI controller, directly attached USB HID boot keyboards and mice. */
bool xhci_init(void);
#endif
