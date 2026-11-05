#ifndef TIMER_H
#define TIMER_H
#include "types.h"

#define TIMER_HZ 100

void timer_init(void);
u32  timer_ticks(void);
void sleep_ms(u32 ms);

#endif
