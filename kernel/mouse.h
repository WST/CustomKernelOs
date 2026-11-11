#ifndef MOUSE_H
#define MOUSE_H
#include "types.h"

struct mouse_state {
    i32 x, y;                      /* координаты на экране */
    u32 buttons;                   /* бит 0 левая, 1 правая, 2 средняя */
    u32 events;                    /* счётчик пакетов — видно, что мышь шевелилась */
    u32 wheel;                    /* cumulative steps, positive = up */
};

bool mouse_init(void);
void mouse_usb_report(u32 buttons, i32 dx, i32 dy, i32 wheel);
void mouse_absolute_enable(bool on);
void mouse_absolute_report(u32 buttons, u32 x, u32 y, i32 wheel);
void mouse_get(struct mouse_state *out);
void mouse_set_bounds(i32 w, i32 h);

#endif
