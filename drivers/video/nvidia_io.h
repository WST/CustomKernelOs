#ifndef NVIDIA_FIRMWARE_IO_H
#define NVIDIA_FIRMWARE_IO_H
#include "../../kernel/pci.h"
#define NVIDIA_FIRMWARE_IO_BYTES 0x80u
bool nvidia_firmware_io_base(const struct pci_dev *gpu,u32 *base);
bool nvidia_firmware_io_contains(u32 base,u32 port,u32 bytes);
#endif
