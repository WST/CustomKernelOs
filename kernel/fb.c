/* Графика: видеорежим 1024x768x32 через Bochs VBE (BGA).
 *
 * У видеокарты QEMU есть простой интерфейс: два порта, 0x1CE (номер регистра)
 * и 0x1CF (значение). Записали ширину, высоту, глубину цвета, включили — и
 * видеопамять превращается в линейный массив пикселей (LFB): пиксель (x, y)
 * лежит по адресу LFB + y * pitch + x * 4 в формате 0x00RRGGBB.
 *
 * Где эта память — говорит PCI: регистр BAR0 видеокарты. Она выше 1 ГБ, куда
 * наша 1:1-карта не достаёт, поэтому отображаем её отдельно на FB_VIRT. */
#include "fb.h"
#include "pci.h"
#include "paging.h"
#include "string.h"
#include "vga.h"

#define VBE_INDEX   0x01CE
#define VBE_DATA    0x01CF
#define VBE_ID      0
#define VBE_XRES    1
#define VBE_YRES    2
#define VBE_BPP     3
#define VBE_ENABLE  4
#define VBE_VIRT_W  6
#define VBE_VIRT_H  7
#define VBE_ENABLED     0x01
#define VBE_LFB_ENABLED 0x40

static bool ready;
static struct fb_info info;
static volatile u32 *fb;
static u8 font[256 * FONT_H];

static void vbe_write(u16 idx, u16 val) { outw(VBE_INDEX, idx); outw(VBE_DATA, val); }
static u16  vbe_read(u16 idx) { outw(VBE_INDEX, idx); return inw(VBE_DATA); }

/* Шрифт текстового режима живёт в «плоскости 2» видеопамяти: 256 символов
 * по 32 байта (используются первые 16). Чтобы прочитать его, на время
 * перенастраиваем секвенсор и графический контроллер VGA так, чтобы по адресу
 * 0xA0000 была видна именно плоскость 2 — а потом возвращаем как было. */
static void grab_font(void) {
    outw(0x3C4, 0x0402);    /* запись — только в плоскость 2 */
    outw(0x3C4, 0x0704);    /* последовательная адресация */
    outw(0x3CE, 0x0204);    /* чтение из плоскости 2 */
    outw(0x3CE, 0x0005);    /* выключить режим чёт/нечет */
    outw(0x3CE, 0x0406);    /* окно 0xA0000, 64 КБ */
    const volatile u8 *src = (const volatile u8 *)0xA0000;
    for (int c = 0; c < 256; c++)
        for (int r = 0; r < FONT_H; r++)
            font[c * FONT_H + r] = src[c * 32 + r];
    outw(0x3C4, 0x0302);    /* вернуть настройки текстового режима */
    outw(0x3C4, 0x0304);
    outw(0x3CE, 0x0004);
    outw(0x3CE, 0x1005);
    outw(0x3CE, 0x0E06);
}

bool fb_init(void) {
    if (vbe_read(VBE_ID) < 0xB0C0) return false;          /* нет Bochs VBE */

    /* видеокарта = PCI-устройство класса 3 (display); её память — BAR0 */
    struct pci_dev d;
    u32 phys = 0xE0000000u;                                /* адрес по умолчанию у Bochs */
    if (pci_find_class(0x03, 0x00, &d) && (d.bar0 & ~0xFu)) phys = d.bar0 & ~0xFu;

    grab_font();

    vbe_write(VBE_ENABLE, 0);
    vbe_write(VBE_XRES, FB_WIDTH);
    vbe_write(VBE_YRES, FB_HEIGHT);
    vbe_write(VBE_BPP, FB_BPP);
    vbe_write(VBE_VIRT_W, FB_WIDTH);
    vbe_write(VBE_VIRT_H, FB_HEIGHT);
    vbe_write(VBE_ENABLE, VBE_ENABLED | VBE_LFB_ENABLED);
    if (vbe_read(VBE_XRES) != FB_WIDTH || vbe_read(VBE_BPP) != FB_BPP) {
        vbe_write(VBE_ENABLE, 0);
        return false;
    }

    info.width = FB_WIDTH;
    info.height = FB_HEIGHT;
    info.bpp = FB_BPP;
    info.pitch = FB_WIDTH * 4;
    info.phys = phys;

    /* отобразить видеопамять в ядро — до создания процессов, тогда эти записи
     * каталога попадут и во все каталоги процессов */
    u32 size = info.pitch * info.height;
    for (u32 off = 0; off < size; off += 4096)
        if (vmm_map(FB_VIRT + off, phys + off, PG_WRITE)) return false;
    fb = (volatile u32 *)FB_VIRT;
    ready = true;
    fb_fill(0, 0, info.width, info.height, 0);
    return true;
}

bool fb_ready(void) { return ready; }
void fb_get_info(struct fb_info *out) { *out = info; }
const u8 *fb_font(void) { return font; }

void fb_glyph(int col, int row, u8 ch, u32 fg, u32 bg) {
    volatile u32 *p = fb + row * FONT_H * FB_WIDTH + col * FONT_W;
    const u8 *g = font + ch * FONT_H;
    for (int y = 0; y < FONT_H; y++, p += FB_WIDTH) {
        u8 bits = g[y];
        p[0] = bits & 0x80 ? fg : bg;  p[1] = bits & 0x40 ? fg : bg;
        p[2] = bits & 0x20 ? fg : bg;  p[3] = bits & 0x10 ? fg : bg;
        p[4] = bits & 0x08 ? fg : bg;  p[5] = bits & 0x04 ? fg : bg;
        p[6] = bits & 0x02 ? fg : bg;  p[7] = bits & 0x01 ? fg : bg;
    }
}

void fb_fill(int x, int y, int w, int h, u32 color) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > FB_WIDTH)  w = FB_WIDTH - x;
    if (y + h > FB_HEIGHT) h = FB_HEIGHT - y;
    for (int j = 0; j < h; j++) {
        volatile u32 *p = fb + (y + j) * FB_WIDTH + x;
        for (int i = 0; i < w; i++) p[i] = color;
    }
}

/* быстрые копирования по 4 байта: rep movsl в сотни раз быстрее байтового цикла */
static inline void copy32(volatile void *dst, const volatile void *src, u32 words) {
    __asm__ volatile ("cld; rep movsl" : "+D"(dst), "+S"(src), "+c"(words) : : "memory");
}

void fb_scroll(int pixels, u32 bg) {
    u32 keep = (FB_HEIGHT - pixels) * FB_WIDTH;
    copy32(fb, fb + pixels * FB_WIDTH, keep);
    fb_fill(0, FB_HEIGHT - pixels, FB_WIDTH, pixels, bg);
}

void fb_blit(const u32 *src, u32 stride, int x, int y, int w, int h) {
    if (x < 0 || y < 0 || w <= 0 || h <= 0) return;
    if (x + w > FB_WIDTH)  w = FB_WIDTH - x;
    if (y + h > FB_HEIGHT) h = FB_HEIGHT - y;
    for (int j = 0; j < h; j++)
        copy32(fb + (y + j) * FB_WIDTH + x, src + (y + j) * stride + x, w);
}

/* ---------------------------------------------------------------- владелец экрана
 * Пока программа рисует сама, консоль не должна поверх неё печатать. */
static u32 owner;

int fb_acquire(u32 pid, bool on) {
    if (!ready) return -1;
    if (on) {
        if (owner && owner != pid) return -2;     /* экран уже занят другим */
        owner = pid;
        vga_suspend(true);
    } else if (owner == pid) {
        owner = 0;
        vga_suspend(false);
    }
    return 0;
}

bool fb_is_owner(u32 pid) { return owner && owner == pid; }

void fb_release(u32 pid) {
    if (owner && owner == pid) { owner = 0; vga_suspend(false); }
}
