#ifndef ACPI_SMP_H
#define ACPI_SMP_H
#include "smp.h"
#define ACPI_SMP_TABLE_MAX 0x100000u
#define ACPI_SMP_ROOT_SLOT 0u
#define ACPI_SMP_CHILD_SLOT 1u
/* Each slot retains its pointer until that same slot is mapped again. */
typedef const void *(*acpi_smp_map_fn)(u32 physical,u32 bytes,unsigned slot,void *context);
enum acpi_smp_error {
    ACPI_SMP_OK, ACPI_SMP_BAD_RSDP, ACPI_SMP_BAD_ROOT,
    ACPI_SMP_NO_MADT, ACPI_SMP_BAD_MADT, ACPI_SMP_HIGH_ADDRESS
};
struct acpi_smp_info {
    u32 rsdp, root, madt, lapic, count, reported, unaddressable;
    bool xsdt;
    u8 ids[SMP_MAX_CPUS];
};
/* Does not dereference a physical address or execute privileged instructions. */
enum acpi_smp_error acpi_smp_discover(u32 rsdp,u8 bsp,acpi_smp_map_fn map,
                                    void *context,struct acpi_smp_info *out);
const char *acpi_smp_error_string(enum acpi_smp_error error);
#endif
