#include "module.h"
#include "pci.h"
#include "paging.h"
#include "pmm.h"
#include "heap.h"
#include "vga.h"
#include "klog.h"
#include "string.h"
#include "timer.h"
#include "task.h"
#include "net.h"
#include "wlan.h"
#include "wlan_frame.h"

/* Таблица держит адреса, иначе --gc-sections выкинет то, к чему ядро само не обращается. */
#define KEXPORT(sym) { #sym, (void *)(sym) }

const struct kexport kexports[] = {
    KEXPORT(pci_read), KEXPORT(pci_write), KEXPORT(pci_find_device), KEXPORT(pci_list),
    KEXPORT(vmm_map), KEXPORT(pmm_alloc), KEXPORT(pmm_free),
    KEXPORT(kmalloc), KEXPORT(kfree),
    KEXPORT(memcpy), KEXPORT(memset), KEXPORT(memcmp), KEXPORT(strcmp), KEXPORT(strlen),
    KEXPORT(kprintf), KEXPORT(klogf), KEXPORT(sleep_ms), KEXPORT(timer_ticks),
    KEXPORT(task_create), KEXPORT(firmware_read),
    KEXPORT(wlan_attach), KEXPORT(wlan_detach), KEXPORT(wlan_rx), KEXPORT(wlan_hw_block),
    KEXPORT(wlan_build_probe),
    KEXPORT(netif_add), KEXPORT(netif_del), KEXPORT(netif_rx),
    KEXPORT(module_current), KEXPORT(module_hold), KEXPORT(module_release),
};
const int kexport_n = (int)(sizeof(kexports) / sizeof(kexports[0]));
