#ifndef HID_MOUSE_H
#define HID_MOUSE_H
#include "types.h"
struct hid_mouse_field {u16 bit;u8 size;bool sign;};
struct hid_mouse_layout {
    struct hid_mouse_field x,y,wheel,buttons[3];
    u16 bits;u8 id;bool numbered;
};
bool hid_mouse_parse(const u8 *descriptor,u32 length,struct hid_mouse_layout *out);
bool hid_mouse_decode(const struct hid_mouse_layout *layout,const u8 *report,u32 length,
                      u32 *buttons,i32 *dx,i32 *dy,i32 *wheel);
#endif
