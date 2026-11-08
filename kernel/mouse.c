/* Мышь PS/2. Висит на том же контроллере 8042, что и клавиатура, но как
 * «второе устройство» (aux), и прерывает по IRQ12.
 *
 * Мышь присылает пакеты по 3 байта:
 *   байт 0: биты кнопок (0 левая, 1 правая, 2 средняя), бит 3 всегда 1,
 *           биты 4/5 — знаки смещений X/Y, биты 6/7 — переполнение
 *   байт 1: смещение по X      байт 2: смещение по Y (вверх — положительное)
 * Координаты мы складываем сами: мышь сообщает только «насколько сдвинулась». */
#include "mouse.h"
#include "idt.h"
#include "fb.h"

static struct mouse_state st;
static i32 max_x = 1023, max_y = 767;
static u8  packet[3];
static int idx;

static bool wait_write(void) { for (int i = 0; i < 100000; i++) if (!(inb(0x64) & 2)) return true; return false; }
static bool wait_read(void)  { for (int i = 0; i < 100000; i++) if (inb(0x64) & 1) return true; return false; }

static void cmd(u8 c)  { wait_write(); outb(0x64, c); }
static void data(u8 d) { wait_write(); outb(0x60, d); }

/* команда самой мыши: через 0xD4 «следующий байт — для aux-устройства» */
static bool mouse_cmd(u8 c) {
    cmd(0xD4);
    data(c);
    return wait_read() && inb(0x60) == 0xFA;    /* ACK */
}

static void mouse_irq(struct regs *r) {
    (void)r;
    /* Читаем, только если в контроллере правда лежит байт от мыши. Иначе
     * «запоздалое» прерывание от ACK времён инициализации прочитало бы из
     * пустого порта старый байт 0xFA — и все пакеты сдвинулись бы на байт. */
    u8 status = inb(0x64);
    if (!(status & 0x01) || !(status & 0x20)) return;
    u8 b = inb(0x60);
    if (idx == 0 && !(b & 0x08)) return;        /* потеряли синхронизацию — ждём начала пакета */
    packet[idx++] = b;
    if (idx < 3) return;
    idx = 0;
    if (packet[0] & 0xC0) return;                /* переполнение — пакет мусорный */

    i32 dx = (i32)packet[1] - ((packet[0] << 4) & 0x100);   /* 9-битные числа со знаком */
    i32 dy = (i32)packet[2] - ((packet[0] << 3) & 0x100);
    st.x += dx;
    st.y -= dy;                                  /* у мыши Y вверх, у экрана — вниз */
    if (st.x < 0) st.x = 0;
    if (st.y < 0) st.y = 0;
    if (st.x > max_x) st.x = max_x;
    if (st.y > max_y) st.y = max_y;
    st.buttons = packet[0] & 7;
    st.events++;
    fb_cursor_move(st.x, st.y);                  /* аппаратный курсор двигает сама карта */
}

bool mouse_init(void) {
    cmd(0xA8);                                   /* включить второй порт контроллера */
    cmd(0x20);                                   /* прочитать байт конфигурации */
    if (!wait_read()) return false;
    u8 conf = inb(0x60);
    conf |= 0x02;                                /* разрешить IRQ12 */
    conf &= ~0x20;                               /* включить тактирование мыши */
    cmd(0x60);
    data(conf);
    if (!mouse_cmd(0xF6)) return false;          /* настройки по умолчанию */
    if (!mouse_cmd(0xF4)) return false;          /* начать присылать пакеты */
    st.x = (max_x + 1) / 2;
    st.y = (max_y + 1) / 2;
    if (fb_ready()) { struct fb_info fi; fb_get_info(&fi); st.x = fi.width / 2; st.y = fi.height / 2; }
    irq_install(12, mouse_irq);
    return true;
}

void mouse_usb_report(u32 buttons, i32 dx, i32 dy) {
    u32 flags=irq_save();
    st.x+=dx; st.y+=dy;
    if (st.x<0) st.x=0;
    if (st.y<0) st.y=0;
    if (st.x>max_x) st.x=max_x;
    if (st.y>max_y) st.y=max_y;
    st.buttons=buttons&7; st.events++;
    fb_cursor_move(st.x,st.y);
    irq_restore(flags);
}

void mouse_set_bounds(i32 w, i32 h) {
    u32 f = irq_save();
    max_x = w - 1; max_y = h - 1;
    if (st.x > max_x) st.x = max_x;
    if (st.y > max_y) st.y = max_y;
    irq_restore(f);
}

void mouse_get(struct mouse_state *out) {
    u32 f = irq_save();
    *out = st;
    irq_restore(f);
}
