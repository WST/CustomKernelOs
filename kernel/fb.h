#ifndef FB_H
#define FB_H
#include "types.h"

/* Графический режим через интерфейс Bochs VBE (QEMU -vga std, Bochs, VirtualBox). */

#define FB_WIDTH   1024
#define FB_HEIGHT  768
#define FB_BPP     32
#define FB_VIRT    0xE0000000u     /* куда в адресном пространстве ядра отображена видеопамять */
#define FONT_W     8
#define FONT_H     16

struct fb_info {
    u32 width, height, bpp, pitch;  /* pitch — байт в строке */
    u32 phys;                       /* физический адрес видеопамяти */
};

bool fb_init(void);                 /* найти карту, забрать шрифт, включить режим */
bool fb_ready(void);
void fb_get_info(struct fb_info *out);
const u8 *fb_font(void);            /* 256 символов по 16 байт */

/* для графической консоли */
void fb_glyph(int col, int row, u8 ch, u32 fg, u32 bg);
void fb_fill(int x, int y, int w, int h, u32 color);
void fb_scroll(int pixels, u32 bg);  /* сдвинуть всё изображение вверх */
/* для программ: скопировать прямоугольник из буфера (stride пикселей в строке) */
void fb_blit(const u32 *src, u32 stride, int x, int y, int w, int h);

/* программа, которая рисует сама (консоль на это время молчит) */
int  fb_acquire(u32 pid, bool on);  /* 0 ок, -1 нет графики, -2 занято */
bool fb_is_owner(u32 pid);
void fb_release(u32 pid);           /* процесс умер — вернуть консоль */

#endif
