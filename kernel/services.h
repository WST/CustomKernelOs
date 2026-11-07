#ifndef SERVICES_H
#define SERVICES_H
#include "types.h"
/* ABI shared with user/lib/myos.h. */
enum { UNIT_LIST, UNIT_STATUS, UNIT_START, UNIT_STOP, UNIT_RESTART,
       UNIT_ENABLE, UNIT_DISABLE, UNIT_RELOAD };
enum { UNIT_INACTIVE, UNIT_ACTIVE, UNIT_FAILED, UNIT_RESTARTING };
struct unit_info {
    char name[28];
    unsigned state, enabled, pid;
    int exit_code;
    unsigned restarts;
};
void services_init(void);
int services_ctl(unsigned op, const char *name, struct unit_info *out, unsigned index);
bool services_console_active(void);
#endif
