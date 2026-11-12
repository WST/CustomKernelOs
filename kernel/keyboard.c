/* Драйвер PS/2 клавиатуры: IRQ1, scancode set 1, раскладки US и русская (ЙЦУКЕН).
 *
 * В буфере — коды клавиш (u16): ASCII, особые клавиши KEY_* (0x80..0x89) и
 * буквы других алфавитов как KEY_UNI + кодовая точка Unicode. Раскладку
 * переключают Alt+Shift (как в Windows) или Win+Space (как в GNOME). */
#include "keyboard.h"
#include "idt.h"
#include "task.h"
#include "process.h"
#include "fb.h"

static const char map_normal[128] = {
    0, 27, '1','2','3','4','5','6','7','8','9','0','-','=','\b',
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0, 'a','s','d','f','g','h','j','k','l',';','\'','`',
    0, '\\','z','x','c','v','b','n','m',',','.','/', 0,
    '*', 0, ' '
};
static const char map_shift[128] = {
    0, 27, '!','@','#','$','%','^','&','*','(',')','_','+','\b',
    '\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',
    0, 'A','S','D','F','G','H','J','K','L',':','"','~',
    0, '|','Z','X','C','V','B','N','M','<','>','?', 0,
    '*', 0, ' '
};

/* русская раскладка: те же клавиши (по map_normal / map_shift), UTF-8 строками */
static const char *ru_from = "qwertyuiop[]asdfghjkl;'zxcvbnm,./`"  "QWERTYUIOP{}ASDFGHJKL:\"ZXCVBNM<>?~" "@#$^&|";
static const char *ru_to   = "йцукенгшщзхъфывапролджэячсмитьбю.ё" "ЙЦУКЕНГШЩЗХЪФЫВАПРОЛДЖЭЯЧСМИТЬБЮ,Ё" "\"№;:?/";
static u16 ru_map[128];             /* ASCII латинской раскладки -> кодовая точка */

static u16 utf8_dec(const char **p) {
    const u8 *s = (const u8 *)*p;
    u16 c = *s++;
    if (c >= 0xE0) { c = (c & 0x0F) << 12 | (s[0] & 0x3F) << 6 | (s[1] & 0x3F); s += 2; }
    else if (c >= 0xC0) { c = (c & 0x1F) << 6 | (s[0] & 0x3F); s++; }
    *p = (const char *)s;
    return c;
}

static bool is_ru_letter_key(char c) {
    for (const char *p = "[];',.`"; *p; p++) if (*p == c) return true;
    return false;
}

static void ru_init(void) {
    const char *t = ru_to;
    for (const char *f = ru_from; *f && *t; f++) ru_map[(u8)*f] = utf8_dec(&t);
}

#define BUF_SIZE 128
static volatile u16 buf[BUF_SIZE];
static volatile u8 head, tail;
static bool shift, caps, extended, ctrl, alt, win, combo_used;
static int layout;                  /* 0 — US, 1 — русская */

int  kb_layout(void) { return layout; }
void kb_set_layout(int l) { layout = l ? 1 : 0; }

static void push(u16 c) {
    if (shift && c >= KEY_UP && c <= KEY_INS) c += 0x10;
    u8 next = (head + 1) % BUF_SIZE;
    if (next != tail) { buf[head] = c; head = next; }
    task_wakeup((void *)buf);           /* разбудить того, кто ждёт клавишу */
}

static void keyboard_irq(struct regs *r) {
    (void)r;
    if (inb(0x64) & 0x20) return;
    kb_scancode(inb(0x60));
}

void kb_scancode(unsigned char sc) {
    fb_input_notify();

    if (sc == 0xE0) { extended = true; return; }
    if (extended) {
        extended = false;
        if ((sc & 0x7F) == 0x1D) { ctrl = !(sc & 0x80); return; }   /* правый Ctrl */
        if ((sc & 0x7F) == 0x38) { alt = !(sc & 0x80); return; }    /* AltGr */
        if ((sc & 0x7F) == 0x5B || (sc & 0x7F) == 0x5C) { win = !(sc & 0x80); return; }   /* Win */
        switch (sc) {
        case 0x48: push(KEY_UP);    break;
        case 0x50: push(KEY_DOWN);  break;
        case 0x4B: push(KEY_LEFT);  break;
        case 0x4D: push(KEY_RIGHT); break;
        case 0x47: push(KEY_HOME);  break;
        case 0x4F: push(KEY_END);   break;
        case 0x49: push(KEY_PGUP);  break;
        case 0x51: push(KEY_PGDN);  break;
        case 0x53: push(KEY_DEL);   break;
        case 0x52: push(KEY_INS);   break;
        }
        return;
    }

    bool released = sc & 0x80;
    sc &= 0x7F;
    /* Alt+Shift: переключаем, когда нажата вторая из двух клавиш */
    if (sc == 0x2A || sc == 0x36) {                                 /* Shift */
        if (!released && !shift && alt) layout ^= 1;
        shift = !released;
        return;
    }
    if (sc == 0x38) {                                               /* левый Alt */
        if (!released && !alt && shift) layout ^= 1;
        alt = !released;
        return;
    }
    if (sc == 0x1D) { ctrl = !released; return; }                  /* Ctrl */
    if (released) return;
    if (win && sc == 0x39) { layout ^= 1; return; }                 /* Win+Space */
    (void)combo_used;
    if (sc == 0x3A) { caps = !caps; return; }                       /* CapsLock */

    if (ctrl && shift && sc == 0x2E) { push(0x9A); return; } /* Ctrl+Shift+C */
    if (ctrl && shift && sc == 0x2F) { push(0x9B); return; } /* Ctrl+Shift+V */
    if (ctrl && sc == 0x2E) { if (!proc_ctrl_c()) push(KEY_CTRL_C); return; }   /* Ctrl+C */
    bool sh = shift;
    if (layout && caps && map_normal[sc] && is_ru_letter_key(map_normal[sc])) sh = !sh;   /* это тоже буквы */
    char c = sh ? map_shift[sc] : map_normal[sc];
    if (ctrl) {                                     /* Ctrl+буква = код 1..26, как в терминалах */
        char l = map_normal[sc];
        if (l >= 'a' && l <= 'z') push(l - 'a' + 1);
        return;
    }
    if (caps && c >= 'a' && c <= 'z') c -= 32;
    else if (caps && c >= 'A' && c <= 'Z') c += 32;
    if (layout && c > 0 && ru_map[(u8)c]) {
        u16 u = ru_map[(u8)c];
        if (u >= 0x80) { push(KEY_UNI + u); return; }
        c = (char)u;
    }
    if (c) push((u8)c);
}

void keyboard_init(void) {
    ru_init();
    for (unsigned i=0;i<1024 && (inb(0x64)&1);i++) inb(0x60);   /* сбрасываем то, что накопилось */
    irq_install(1, keyboard_irq);
}

/* Ждём символ, не тратя процессор: задача засыпает, пока IRQ1 её не разбудит.
 * Прерывания запрещены между проверкой и засыпанием, иначе нажатие могло бы
 * прийти ровно между ними и потеряться (классическая гонка lost wakeup). */
/* без ожидания: -> код клавиши или -1, если буфер пуст */
int kb_has_key(void) { return head != tail; }

int kb_poll(void) {
    u32 f = irq_save();
    int c = -1;
    if (head != tail) { c = buf[tail]; tail = (tail + 1) % BUF_SIZE; }
    irq_restore(f);
    return c;
}

int kb_getchar(void) {
    u32 f = irq_save();
    while (head == tail) {
        if (tasking_ready() && task_current()->kill_pending) { irq_restore(f); return 0; }
        if (tasking_ready()) task_wait((void *)buf);
        else __asm__ volatile ("sti; hlt; cli");
    }
    int c = buf[tail];
    tail = (tail + 1) % BUF_SIZE;
    irq_restore(f);
    return c;
}

void *kb_channel(void) { return (void *)buf; }
