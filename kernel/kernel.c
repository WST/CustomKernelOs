#include "klog.h"
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
#include "smp.h"
#include "gdt.h"
#include "fb.h"
#include "display.h"
#include "net.h"
#include "e1000.h"
#include "mouse.h"
#include "virtinput.h"
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

/* Print before entering a subsystem, so a hang leaves its stage visible. */
static void boot_stage(const char *stage) {
    vga_set_color(LIGHT_CYAN, BLACK);
    kprintf("[BOOT] %s\n",stage);
    vga_set_color(LIGHT_GREY, BLACK);
}

void kmain(void) {
    vga_init();
    boot_stage("early CPU and system information");
    smp_early_init();
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
    const struct boot_info *boot=boot_info_get();
    if (boot) {
        ok("Boot: BIOS/CSM live, root=%x bytes=%u",boot->root_phys,boot->root_bytes);
        ok("VBE boot: base=%x pitch=%u, %ux%u, %u bpp",
           boot->fb_phys,boot->pitch,boot->width,boot->height,boot->bpp);
    } else ok("Boot: standard disk image");
    boot_stage("GDT/TSS and interrupt tables");
    gdt_init();
    idt_init();
    ok("GDT+TSS (ring 0/3), IDT, PIC remapped, syscalls on int 0x80");

    boot_stage("physical memory allocator");
    pmm_init();
    struct pmm_stats ps;
    pmm_stats(&ps);
    ok("Physical memory: %u MB usable (%u E820 regions)",
       ps.total_frames / 256, e820_count());
    ok("PMM: managed end=%x, free frames=%u",ps.managed_limit,ps.total_frames-ps.used_frames);
    boot_stage("paging and kernel mappings");
    paging_init();
    ok("Paging enabled: RAM identity-mapped up to %u MB, NULL page unmapped",
       paging_identity_limit() >> 20);
    boot_stage("kernel heap");
    heap_init();
    ok("Kernel heap at %p", HEAP_START);
    boot_stage("framebuffer and graphical console");
    vga_save_text();
    if (fb_init()) {
        struct fb_info fi;
        fb_get_info(&fi);
        vga_switch_to_fb();
        ok("Graphics: %s, %u MB VRAM, %ux%u, %u bpp -> console %ux%u",
           fi.driver, fi.vram >> 20, fi.width, fi.height, fi.bpp, vga_cols(), vga_rows());
        ok("Framebuffer: base=%x pitch=%u caps=%x",fi.phys,fi.pitch,fi.caps);
        if (fi.caps & FB_CAP_COMPOSITOR)
            ok("GPU: VirGL textures, shader alpha composition, buffered frames");
        else if (fi.caps & FB_CAP_GPU)
            ok("GPU 2D acceleration: rect copy, fill%s", fi.caps & FB_CAP_CURSOR ? ", hardware cursor" : "");
    } else {
        ok("Graphics: no supported framebuffer adapter - staying in VGA text mode");
    }
    boot_stage("FPU and task scheduler");
    fpu_init();
    tasking_init();
    ok("Multitasking: preemptive round robin, %u ms time slice", TIME_SLICE * 10);

    boot_stage("timer and PS/2 input");
    timer_init();
    keyboard_init();
    bool have_mouse = mouse_init();
    if (fb_ready()) { struct fb_info fi; fb_get_info(&fi); mouse_set_bounds(fi.width, fi.height); }
    boot_stage("VirtIO pointer");
    if (virtinput_init()) {
        if (fb_ready()) { struct fb_info fi; fb_get_info(&fi); mouse_set_bounds(fi.width, fi.height); }
        ok("Input: VirtIO absolute pointer (%s)", virtinput_interrupts() ? "interrupts" : "timer fallback");
    }
    __asm__ volatile ("sti");
    ok("PIT timer 100 Hz, PS/2 keyboard%s, FPU, interrupts on", have_mouse ? ", PS/2 mouse" : "");

#ifdef MYOS_HARDWARE
    boot_stage("xHCI and USB HID input");
    if (xhci_init()) ok("USB HID input ready");
    else fail("USB HID", "no supported direct keyboard/mouse; PS/2 or BIOS legacy may still work");
    if (boot_info_get()) ok("LIVE MODE: filesystem changes stay in RAM; internal ATA disks are hidden");
#endif

    boot_stage("network adapter");
    if (net_init())
        ok("Network: %s, MAC %02x:%02x:%02x:%02x:%02x:%02x, DHCP in background",
           e1000_name(),netcfg.mac[0], netcfg.mac[1], netcfg.mac[2], netcfg.mac[3], netcfg.mac[4], netcfg.mac[5]);
    else
        fail("Network", "adapter unavailable; use dmesg");

    boot_stage("storage probe");
    if (ata_init()) {
        ok("ATA disk: %s, %u KB", ata_model(), ata_sectors() / 2);
        bool formatted;
        boot_stage("MyFS mount");
        int r = fs_init(&formatted);
        if (r == FS_OK) {
            struct fs_info fi;
            fs_info(&fi);
            ok("MyFS %s, %u KB free",
               formatted ? "created (new disk formatted)" : "mounted", fi.free_blocks / 2);
            struct fb_info before, after;
            fb_get_info(&before);
            boot_stage("saved display settings");
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

    boot_stage("SMP processor startup");
    smp_init();
    ok("SMP initialization complete: %u CPU(s)",smp_count());
    boot_stage("services");
    kprintf("Kernel log: 16 KiB memory ring; use dmesg\n");
    services_init();
    boot_stage("kernel ready; entering login/shell");
    shell_run();
}
