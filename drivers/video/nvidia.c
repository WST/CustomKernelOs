/* Initial NVIDIA display backend: adopt firmware scanout without resetting
 * the GPU. No MMIO, VRAM BAR reprogramming or native modesetting is attempted.
 * The existing fb drawing path provides CPU blits and a software cursor. */
#include "video.h"
#include "../../kernel/pci.h"
#include "../../kernel/vga.h"
#include "../../kernel/string.h"

void nvidia_firmware_identify(struct fb_info *info) {
    struct pci_dev gpu;
    if (!pci_find_display_vendor(0x10de,&gpu)) return;
    const char *model=gpu.device==0x21c4?"GeForce GTX 1660 SUPER":"NVIDIA GPU";
    bool uefi=info->driver && !strcmp(info->driver,"UEFI GOP framebuffer");
    kprintf("NVIDIA display controller: %x:%x.%x device=%x (%s); %s output, CPU rendering\n",
            gpu.bus,gpu.dev,gpu.func,gpu.device,model,uefi?"UEFI GOP":"BIOS VBE");
    /* On hybrid systems the NVIDIA GPU need not own the firmware display.
     * Only attribute the backend when the LFB starts at NVIDIA's VRAM BAR1.
     * Never truncate an above-4GiB BAR or enable PCI bus mastering here. */
    u32 bar=pci_read(gpu.bus,gpu.dev,gpu.func,0x14);
    u32 kind=(bar>>1)&3;
    if ((bar&1) || (kind!=0 && kind!=2) || !(bar&0xfffffff0u)) return;
    if (kind==2 && pci_read(gpu.bus,gpu.dev,gpu.func,0x18)) return;
    if ((bar&0xfffffff0u)!=info->phys) return;
    info->driver=uefi?"NVIDIA UEFI GOP":gpu.device==0x21c4?"NVIDIA 1660 SUPER VBE":"NVIDIA BIOS VBE";
}
