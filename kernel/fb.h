#ifndef FB_H
#define FB_H
#include "types.h"

/* Графика. Два драйвера видеокарты:
 *   VMware SVGA II (QEMU -vga vmware, VirtualBox VMSVGA, VMware) — настоящий
 *     GPU-интерфейс: очередь команд FIFO, аппаратные заливка и копирование
 *     прямоугольников, аппаратный курсор;
 *   Bochs VBE (QEMU -vga std, Bochs) — просто линейная видеопамять, всё
 *     «ускорение» делает процессор.
 * Разрешение меняется на лету: fb_set_mode(). */

#define FB_DEF_W   1024
#define FB_DEF_H   768
#define FB_MAX_W   2560
#define FB_MAX_H   1600
#define FB_BPP     32
#define FB_VIRT    0xE0000000u     /* видеопамять в адресном пространстве ядра (до 16 МБ) */
#define FB_VIRT_SZ 0x01000000u
#define FIFO_VIRT  0xE2000000u     /* очередь команд SVGA (до 256 КБ) */
#define FONT_W     8
#define FONT_H     16

/* что умеет видеокарта */
#define FB_CAP_COPY    1           /* копирование прямоугольника на экране */
#define FB_CAP_FILL    2           /* заливка прямоугольника */
#define FB_CAP_CURSOR  4           /* аппаратный курсор мыши */
#define FB_CAP_GPU     8           /* копирование/заливку делает сама карта, а не процессор */
#define FB_CAP_GPU_ON  16          /* ...и в текущем режиме это проверено и работает */
#define FB_CAP_CURSOR_ON 32        /* аппаратный курсор сейчас включён */

struct fb_info {
    u32 width, height, bpp, pitch;  /* pitch — байт в строке */
    u32 phys;                       /* физический адрес видеопамяти */
    u32 vram;                       /* байт видеопамяти */
    u32 max_w, max_h;
    u32 caps;                       /* FB_CAP_* */
    u32 gen;                        /* +1 при каждой смене режима */
    const char *driver;
    u32 hwcaps;                     /* регистр возможностей карты (SVGA) */
};

bool fb_init(void);                 /* найти карту, забрать шрифт, включить режим */
bool fb_ready(void);
void fb_get_info(struct fb_info *out);
const u8 *fb_font(void);            /* 256 символов по 16 байт */
int  fb_set_mode(u32 w, u32 h);     /* 0 или -1 (не поддерживается) */
bool fb_mode_ok(u32 w, u32 h);

/* рисование (координаты обрезаются по экрану) */
void fb_glyph(int col, int row, u8 ch, u32 fg, u32 bg);
void fb_fill(int x, int y, int w, int h, u32 color);           /* на GPU, если умеет */
void fb_copy(int sx, int sy, int dx, int dy, int w, int h);    /* на GPU, если умеет */
void fb_scroll(int pixels, u32 bg);  /* сдвинуть всё изображение вверх */
/* скопировать прямоугольник из буфера программы (stride пикселей в строке) */
void fb_blit(const u32 *src, u32 stride, int x, int y, int w, int h);
void fb_tick(void);                 /* из таймера: показать накопленные изменения */

/* аппаратный курсор: картинка 0xAARRGGBB (A = 0 — прозрачно), до 32x32 */
int  fb_cursor_define(const u32 *argb, int w, int h, int hot_x, int hot_y);
int  fb_cursor_enable(bool on);     /* 0 или -1 (нет аппаратного курсора) */
bool fb_cursor_enabled(void);
void fb_cursor_move(int x, int y);  /* из прерывания мыши */

/* программа, которая рисует сама (консоль на это время молчит) */
int  fb_acquire(u32 pid, bool on);  /* 0 ок, -1 нет графики, -2 занято */
bool fb_is_owner(u32 pid);
void fb_release(u32 pid);           /* процесс умер — вернуть консоль */

#endif
