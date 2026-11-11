/* NVIDIA BAR5 is the indirect real-mode register/VRAM I/O window.
 * G80+ layout: 128 bytes, including DWORD address/data registers.
 * Reference: https://envytools.readthedocs.io/en/latest/hw/bus/bars.html
 * Discover the address from PCI; never assume a fixed port such as 0xe000.
 */
#include "nvidia_io.h"

bool nvidia_firmware_io_base(const struct pci_dev *gpu,u32 *base) {
    if(!base)return false;
    *base=0;
    /* Limit this hardware layout to TU116 / GTX 1660 SUPER. */
    if(!gpu || gpu->vendor!=0x10de || gpu->device!=0x21c4 || gpu->class_code!=3)
        return false;
    /* BAR5 belongs to this PCI function regardless of the scanout aperture.
     * Firmware LFB addresses need not equal the beginning of BAR1. Comparing
     * them rejected a real TU116 with LFB=0xf1000000 before the first VBE call.
     * Do not probe/resize live memory BARs just to admit the GPU's I/O ports. */
    u32 bar5=pci_read(gpu->bus,gpu->dev,gpu->func,0x24),start=bar5&~3u;
    if((bar5&3)!=1 || start<0x1000 || (start&(NVIDIA_FIRMWARE_IO_BYTES-1)) ||
       start>0x10000u-NVIDIA_FIRMWARE_IO_BYTES)return false;
    /* Keep firmware's decode configuration; do not change PCI command bits. */
    if(!(pci_read(gpu->bus,gpu->dev,gpu->func,0x04)&1))return false;
    *base=start;
    return true;
}

bool nvidia_firmware_io_contains(u32 base,u32 port,u32 bytes) {
    if(!base || base<0x1000 || (base&(NVIDIA_FIRMWARE_IO_BYTES-1)) ||
       base>0x10000u-NVIDIA_FIRMWARE_IO_BYTES ||
       (bytes!=1 && bytes!=2 && bytes!=4) || port<base)return false;
    return port-base<=NVIDIA_FIRMWARE_IO_BYTES-bytes;
}
