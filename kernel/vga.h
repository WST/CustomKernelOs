#ifndef VGA_H_
#define VGA_H_
#include "types.h"

enum vga_color {
    BLACK, BLUE, GREEN, CYAN, RED, MAGENTA, BROWN, LIGHT_GREY,
    DARK_GREY, LIGHT_BLUE, LIGHT_GREEN, LIGHT_CYAN, LIGHT_RED,
    LIGHT_MAGENTA, YELLOW, WHITE
};

void vga_init(void);
void vga_clear(void);
void vga_set_color(u8 fg, u8 bg);
void vga_set_default_color(u8 fg, u8 bg);   /* цвет, к которому ведёт ESC[0m */
void vga_reset_terminal(void);
void vga_save_text(void);                   /* снимок текстового экрана (до fb_init) */
void vga_switch_to_fb(void);                /* перейти на графическую консоль 128x48 */
void vga_resize(void);                       /* сменилось разрешение экрана */
void vga_suspend(bool suspend);              /* программа рисует сама — консоль не трогает экран */
int  vga_cols(void);
int  vga_rows(void);
bool vga_graphics(void);              /* сброс ANSI-состояния и показ курсора */
void vga_putc(char c);
void vga_puts(const char *s);
void vga_write_at(int x, int y, const char *s, u8 fg, u8 bg);
void kprintf(const char *fmt, ...);
void vkprintf(const char *fmt, va_list ap);

#endif
