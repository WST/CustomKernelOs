/* Small system service manager. No shell is involved in ExecStart. */
#include "services.h"
#include "task.h"
#include "fs.h"
#include "file.h"
#include "process.h"
#include "syscall.h"
#include "string.h"
#include "timer.h"
#include "vga.h"

#define UNIT_MAX 16
#define CONFIG_MAX 1024
#define COMMAND_MAX 256
#define UNIT_DIR "/etc/services"
#define ENABLE_DIR "/etc/services/enabled"
struct unit {
    struct unit_info info;
    char command[COMMAND_MAX];
    unsigned restart, next_start;
    bool wanted, valid, console, stopping;
};
static struct unit units[UNIT_MAX];
static unsigned count;
static struct mutex lock;
static bool ready;

static void path_for(char *out, const char *dir, const char *name) {
    strcpy(out, dir); strcpy(out + strlen(out), "/");
    strcpy(out + strlen(out), name);
}
static bool unit_name(const char *s) {
    unsigned n = strlen(s);
    if (n <= 8 || n > 27 || strcmp(s + n - 8, ".service")) return false;
    for (unsigned i = 0; i < n - 8; i++)
        if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z') ||
              (s[i] >= '0' && s[i] <= '9') || s[i] == '-' || s[i] == '_')) return false;
    return true;
}
static char *trim(char *s) {
    while (*s == ' ' || *s == '\t' || *s == '\r') s++;
    unsigned n = strlen(s);
    while (n && (s[n-1] == ' ' || s[n-1] == '\t' || s[n-1] == '\r')) s[--n] = 0;
    return s;
}
static int config(struct unit *u) {
    char path[80], text[CONFIG_MAX + 1]; u32 ino;
    struct fs_stat st;
    path_for(path, UNIT_DIR, u->info.name);
    if (fs_resolve(FS_ROOT, path, &ino)) return -ENOENT;
    if (fs_stat(ino, &st) || st.type != FS_TYPE_FILE) return -EINVAL;
    if (st.size > CONFIG_MAX) return -EFBIG;
    int n = fs_read(ino, 0, text, st.size);
    if (n < 0 || (unsigned)n != st.size) return -EIO;
    text[n] = 0;
    u->command[0] = 0; u->restart = 0; u->console = false;
    bool section = false;
    for (char *line = text; *line;) {
        char *next = line;
        while (*next && *next != '\n') next++;
        if (*next) *next++ = 0;
        char *s = trim(line); line = next;
        if (!*s || *s == '#' || *s == ';') continue;
        if (*s == '[') { section = !strcmp(s, "[Service]"); continue; }
        if (!section) continue;
        char *value = s;
        while (*value && *value != '=') value++;
        if (!*value) return -EINVAL;
        *value++ = 0; value = trim(value); s = trim(s);
        if (!strcmp(s, "ExecStart")) {
            if (u->command[0] || strlen(value) >= COMMAND_MAX || value[0] != '/') return -EINVAL;
            strcpy(u->command, value);
        } else if (!strcmp(s, "Restart")) {
            if (!strcmp(value, "no")) u->restart = 0;
            else if (!strcmp(value, "always")) u->restart = 1;
            else if (!strcmp(value, "on-failure")) u->restart = 2;
            else return -EINVAL;
        } else if (!strcmp(s, "StandardInput")) {
            if (!strcmp(value, "tty")) u->console = true;
            else if (!strcmp(value, "null")) u->console = false;
            else return -EINVAL;
        } else if (strcmp(s, "Type") || strcmp(value, "simple")) return -EINVAL;
    }
    return u->command[0] ? 0 : -EINVAL;
}
/* Quotes and backslash, up to PROC_MAX_ARGS. No expansions or pipelines. */
static int arguments(char *s, char **argv) {
    int n = 0; char *out = s;
    while (*s) {
        while (*s == ' ' || *s == '\t') s++;
        if (!*s) break;
        if (n == PROC_MAX_ARGS) return -EINVAL;
        argv[n++] = out; char quote = 0;
        while (*s && (quote || (*s != ' ' && *s != '\t'))) {
            char c = *s++;
            if (c == '\\') { if (!*s) return -EINVAL; *out++ = *s++; }
            else if (c == '\'' || c == '"') {
                if (!quote) quote = c;
                else if (quote == c) quote = 0;
                else *out++ = c;
            } else *out++ = c;
        }
        if (quote) return -EINVAL;
        while (*s == ' ' || *s == '\t') s++;
        *out++ = 0;
    }
    argv[n] = NULL;
    return n;
}
static void refresh(struct unit *u) {
    if (u->stopping || !u->info.pid) return;
    u32 flags = irq_save();
    struct task *t = task_by_id(u->info.pid);
    if (t && t->state != TASK_ZOMBIE) { irq_restore(flags); return; }
    u->info.exit_code = t ? t->exit_code : 137;
    if (t) t->keep_zombie = false;
    u->info.pid = 0;
    irq_restore(flags);
    u->info.state = u->info.exit_code ? UNIT_FAILED : UNIT_INACTIVE;
    if (u->wanted && (u->restart == 1 || (u->restart == 2 && u->info.exit_code))) {
        u->info.state = UNIT_RESTARTING;
        u->next_start = timer_ticks() + TIMER_HZ; /* rate-limit crash loops */
    } else u->wanted = false;
}
static int start(struct unit *u) {
    if (u->stopping) return -EBUSY;
    refresh(u);
    if (u->info.pid) return 0;
    int r = config(u);
    char command[COMMAND_MAX], *argv[PROC_MAX_ARGS + 1];
    if (!r) {
        strcpy(command, u->command);
        int argc = arguments(command, argv);
        if (argc <= 0) r = -EINVAL;
        else {
            /* Keep the new task from running until its descriptors are ready. */
            char logpath[80]; u32 ino;
            path_for(logpath, "/var/log", u->info.name);
            int f = fs_resolve(FS_ROOT, logpath, &ino);
            if (f == FS_ENOENT) f = fs_create(FS_ROOT, logpath, FS_TYPE_FILE, &ino);
            struct file *log = f ? NULL : file_open_disk(ino, FMODE_WRITE | FMODE_APPEND, 0);
            if (!log) r = -EIO;
            else {
                struct file *input = NULL;
                if (!u->console && !fs_resolve(FS_ROOT, "/dev/null", &ino)) input = file_open_disk(ino, FMODE_READ, 0);
                int pid = (!u->console && !input) ? PROC_ENOMEM : proc_spawn_service(argv[0], argc, argv, log, input);
                file_close(input);
                if (pid > 0) {
                    /* System services start without an authenticated login.
                     * A terminal launched by desktop still asks for credentials. */
                    u->info.pid = pid; u->info.state = UNIT_ACTIVE;
                    u->info.exit_code = 0; u->wanted = true;
                } else r = pid == PROC_ENOENT ? -ENOENT : pid == PROC_ENOEXEC ? -ENOEXEC : -ENOMEM;
                file_close(log);
            }
        }
    }
    if (r < 0) { u->info.state = UNIT_FAILED; u->info.exit_code = r; u->wanted = false; }
    return r;
}
static int stop(struct unit *u) {
    if (u->stopping) return -EBUSY;
    u->wanted = false;
    if (u->info.pid) {
        u->stopping = true;
        u32 pid = u->info.pid;
        task_kill_group(pid);
        /* A service may itself be waiting in unitctl. Let it finish its syscall
         * so a deferred kill can complete, without holding the manager lock. */
        mutex_unlock(&lock);
        task_waitpid(pid, NULL);
        mutex_lock(&lock);
        u->info.pid = 0;
        u->stopping = false;
    }
    u->info.state = UNIT_INACTIVE;
    return 0;
}
static int reload(void) {
    for (unsigned i = 0; i < count; i++) if (units[i].stopping) return -EBUSY;
    u32 dir, pos = 0; struct fs_dirent entry;
    if (fs_resolve(FS_ROOT, UNIT_DIR, &dir)) return -ENOENT;
    for (unsigned i = 0; i < count;) {
        char path[80]; u32 ino;
        path_for(path, UNIT_DIR, units[i].info.name);
        if (!units[i].info.pid && units[i].info.state != UNIT_RESTARTING && fs_resolve(FS_ROOT, path, &ino) == FS_ENOENT) {
            memmove(&units[i], &units[i+1], (--count-i)*sizeof(units[0]));
        } else i++;
    }
    while (fs_readdir(dir, &pos, &entry) > 0) {
        if (!unit_name(entry.name)) continue;
        unsigned i;
        for (i = 0; i < count; i++) if (!strcmp(units[i].info.name, entry.name)) break;
        if (i == count) {
            if (count == UNIT_MAX) return -ENOSPC;
            memset(&units[count], 0, sizeof(units[count]));
            strcpy(units[count++].info.name, entry.name);
        }
        struct unit *u = &units[i];
        if (!u->info.pid && u->info.state != UNIT_RESTARTING) {
            int r = config(u); u->valid = r == 0;
            if (r) { u->info.state = UNIT_FAILED; u->info.exit_code = r; }
            else { u->info.state = UNIT_INACTIVE; u->info.exit_code = 0; }
        }
        char path[80]; u32 ino;
        path_for(path, ENABLE_DIR, u->info.name);
        u->info.enabled = fs_resolve(FS_ROOT, path, &ino) == 0;
    }
    return 0;
}
int services_ctl(unsigned op, const char *name, struct unit_info *out, unsigned index) {
    if (!ready) return -ENODEV;
    if (op > UNIT_RELOAD) return -EINVAL;
    if (op >= UNIT_START && (!task_current()->authenticated || task_current()->uid)) return -EPERM;
    mutex_lock(&lock);
    int r = 0;
    if (op == UNIT_RELOAD) r = reload();
    else {
        struct unit *u = NULL;
        if (op == UNIT_LIST) { if (index < count) u = &units[index]; }
        else for (unsigned i = 0; i < count; i++) if (!strcmp(name, units[i].info.name)) { u = &units[i]; break; }
        if (!u) r = -ENOENT;
        else {
            refresh(u);
            if (op == UNIT_START) r = start(u);
            else if (op == UNIT_STOP) r = stop(u);
            else if (op == UNIT_RESTART) { r = stop(u); if (!r) r = start(u); }
            else if (op == UNIT_ENABLE || op == UNIT_DISABLE) {
                char path[80]; u32 ino;
                path_for(path, ENABLE_DIR, u->info.name);
                if (op == UNIT_ENABLE) {
                    r = config(u);
                    if (!r) { int f = fs_create(FS_ROOT, path, FS_TYPE_FILE, &ino); if (f && f != FS_EEXIST) r = -EIO; }
                } else { int f = fs_remove(FS_ROOT, path); if (f && f != FS_ENOENT) r = -EIO; }
                if (!r) u->info.enabled = op == UNIT_ENABLE;
            }
            if (out) *out = u->info;
        }
    }
    mutex_unlock(&lock);
    return r;
}
bool services_console_active(void) {
    bool active = false;
    mutex_lock(&lock);
    for (unsigned i = 0; i < count; i++) if (units[i].console) {
        refresh(&units[i]); active = active || units[i].info.pid || units[i].info.state == UNIT_RESTARTING;
    }
    mutex_unlock(&lock);
    return active;
}
static void monitor(void *arg) {
    (void)arg;
    for (;;) {
        mutex_lock(&lock);
        for (unsigned i = 0; i < count; i++) {
            struct unit *u = &units[i]; refresh(u);
            if (u->wanted && u->info.state == UNIT_RESTARTING && (i32)(timer_ticks() - u->next_start) >= 0) {
                u->info.restarts++; start(u);
            }
        }
        mutex_unlock(&lock);
        task_sleep_ticks(10);
    }
}
void services_init(void) {
    if (!fs_ready()) return;
    const char *dirs[] = { "/etc", UNIT_DIR, ENABLE_DIR, "/var", "/var/log" };
    for (unsigned i = 0; i < sizeof(dirs)/sizeof(dirs[0]); i++) fs_create(FS_ROOT, dirs[i], FS_TYPE_DIR, NULL);
    reload();
    if (task_create("services", monitor, NULL) < 0) { kprintf("services: cannot create monitor\n"); return; }
    ready = true;
    mutex_lock(&lock);
    for (unsigned i = 0; i < count; i++) if (units[i].info.enabled && units[i].valid) start(&units[i]);
    mutex_unlock(&lock);
}
