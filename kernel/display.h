#ifndef DISPLAY_H
#define DISPLAY_H
#include "types.h"

int  display_set_mode(u32 w, u32 h);       /* 0 или -1 */
bool display_parse(const char *s, u32 *w, u32 *h);
void display_load_config(void);            /* /etc/display — после монтирования ФС */

#endif
