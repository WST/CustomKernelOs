/* Смена разрешения экрана целиком: видеокарта, консоль, границы мыши.
 * Выбор пользователя хранится в файле /etc/display (строка «1280x720»),
 * его читает ядро при загрузке, а пишут программы mode и Display. */
#include "display.h"
#include "fb.h"
#include "vga.h"
#include "mouse.h"
#include "fs.h"

int display_set_mode(u32 w, u32 h) {
    if (!fb_ready()) return -1;
    struct fb_info fi;
    fb_get_info(&fi);
    if (fi.width == w && fi.height == h) return 0;
    if (fb_set_mode(w, h) < 0) return -1;
    vga_resize();
    mouse_set_bounds(w, h);
    return 0;
}

/* «1280x720» -> w, h */
bool display_parse(const char *s, u32 *w, u32 *h) {
    u32 a = 0, b = 0;
    while (*s == ' ' || *s == '\t') s++;
    if (*s < '0' || *s > '9') return false;
    while (*s >= '0' && *s <= '9') a = a * 10 + (*s++ - '0');
    if (*s != 'x' && *s != 'X') return false;
    s++;
    if (*s < '0' || *s > '9') return false;
    while (*s >= '0' && *s <= '9') b = b * 10 + (*s++ - '0');
    *w = a; *h = b;
    return true;
}

void display_load_config(void) {
    if (!fb_ready() || !fs_ready()) return;
    u32 ino;
    if (fs_resolve(FS_ROOT, "/etc/display", &ino) != FS_OK) return;
    char buf[64];
    int n = fs_read(ino, 0, buf, sizeof(buf) - 1);
    if (n <= 0) return;
    buf[n] = 0;
    u32 w, h;
    if (display_parse(buf, &w, &h)) display_set_mode(w, h);
}
