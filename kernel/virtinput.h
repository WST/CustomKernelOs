#ifndef VIRTINPUT_H
#define VIRTINPUT_H
#include "types.h"
bool virtinput_init(void);
void virtinput_poll(void);
void virtinput_tick(void);
bool virtinput_interrupts(void);
#endif
