#ifndef MOUSE_H
#define MOUSE_H
#include "types.h"

struct mouse_state {
    i32 x, y;                      /* координаты на экране */
    u32 buttons;                   /* бит 0 левая, 1 правая, 2 средняя */
    u32 events;                    /* счётчик пакетов — видно, что мышь шевелилась */
};

bool mouse_init(void);
void mouse_usb_report(u32 buttons, i32 dx, i32 dy);
void mouse_get(struct mouse_state *out);
void mouse_set_bounds(i32 w, i32 h);

#endif
