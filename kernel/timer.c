/* Программируемый интервальный таймер (PIT 8253/8254), канал 0 -> IRQ0. */
#include "timer.h"
#include "idt.h"
#include "task.h"
#include "fb.h"

static volatile u32 ticks;

static void timer_irq(struct regs *r) { (void)r; ticks++; fb_tick(); }

void timer_init(void) {
    u32 divisor = 1193182 / TIMER_HZ;   /* базовая частота PIT ~1.193 МГц */
    outb(0x43, 0x36);                   /* канал 0, lo/hi, режим 3 */
    outb(0x40, divisor & 0xFF);
    outb(0x40, divisor >> 8);
    irq_install(0, timer_irq);
}

u32 timer_ticks(void) { return ticks; }

/* заснуть: задача уходит в SLEEPING, процессор достаётся другим */
void sleep_ms(u32 ms) {
    task_sleep_ticks((ms * TIMER_HZ + 999) / 1000);
}
