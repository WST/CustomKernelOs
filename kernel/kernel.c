/* Главная функция ядра: сюда приходит управление из entry.asm. */
#include "vga.h"
#include "sysinfo.h"
#include "idt.h"
#include "timer.h"
#include "keyboard.h"
#include "shell.h"
#include "services.h"
#include "ata.h"
#include "fs.h"
#include "pmm.h"
#include "paging.h"
#include "heap.h"
#include "task.h"
#include "gdt.h"
#include "fb.h"
#include "display.h"
#include "net.h"
#include "mouse.h"
#include "xhci.h"
#include "bootinfo.h"

static void ok(const char *fmt, ...) {
    va_list ap;
    vga_set_color(LIGHT_GREY, BLACK); vga_puts("[ ");
    vga_set_color(LIGHT_GREEN, BLACK); vga_puts("OK");
    vga_set_color(LIGHT_GREY, BLACK); vga_puts(" ] ");
    va_start(ap, fmt);
    vkprintf(fmt, ap);
    va_end(ap);
    vga_putc('\n');
}

static void fail(const char *what, const char *why) {
    vga_set_color(LIGHT_GREY, BLACK); vga_puts("[");
    vga_set_color(LIGHT_RED, BLACK);  vga_puts("FAIL");
    vga_set_color(LIGHT_GREY, BLACK); kprintf("] %s: %s\n", what, why);
}

void kmain(void) {
    vga_init();
    sysinfo_init();

    vga_set_color(LIGHT_CYAN, BLACK);
    vga_puts(
        "  __  __        ___  ____  \n"
        " |  \\/  |_   _ / _ \\/ ___| \n"
        " | |\\/| | | | | | | \\___ \\ \n"
        " | |  | | |_| | |_| |___) |\n"
        " |_|  |_|\\__, |\\___/|____/ \n"
        "         |___/             \n\n");

    struct system_info identity; sysinfo_get(&identity);
    ok("%s %s %s (%s)", identity.os, identity.version, identity.commit, identity.build_time);
    ok("CPU: %s", identity.cpu_brand);
    ok("VGA text console 80x25");
    gdt_init();
    idt_init();
    ok("GDT+TSS (ring 0/3), IDT, PIC remapped, syscalls on int 0x80");

    pmm_init();
    struct pmm_stats ps;
    pmm_stats(&ps);
    ok("Physical memory: %u MB usable (%u E820 regions)",
       ps.total_frames / 256, e820_count());
    paging_init();
    ok("Paging enabled: RAM identity-mapped up to %u MB, NULL page unmapped",
       paging_identity_limit() >> 20);
    heap_init();
    ok("Kernel heap at %p", HEAP_START);
    vga_save_text();
    if (fb_init()) {
        struct fb_info fi;
        fb_get_info(&fi);
        vga_switch_to_fb();
        ok("Graphics: %s, %u MB VRAM, %ux%u, %u bpp -> console %ux%u",
           fi.driver, fi.vram >> 20, fi.width, fi.height, fi.bpp, vga_cols(), vga_rows());
        if (fi.caps & FB_CAP_GPU)
            ok("GPU 2D acceleration: rect copy, fill%s", fi.caps & FB_CAP_CURSOR ? ", hardware cursor" : "");
    } else {
        ok("Graphics: no VMware SVGA / Bochs VBE adapter - staying in VGA text mode");
    }
    fpu_init();
    tasking_init();
    ok("Multitasking: preemptive round robin, %u ms time slice", TIME_SLICE * 10);

    timer_init();
    keyboard_init();
    bool have_mouse = mouse_init();
    if (have_mouse && fb_ready()) { struct fb_info fi; fb_get_info(&fi); mouse_set_bounds(fi.width, fi.height); }
    __asm__ volatile ("sti");
    ok("PIT timer 100 Hz, PS/2 keyboard%s, FPU, interrupts on", have_mouse ? ", PS/2 mouse" : "");

#ifdef MYOS_HARDWARE
    if (xhci_init()) ok("USB HID input ready");
    else fail("USB HID", "no supported direct keyboard/mouse; PS/2 or BIOS legacy may still work");
    if (boot_info_get()) ok("LIVE MODE: filesystem changes stay in RAM; internal ATA disks are hidden");
#endif

    if (net_init())
        ok("Network: Intel e1000, MAC %02x:%02x:%02x:%02x:%02x:%02x, DHCP in background",
           netcfg.mac[0], netcfg.mac[1], netcfg.mac[2], netcfg.mac[3], netcfg.mac[4], netcfg.mac[5]);
    else
        fail("Network", "no e1000 card (run QEMU with -nic user,model=e1000)");

    if (ata_init()) {
        ok("ATA disk: %s, %u KB", ata_model(), ata_sectors() / 2);
        bool formatted;
        int r = fs_init(&formatted);
        if (r == FS_OK) {
            struct fs_info fi;
            fs_info(&fi);
            ok("MyFS %s, %u KB free",
               formatted ? "created (new disk formatted)" : "mounted", fi.free_blocks / 2);
            struct fb_info before, after;
            fb_get_info(&before);
            display_load_config();
            fb_get_info(&after);
            if (fb_ready() && after.gen != before.gen)
                ok("Display: %ux%u from /etc/display", after.width, after.height);
        } else {
            fail("MyFS", fs_strerror(r));
        }
    } else {
        fail("ATA disk", "not found - file commands disabled");
    }

    vga_set_color(WHITE, BLACK);
    kprintf("\nWelcome to MyOS! Type 'help' for a list of commands.\n\n");

    services_init();
    shell_run();
}
