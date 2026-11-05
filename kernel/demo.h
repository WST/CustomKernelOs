#ifndef DEMO_H
#define DEMO_H
#include "types.h"

struct demo { const char *name, *help; };
extern const struct demo demos[];
extern const u32 demo_count;

int demo_spawn(const char *name, const char *arg);   /* -> id задачи, <0 ошибка */

#endif
