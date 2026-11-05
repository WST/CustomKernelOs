/* Драйвер PS/2 клавиатуры: IRQ1, scancode set 1, раскладка US. */
#include "keyboard.h"
#include "idt.h"
#include "task.h"
#include "process.h"

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

#define BUF_SIZE 128
static volatile char buf[BUF_SIZE];
static volatile u8 head, tail;
static bool shift, caps, extended, ctrl;

static void push(char c) {
    u8 next = (head + 1) % BUF_SIZE;
    if (next != tail) { buf[head] = c; head = next; }
    task_wakeup((void *)buf);           /* разбудить того, кто ждёт клавишу */
}

static void keyboard_irq(struct regs *r) {
    (void)r;
    if (inb(0x64) & 0x20) return;       /* это байт от мыши — его заберёт IRQ12 */
    u8 sc = inb(0x60);

    if (sc == 0xE0) { extended = true; return; }
    if (extended) {
        extended = false;
        if ((sc & 0x7F) == 0x1D) { ctrl = !(sc & 0x80); return; }   /* правый Ctrl */
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
    if (sc == 0x2A || sc == 0x36) { shift = !released; return; }   /* Shift */
    if (sc == 0x1D) { ctrl = !released; return; }                  /* Ctrl */
    if (released) return;
    if (sc == 0x3A) { caps = !caps; return; }                       /* CapsLock */

    if (ctrl && sc == 0x2E) { if (!proc_ctrl_c()) push(KEY_CTRL_C); return; }   /* Ctrl+C */
    char c = shift ? map_shift[sc] : map_normal[sc];
    if (ctrl) {                                     /* Ctrl+буква = код 1..26, как в терминалах */
        char l = map_normal[sc];
        if (l >= 'a' && l <= 'z') push(l - 'a' + 1);
        return;
    }
    if (caps && c >= 'a' && c <= 'z') c -= 32;
    else if (caps && c >= 'A' && c <= 'Z') c += 32;
    if (c) push(c);
}

void keyboard_init(void) {
    while (inb(0x64) & 1) inb(0x60);   /* сбрасываем то, что накопилось */
    irq_install(1, keyboard_irq);
}

/* Ждём символ, не тратя процессор: задача засыпает, пока IRQ1 её не разбудит.
 * Прерывания запрещены между проверкой и засыпанием, иначе нажатие могло бы
 * прийти ровно между ними и потеряться (классическая гонка lost wakeup). */
/* без ожидания: -> код клавиши или -1, если буфер пуст */
int kb_poll(void) {
    u32 f = irq_save();
    int c = -1;
    if (head != tail) { c = (u8)buf[tail]; tail = (tail + 1) % BUF_SIZE; }
    irq_restore(f);
    return c;
}

char kb_getchar(void) {
    u32 f = irq_save();
    while (head == tail) {
        if (tasking_ready() && task_current()->kill_pending) { irq_restore(f); return 0; }
        if (tasking_ready()) task_wait((void *)buf);
        else __asm__ volatile ("sti; hlt; cli");
    }
    char c = buf[tail];
    tail = (tail + 1) % BUF_SIZE;
    irq_restore(f);
    return c;
}

void *kb_channel(void) { return (void *)buf; }
