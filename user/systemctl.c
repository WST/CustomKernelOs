#include "myos.h"
static const char *states[] = { "inactive", "active", "failed", "restarting" };
static void show(struct unit_info *u) {
    printf("%-27s %-8s %-10s pid=%u exit=%d restarts=%u\n", u->name,
           u->enabled ? "enabled" : "disabled", states[u->state < 4 ? u->state : 2],
           u->pid, u->exit_code, u->restarts);
}
int main(int argc, char **argv) {
    const char *cmd = argc > 1 ? argv[1] : "list-units";
    int op = -1, now = 0;
    if (!strcmp(cmd, "list-units") || !strcmp(cmd, "list-unit-files")) op = UNIT_LIST;
    else if (!strcmp(cmd, "status")) op = UNIT_STATUS;
    else if (!strcmp(cmd, "start")) op = UNIT_START;
    else if (!strcmp(cmd, "stop")) op = UNIT_STOP;
    else if (!strcmp(cmd, "restart")) op = UNIT_RESTART;
    else if (!strcmp(cmd, "enable")) op = UNIT_ENABLE;
    else if (!strcmp(cmd, "disable")) op = UNIT_DISABLE;
    else if (!strcmp(cmd, "daemon-reload")) op = UNIT_RELOAD;
    else if (!strcmp(cmd, "is-active") || !strcmp(cmd, "is-enabled")) op = UNIT_STATUS;
    if (op < 0 || !strcmp(cmd, "--help")) {
        puts("usage: systemctl [list-units|list-unit-files|daemon-reload]\n"
             "       systemctl status|start|stop|restart|is-active|is-enabled UNIT\n"
             "       systemctl enable|disable [--now] UNIT");
        return !strcmp(cmd, "--help") ? 0 : 2;
    }
    struct unit_info u;
    if (op == UNIT_LIST) {
        if (argc > 2) return 2;
        unsigned i = 0; int r;
        while ((r = unitctl(op, NULL, &u, i)) == 0) { show(&u); i++; }
        if (r == -ENOENT) return 0;
        dprintf(STDERR, "systemctl: %s\n", strerror(-r)); return 1;
    }
    int arg = 2;
    if ((op == UNIT_ENABLE || op == UNIT_DISABLE) && argc > arg && !strcmp(argv[arg], "--now")) { now = 1; arg++; }
    char name[28] = {0};
    if (op != UNIT_RELOAD) {
        if (argc != arg + 1) { dprintf(STDERR, "systemctl: specify one unit\n"); return 2; }
        int n = snprintf(name, sizeof(name), "%s%s", argv[arg], strstr(argv[arg], ".service") ? "" : ".service");
        if (n >= (int)sizeof(name)) { dprintf(STDERR, "systemctl: name too long\n"); return 2; }
    } else if (argc != 2) return 2;
    int r = unitctl(op, name, &u, 0);
    if (!r && now) r = unitctl(op == UNIT_ENABLE ? UNIT_START : UNIT_STOP, name, &u, 0);
    if (r < 0) { dprintf(STDERR, "systemctl: %s: %s\n", name, strerror(-r)); return 1; }
    if (!strcmp(cmd, "is-active")) { puts(states[u.state]); return u.state == UNIT_ACTIVE ? 0 : 3; }
    if (!strcmp(cmd, "is-enabled")) { puts(u.enabled ? "enabled" : "disabled"); return u.enabled ? 0 : 1; }
    if (op != UNIT_RELOAD) show(&u);
    return op == UNIT_STATUS && u.state != UNIT_ACTIVE ? 3 : 0;
}
