#ifndef MYOS_HWFORMAT_H
#define MYOS_HWFORMAT_H
static inline const char *hw_vendor(unsigned id) {
    switch(id) {
    case 0x8086: return "Intel";
    case 0x1022: return "AMD";
    case 0x1002: return "AMD/ATI";
    case 0x10de: return "NVIDIA";
    case 0x10ec: return "Realtek";
    case 0x1b36: return "QEMU";
    case 0x1234: return "QEMU";
    case 0x15ad: return "VMware";
    case 0x1af4: return "Virtio";
    case 0x1b21: return "ASMedia";
    case 0x1912: return "Renesas";
    default: return "Unknown vendor";
    }
}
static inline const char *hw_pci_class(unsigned cls,unsigned sub,unsigned pi) {
    switch(cls) {
    case 0: return "Unclassified";
    case 1:
        switch(sub) {
        case 0: return "SCSI controller";
        case 1: return "IDE controller";
        case 4: return "RAID controller";
        case 6: return "SATA controller";
        case 8: return pi==2?"NVMe controller":"NVM controller";
        default: return "Storage controller";
        }
    case 2: return sub==0?"Ethernet controller":"Network controller";
    case 3: return sub==0?"VGA controller":"Display controller";
    case 4: return sub==3?"Audio controller":"Multimedia controller";
    case 5: return "Memory controller";
    case 6:
        switch(sub) {
        case 0: return "Host bridge";
        case 1: return "ISA bridge";
        case 4: return "PCI bridge";
        default: return "Bridge";
        }
    case 7: return "Communication controller";
    case 8: return "System peripheral";
    case 9: return "Input controller";
    case 0x0c:
        if(sub==3) {
            switch(pi) {
            case 0: return "USB UHCI controller";
            case 0x10: return "USB OHCI controller";
            case 0x20: return "USB EHCI controller";
            case 0x30: return "USB xHCI controller";
            default: return "USB controller";
            }
        }
        return sub==5?"SMBus controller":"Serial bus controller";
    case 0x10: return "Encryption controller";
    default: return "Other device";
    }
}
static inline const char *hw_usb_speed(unsigned speed) {
    switch(speed) {
    case 1: return "12M";
    case 2: return "1.5M";
    case 3: return "480M";
    case 4: return "5G";
    default: return "unknown"; /* Higher xHCI speed IDs are controller-defined. */
    }
}
static inline const char *hw_usb_class(unsigned cls,unsigned protocol) {
    switch(cls) {
    case 0: return "Unknown/per-interface";
    case 1: return "Audio";
    case 2: return "Communications";
    case 3: return protocol==1?"HID keyboard":protocol==2?"HID mouse":"HID";
    case 8: return "Mass storage";
    case 9: return "Hub";
    case 0x0e: return "Video";
    case 0xff: return "Vendor-specific";
    default: return "Other";
    }
}
static inline void hw_usb_row(const struct hw_usb_info *p) {
    printf("%02x:%02x.%x port %-3u %-12s",p->bus,p->device,p->function,p->port,
           p->connected?"connected":"empty");
    if (!p->connected) { putchar('\n'); return; }
    printf(" %-7s %s",hw_usb_speed(p->speed),p->enabled?"enabled":"disabled");
    if (p->descriptor_valid)
        printf(" %04x:%04x %s [boot descriptor]",p->vendor_id,p->product_id,
               hw_usb_class(p->device_class?p->device_class:p->interface_class,p->protocol));
    else printf(" descriptor unavailable");
    if (p->hid_active && p->controller_running) printf(" [HID configured at boot]");
    if (!p->controller_running) printf(" [controller inactive]");
    putchar('\n');
}
#endif
