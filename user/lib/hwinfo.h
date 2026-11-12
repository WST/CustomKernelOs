#ifndef MYOS_HWINFO_H
#define MYOS_HWINFO_H
/* Read-only ABI. Every field is a 32-bit unsigned integer except model.
 * hwinfo returns 1 for a record, 0 at the end, or a negative errno.
 * PCI index is the starting BDF (bus<<8 | device<<3 | function), inclusive;
 * advance to the returned BDF + 1. USB/SATA indices are zero-based rows.
 * Logical root-port numbers do not identify physical case connectors. */
#define HWINFO_PCI  1u
#define HWINFO_USB  2u
#define HWINFO_SATA 3u
struct hw_pci_info {
    unsigned bus, device, function, vendor_id, device_id;
    unsigned class_code, subclass, prog_if, revision, header_type, irq;
    unsigned subsystem_vendor, subsystem_device, bar[6];
};
struct hw_usb_info {
    unsigned bus, device, function, port, connected, enabled, speed;
    unsigned vendor_id, product_id, device_class, interface_class, protocol, bcd_usb;
    unsigned descriptor_valid, hid_active, controller_running, raw_status;
};
struct hw_sata_info {
    unsigned bus, device, function, port, detection, speed, power, signature;
    unsigned driver_active, sectors;
    char model[44];
};
/* USB IDs/classes describe enumeration at boot; connected/enabled/speed are
 * current PORTSC values. No hub traversal or hotplug enumeration is implied. */
int hwinfo(unsigned kind, unsigned index, void *out, unsigned size);
#endif
