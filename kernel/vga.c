/* Консоль: текстовый режим VGA 80x25 или графическая консоль 128x48.
 *
 * Экран хранится как сетка «ячеек» по 2 байта (код символа + цвет), как в
 * текстовом режиме VGA. В текстовом режиме сетка — это сама видеопамять 0xB8000.
 * В графическом — массив в памяти ядра, а каждую изменённую ячейку мы
 * рисуем шрифтом 8x16 в видеопамять (fb.c). Всё остальное — ANSI, прокрутка,
 * kprintf — одинаково в обоих режимах. */
#include "vga.h"
#include "string.h"
#include "klog.h"
#include "fb.h"
#include "bootinfo.h"

#define MAX_W   (FB_MAX_W / FONT_W)     /* 320 */
#define MAX_H   (FB_MAX_H / FONT_H)     /* 100 */
#define VGA_MEM ((volatile u16 *)0xB8000)

static volatile u16 *cells = VGA_MEM;
static u16  shadow[MAX_W * MAX_H];
static u16  saved_text[80 * 25];        /* текстовый экран до включения графики */
static int  PW, PH;                     /* размер экрана в пикселях (графика) */
static int  W = 80, H = 25;
static bool fbmode;                     /* рисуем пикселями */
static bool firmware_console;           /* avoid slow reads of physical VBE VRAM */
static unsigned cursor_defer;           /* draw the final cursor once per output */
static u32 dirty_cells[(MAX_W*MAX_H+31)/32];
static bool suspended;                  /* экран захватила программа — только запоминаем */
static bool cur_on = true;              /* видим ли курсор */
static int  cur_x = -1, cur_y = -1;     /* где нарисован курсор (графика) */

static int col, row;
static u8  attr = (BLACK << 4) | LIGHT_GREY;
static u8  default_attr;
static int esc_state;
static void cursor_visible(bool on);

/* классическая палитра VGA в RGB */
static const u32 palette[16] = {
    0x000000, 0x0000AA, 0x00AA00, 0x00AAAA, 0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
    0x555555, 0x5555FF, 0x55FF55, 0x55FFFF, 0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF,
};

static inline u16 cell(char c) { return (u16)(u8)c | ((u16)attr << 8); }

static void draw_cell(int i) {
    u16 v = cells[i];
    u8 a = v >> 8;
    fb_glyph(i % W, i / W, v & 0xFF, palette[a & 15], palette[a >> 4]);
}

static void paint_cell(int i) {
    if (firmware_console && cursor_defer) dirty_cells[i/32]|=1u<<(i%32);
    else draw_cell(i);
}

static void flush_cells(void) {
    if (!firmware_console || !fbmode || suspended) return;
    for (int i=0;i<W*H;i++) if (dirty_cells[i/32]&(1u<<(i%32))) {
        dirty_cells[i/32]&=~(1u<<(i%32));
        draw_cell(i);
    }
}

static inline void set_cell(int i, u16 v) {
    if (cells[i]==v) return;
    cells[i] = v;
    if (fbmode && !suspended) {
        paint_cell(i);
        if (i == cur_y * W + cur_x) cur_x = cur_y = -1;   /* затёрли курсор — нарисуем заново */
    }
}

/* курсор в графике: подчёркивание цветом символа под ним */
static void erase_cursor(void) {
    if (cur_x >= 0 && !suspended) paint_cell(cur_y * W + cur_x);
    cur_x = cur_y = -1;
}

static void update_cursor(void) {
    if (cursor_defer) return;
    flush_cells();
    if (!fbmode) {
        u16 pos = row * W + col;
        outb(0x3D4, 0x0F); outb(0x3D5, pos & 0xFF);
        outb(0x3D4, 0x0E); outb(0x3D5, pos >> 8);
        return;
    }
    if (suspended) return;
    if (cur_x == col && cur_y == row) return;
    erase_cursor();
    if (!cur_on || col >= W || row >= H) return;
    u8 a = cells[row * W + col] >> 8;
    fb_fill(col * FONT_W, row * FONT_H + FONT_H - 3, FONT_W, 2, palette[(a & 15) ? (a & 15) : 7]);
    cur_x = col; cur_y = row;
}

static void scroll(void) {
    if (row < H) return;
    if (fbmode) erase_cursor();
    /* VBE video memory is uncached MMIO. Scrolling pixels reads almost the
     * whole screen over the display bus for every newline. Compare the RAM
     * text cells instead and only write glyphs whose character/color changed. */
    bool redraw=firmware_console && fbmode && !suspended;
    for (int i = 0; i < H * W; i++) {
        u16 next=i<(H-1)*W ? cells[i+W] : cell(' ');
        if (cells[i]==next) continue;
        cells[i]=next;
        if (redraw) paint_cell(i);
    }
    if (fbmode && !suspended && !redraw) fb_scroll(FONT_H, palette[attr >> 4]);
    row = H - 1;
}

void vga_set_color(u8 fg, u8 bg) { attr = (bg << 4) | (fg & 0x0F); }
void vga_set_default_color(u8 fg, u8 bg) { default_attr = (bg << 4) | (fg & 0x0F); }
void vga_reset_terminal(void) { u32 f = irq_save(); esc_state = 0; attr = default_attr; cursor_visible(true); irq_restore(f); }

static void clear_nolock(void) {
    for (int i = 0; i < W * H; i++) cells[i] = cell(' ');
    if (fbmode && !suspended) { cur_x = -1; fb_fill(0, 0, PW, PH, palette[attr >> 4]); }
    col = row = 0;
    update_cursor();
}

void vga_init(void) {
    /* The hardware loader may already be in VBE graphics mode. Its VGA
     * aperture is not reliable text storage; preserve early output in RAM. */
    const struct boot_info *boot=boot_info_get();
    firmware_console=boot && boot->fb_phys;
    if (firmware_console) cells=saved_text;
    /* включаем «толстый» курсор: строки 13..15 */
    outb(0x3D4, 0x0A); outb(0x3D5, (inb(0x3D5) & 0xC0) | 13);
    outb(0x3D4, 0x0B); outb(0x3D5, (inb(0x3D5) & 0xE0) | 15);
    default_attr = attr;
    clear_nolock();
}

/* перерисовать весь экран из сетки ячеек */
static void repaint(void) {
    for (int i = 0; i < W * H; i++) draw_cell(i);
    cur_x = -1;
    update_cursor();
}

/* Запомнить текстовый экран ДО включения графики: после переключения
 * видеокарта перестаёт показывать память 0xB8000, и прочитать её уже нельзя. */
void vga_save_text(void) {
    u32 f = irq_save();
    for (int i = 0; i < 80 * 25; i++) saved_text[i] = cells[i];
    irq_restore(f);
}

static void fb_dims(void) {
    struct fb_info fi;
    fb_get_info(&fi);
    PW = fi.width; PH = fi.height;
}

/* Перейти на графическую консоль: сохранённый текст — в новую, большую сетку */
void vga_switch_to_fb(void) {
    u32 f = irq_save();
    fb_dims();
    int nw = PW / FONT_W, nh = PH / FONT_H;
    for (int i = 0; i < nw * nh; i++) shadow[i] = (u16)' ' | ((u16)default_attr << 8);
    for (int y = 0; y < 25 && y < nh; y++)
        for (int x = 0; x < 80 && x < nw; x++) shadow[y * nw + x] = saved_text[y * 80 + x];
    cells = shadow;
    W = nw;
    H = nh;
    fbmode = true;
    repaint();
    irq_restore(f);
}

/* Разрешение сменилось: новая сетка ячеек. Текст переносим как есть (лишние
 * столбцы обрезаются); если строк стало меньше — сохраняем нижние, где курсор. */
void vga_resize(void) {
    if (!fbmode) return;
    static u16 tmp[MAX_W * MAX_H];
    u32 f = irq_save();
    fb_dims();
    int nw = PW / FONT_W, nh = PH / FONT_H;
    int off = row + 1 > nh ? row + 1 - nh : 0;
    for (int i = 0; i < nw * nh; i++) tmp[i] = (u16)' ' | ((u16)default_attr << 8);
    for (int y = 0; y < nh && y + off < H; y++)
        for (int x = 0; x < nw && x < W; x++) tmp[y * nw + x] = shadow[(y + off) * W + x];
    memcpy(shadow, tmp, nw * nh * 2);
    W = nw; H = nh;
    row -= off;
    if (col >= W) col = W - 1;
    cur_x = cur_y = -1;
    if (!suspended) {
        fb_fill(0, 0, PW, PH, palette[default_attr >> 4]);
        repaint();
    }
    irq_restore(f);
}

/* программа захватила экран (suspend = true) или отдала обратно */
void vga_suspend(bool suspend) {
    u32 f = irq_save();
    if (suspended != suspend) {
        suspended = suspend;
        if (!suspend && fbmode) { fb_fill(0, 0, PW, PH, palette[default_attr >> 4]); repaint(); }
    }
    irq_restore(f);
}

int vga_cols(void) { return W; }
int vga_rows(void) { return H; }
bool vga_graphics(void) { return fbmode; }

/* ---------------------------------------------------------------- ANSI
 * Подмножество escape-последовательностей VT100/xterm, как в терминале Linux:
 *   ESC[r;cH  курсор в строку r, столбец c (с 1)      ESC[A/B/C/D  вверх/вниз/вправо/влево
 *   ESC[2J    очистить экран        ESC[J  до конца экрана        ESC[K  до конца строки
 *   ESC[...m  цвета: 0 сброс, 1 ярко, 7 инверсия, 30-37/90-97 текст, 40-47 фон
 *   ESC[?25l / ESC[?25h  спрятать / показать курсор
 * Программам не нужно знать про видеопамять — они просто печатают. */

static const u8 ansi_to_vga[8] = { BLACK, RED, GREEN, BROWN, BLUE, MAGENTA, CYAN, LIGHT_GREY };
/* default_attr, esc_state объявлены выше; esc_state: 0 — текст, 1 — был ESC, 2 — внутри ESC[ */
static int esc_args[4], esc_n;
static bool esc_private;            /* ESC[? */

static void cursor_visible(bool on) {
    if (fbmode) {
        cur_on = on;
        if (!on) erase_cursor();
        update_cursor();
        return;
    }
    outb(0x3D4, 0x0A);
    u8 v = inb(0x3D5);
    outb(0x3D5, on ? (v & ~0x20) : (v | 0x20));
}

static void sgr(int n) {
    u8 fg = attr & 0x0F, bg = attr >> 4;
    if (n == 0) { attr = default_attr; return; }
    if (n == 1) fg |= 8;
    else if (n == 7) { u8 t = fg; fg = bg; bg = t & 7; }
    else if (n == 22) fg &= 7;
    else if (n >= 30 && n <= 37) fg = (fg & 8) | ansi_to_vga[n - 30];
    else if (n == 39) fg = default_attr & 0x0F;
    else if (n >= 40 && n <= 47) bg = ansi_to_vga[n - 40];
    else if (n == 49) bg = default_attr >> 4;
    else if (n >= 90 && n <= 97) fg = ansi_to_vga[n - 90] | 8;
    else if (n >= 100 && n <= 107) bg = ansi_to_vga[n - 100];
    attr = (bg << 4) | (fg & 0x0F);
}

static void clear_range(int from, int to) {
    if (fbmode) erase_cursor();
    for (int i = from; i < to; i++) set_cell(i, cell(' '));
}

/* -> true, если символ съеден escape-последовательностью */
static bool ansi(char c) {
    if (esc_state == 0) {
        if (c != 27) return false;
        esc_state = 1;
        return true;
    }
    if (esc_state == 1) {
        if (c == '[') { esc_state = 2; esc_n = 0; esc_private = false; memset(esc_args, 0, sizeof(esc_args)); }
        else esc_state = 0;
        return true;
    }
    /* esc_state == 2: параметры и финальная буква */
    if (c == '?') { esc_private = true; return true; }
    if (c >= '0' && c <= '9') {
        if (esc_n == 0) esc_n = 1;
        /* Saturate before arithmetic: an oversized CSI must never wrap into
         * a negative cursor position or an out-of-bounds cell index. */
        int *value=&esc_args[esc_n-1];
        if (*value<=6553) {
            int next=*value*10+(c-'0');
            *value=next>65535?65535:next;
        } else *value=65535;
        return true;
    }
    if (c == ';') { if (esc_n == 0) esc_n = 1; if (esc_n < 4) esc_n++; return true; }

    esc_state = 0;
    int a = esc_n ? esc_args[0] : 0, b = esc_n > 1 ? esc_args[1] : 0;
    int n = a ? a : 1;
    switch (c) {
    case 'H': case 'f':
        row = (a ? a : 1) - 1; col = (b ? b : 1) - 1;
        if (row >= H) row = H - 1;
        if (col >= W) col = W - 1;
        break;
    case 'A': row = row - n < 0 ? 0 : row - n; break;
    case 'B': row = row + n >= H ? H - 1 : row + n; break;
    case 'C': col = col + n >= W ? W - 1 : col + n; break;
    case 'D': col = col - n < 0 ? 0 : col - n; break;
    case 'G': col = (n > W ? W : n) - 1; break;
    case 'J':
        if (a == 2) { clear_range(0, W * H); }
        else clear_range(row * W + col, W * H);
        break;
    case 'K': clear_range(row * W + col, (row + 1) * W); break;
    case 'm':
        if (!esc_n) sgr(0);
        for (int i = 0; i < esc_n; i++) sgr(esc_args[i]);
        break;
    case 'h': case 'l':
        if (esc_private && a == 25) cursor_visible(c == 'h');
        break;
    }
    update_cursor();
    return true;
}

static void putc_nolock(char c) {
    outb(0x3F8, (u8)c);                          /* копия на COM1: в QEMU видно с -serial stdio/file */
    if (ansi(c)) return;
    switch (c) {
    case '\n': col = 0; row++; break;
    case '\r': col = 0; break;
    case '\f': clear_nolock(); return;           /* form feed: очистить экран (так работает clear) */
    case '\t': col = (col + 4) & ~3; break;
    case '\b':
        if (col > 0) col--;
        else if (row > 0) { row--; col = W - 1; }
        set_cell(row * W + col, cell(' '));
        break;
    default:
        set_cell(row * W + col, cell(c));
        col++;
    }
    if (col >= W) { col = 0; row++; }
    scroll();
    update_cursor();
}

/* Экран общий для всех задач: каждую операцию делаем целиком, без переключений,
 * чтобы строки разных задач не перемешивались посреди kprintf. */
void vga_putc(char c)        { u32 f = irq_save(); putc_nolock(c); irq_restore(f); }
void vga_write(const char *s,u32 length) {
    u32 f=irq_save(); cursor_defer++;
    for (u32 i=0;i<length;i++) putc_nolock(s[i]);
    cursor_defer--; update_cursor(); irq_restore(f);
}
void vga_puts(const char *s) {
    u32 f=irq_save(); cursor_defer++;
    while (*s) putc_nolock(*s++);
    cursor_defer--; update_cursor(); irq_restore(f);
}
void vga_clear(void)         { u32 f = irq_save(); clear_nolock(); irq_restore(f); }

/* написать строку в фиксированное место экрана, не трогая курсор */
void vga_write_at(int x, int y, const char *s, u8 fg, u8 bg) {
    u32 f = irq_save();
    u16 a = (u16)((bg << 4) | (fg & 0x0F)) << 8;
    for (; *s && x < W; s++, x++) set_cell(y * W + x, (u8)*s | a);
    irq_restore(f);
}

/* ---- kprintf: %s %c %d %u %x %p %% и ширина вида %08x / %5d ---- */
static void print_num(u32 v, int base, bool neg, int width, char pad,void (*emit)(char)) {
    char buf[16];
    int i = 0;
    const char *digits = "0123456789abcdef";
    do { buf[i++] = digits[v % base]; v /= base; } while (v);
    if (neg) buf[i++] = '-';
    while (i < width) buf[i++] = pad;
    while (i) emit(buf[--i]);
}

static void emit_string(const char *s,void (*emit)(char)) {
    while(*s)emit(*s++);
}
static void format(const char *fmt,va_list ap,void (*emit)(char)) {
    for (; *fmt; fmt++) {
        if (*fmt != '%') { emit(*fmt); continue; }
        fmt++;
        char pad = ' ';
        int width = 0;
        if (*fmt == '0') { pad = '0'; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        switch (*fmt) {
        case 's': { const char *s = va_arg(ap, const char *); emit_string(s ? s : "(null)",emit); break; }
        case 'c': emit((char)va_arg(ap, int)); break;
        case 'd': { int v = va_arg(ap, int);
                    print_num(v < 0 ? -(u32)v : (u32)v, 10, v < 0, width, pad,emit); break; }
        case 'u': print_num(va_arg(ap, u32), 10, false, width, pad,emit); break;
        case 'x': print_num(va_arg(ap, u32), 16, false, width, pad,emit); break;
        case 'p': emit_string("0x",emit); print_num(va_arg(ap, u32), 16, false, 8, '0',emit); break;
        case '%': emit('%'); break;
        default:  emit('%'); emit(*fmt);
        }
    }
}
static void console_log(char c) {
    klog_write(&c,1);vga_putc(c);
}
static void quiet_log(char c) {
    klog_write(&c,1);outb(0x3f8,(u8)c);
}
void klogf(const char *fmt,...) {
    va_list ap;u32 flags=irq_save();
    va_start(ap,fmt);format(fmt,ap,quiet_log);va_end(ap);
    irq_restore(flags);
}
void kprintf(const char *fmt,...) {
    va_list ap;va_start(ap,fmt);vkprintf(fmt,ap);va_end(ap);
}
void vkprintf(const char *fmt,va_list ap) {
    u32 flags=irq_save();cursor_defer++;
    format(fmt,ap,console_log);
    cursor_defer--;update_cursor();irq_restore(flags);
}
