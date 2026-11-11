/* Native htop-style process monitor. Metrics come from SYS_PS/SYS_SYSINFO. */
#include "myos.h"
#define MAXP 256
static struct proc_info procs[MAXP];
static int order[MAXP], depth[MAXP], count, selected, scroll, sortby, reverse, tree, hide_kernel;
static unsigned selected_pid;
static char filter[64], query[64], message[128];
static int editing, help, confirm, paused;
static struct system_info info;
static const char *sortnames[] = { "CPU%", "MEM", "PID", "TIME+", "Command" };
static int cols, rows;
#define MAX_CPUS 8
static struct cpu_info previous[MAX_CPUS];
static unsigned cpu_percent[MAX_CPUS];
static int cpu_count = 1, have_cpu_sample;
static void sample_cpus(void) {
    struct cpu_info sample[MAX_CPUS];
    int n = cpuinfo(sample, MAX_CPUS);
    have_cpu_sample = n > 0 && n <= MAX_CPUS;
    if (!have_cpu_sample) return;
    cpu_count = n;
    for (int i = 0; i < n; i++) {
        unsigned total = sample[i].total_ticks - previous[i].total_ticks;
        unsigned idle = sample[i].idle_ticks - previous[i].idle_ticks;
        if (total) cpu_percent[i] = idle <= total ?
            (unsigned)((unsigned long long)(total - idle) * 100 / total) : 0;
        previous[i] = sample[i];
    }
}

/* Clip each row to avoid terminal wrapping, including in small windows. */
static void line(int y, const char *color, const char *s) {
    gotoxy(0, y); printf("%s", color);
    int n = strlen(s); if (n > cols - 1) n = cols - 1;
    if (n > 0) write(STDOUT, s, n);
    printf(ANSI_RESET "\x1b[K");
}
static int matches(const char *name, const char *needle) {
    if (!*needle) return 1;
    for (; *name; name++) {
        int i = 0;
        while (needle[i] && name[i] && tolower(name[i]) == tolower(needle[i])) i++;
        if (!needle[i]) return 1;
    }
    return 0;
}
static int before(int a, int b) {
    struct proc_info *p = &procs[a], *q = &procs[b];
    int cmp;
    if (sortby == 4) cmp = strcmp(p->name, q->name);
    else {
        unsigned x = sortby == 0 ? p->cpu_percent : sortby == 1 ? p->mem_kb : sortby == 2 ? p->pid : p->cpu_ms;
        unsigned y = sortby == 0 ? q->cpu_percent : sortby == 1 ? q->mem_kb : sortby == 2 ? q->pid : q->cpu_ms;
        cmp = x == y ? 0 : x < y ? -1 : 1;
        if (sortby != 2) cmp = -cmp;
    }
    if (!cmp) cmp = p->pid < q->pid ? -1 : 1;
    return reverse ? cmp > 0 : cmp < 0;
}
static void visit(int index, int level, int *out, int *levels, int *used, int *n) {
    if (used[index]) return;
    used[index] = 1; out[*n] = index; levels[(*n)++] = level;
    for (int j = 0; j < count; j++) {
        int child = order[j];
        if (child != index && procs[child].ppid == procs[index].pid)
            visit(child, level + 1, out, levels, used, n);
    }
}
static void rebuild(int n) {
    count = 0;
    for (int i = 0; i < n; i++)
        if ((!hide_kernel || procs[i].ring) && matches(procs[i].name, filter)) order[count++] = i;
    for (int i = 1; i < count; i++) {
        int v = order[i], j = i;
        while (j > 0 && before(v, order[j-1])) { order[j] = order[j-1]; j--; }
        order[j] = v;
    }
    memset(depth, 0, sizeof(depth));
    if (tree) {
        int out[MAXP], levels[MAXP], used[MAXP], total = 0;
        memset(used, 0, sizeof(used));
        for (int i = 0; i < count; i++) {
            int v = order[i], parent = 0;
            for (int j = 0; j < count; j++)
                if (order[j] != v && procs[order[j]].pid == procs[v].ppid) parent = 1;
            if (!parent) visit(v, 0, out, levels, used, &total);
        }
        for (int i = 0; i < count; i++) visit(order[i], 0, out, levels, used, &total);
        memcpy(order, out, count * sizeof(int)); memcpy(depth, levels, count * sizeof(int));
    }
    for (int i = 0; i < count; i++) if (procs[order[i]].pid == selected_pid) selected = i;
    if (selected >= count) selected = count - 1;
    if (selected < 0) selected = 0;
    selected_pid = count ? procs[order[selected]].pid : 0;
}
static void meter(int y, const char *label, unsigned percent, const char *value, const char *color) {
    char s[512]; int width = cols / 2 - 10;
    if (width < 4) width = 4;
    if (width > 180) width = 180;
    int filled = (percent > 100 ? 100 : percent) * width / 100;
    int k = snprintf(s, sizeof(s), "%s[", label);
    for (int i = 0; i < width; i++) s[k++] = i < filled ? '|' : ' ';
    s[k++] = ']'; s[k++] = ' '; s[k] = 0;
    snprintf(s + k, sizeof(s) - k, "%s", value);
    line(y, color, s);
}
static void draw(int n) {
    char s[512]; cols = term_cols(); rows = term_rows();
    if (cols < 2 || rows < 2) return;
    int header = cpu_count + 5;
    if (rows < header + 4 || cols < 60) {
        printf(ANSI_CLEAR); line(0, ANSI_YELLOW, "htop: enlarge terminal to show all CPUs (q quits)"); return;
    }
    unsigned busy = 0, running = 0;
    for (int i = 0; i < n; i++) {
        if (strcmp(procs[i].name, "idle")) busy += procs[i].cpu_percent;
        if (procs[i].state == 1) running++;
    }
    busy /= info.cpu_online ? info.cpu_online : 1;
    for (int i = 0; i < cpu_count; i++) {
        char label[16]; snprintf(label, sizeof(label), "CPU%d", i);
        unsigned usage = have_cpu_sample ? cpu_percent[i] : busy;
        snprintf(s, sizeof(s), "%u%%", usage);
        meter(i, label, usage, s, ANSI_GREEN);
    }
    snprintf(s, sizeof(s), "%u/%u MB", info.ram_used_kb / 1024, info.ram_total_kb / 1024);
    meter(cpu_count, "Mem", info.ram_total_kb ? (unsigned)((unsigned long long)info.ram_used_kb * 100 / info.ram_total_kb) : 0, s, ANSI_CYAN);
    line(cpu_count + 1, ANSI_CYAN, "Swp[ unavailable ]");
    snprintf(s, sizeof(s), "Tasks: %d, %u running   Uptime: %u:%02u:%02u", n, running,
             info.uptime_seconds / 3600, info.uptime_seconds / 60 % 60, info.uptime_seconds % 60);
    line(cpu_count + 2, ANSI_BOLD, s);
    snprintf(s, sizeof(s), "Sort: %s %s  %s %s%s", sortnames[sortby], reverse ? "reversed" : "", tree ? "Tree" : "List", paused ? "PAUSED " : "", filter);
    line(cpu_count + 3, ANSI_WHITE, s);
    line(header - 1, "\x1b[30;42m", "  PID  PPID USER     RES   S  CPU%  MEM%    TIME+ Command");
    int visible = rows - header - 2;
    if (selected < scroll) scroll = selected;
    if (selected >= scroll + visible) scroll = selected - visible + 1;
    if (scroll > count - visible) scroll = count - visible;
    if (scroll < 0) scroll = 0;
    for (int r = 0; r < visible; r++) {
        int i = scroll + r;
        if (i >= count) { line(header + r, "", ""); continue; }
        struct proc_info *p = &procs[order[i]];
        unsigned mem10 = info.ram_total_kb ? (unsigned)((unsigned long long)p->mem_kb * 1000 / info.ram_total_kb) : 0;
        int indent = depth[i] * 2; if (indent > 24) indent = 24;
        char command[80]; memset(command, ' ', indent); command[indent] = 0;
        snprintf(command + indent, sizeof(command) - indent, "%s%s", tree && depth[i] ? "`- " : "", p->name);
        snprintf(s, sizeof(s), "%5u %5u %-6s %5u %c %4u  %3u.%u %3u:%02u.%02u %s", p->pid, p->ppid,
            p->ring ? "user" : "kernel", p->mem_kb, p->state < 5 ? "RRSDZ"[p->state] : '?', p->cpu_percent,
            mem10 / 10, mem10 % 10, p->cpu_ms / 60000, p->cpu_ms / 1000 % 60, p->cpu_ms / 10 % 100, command);
        line(header + r, i == selected ? "\x1b[30;46m" : p->ring ? ANSI_GREEN : ANSI_WHITE, s);
    }
    if (editing) snprintf(s, sizeof(s), "%s: %s_ (Enter accepts, Esc cancels)", editing == 1 ? "Search" : "Filter", query);
    else if (confirm) snprintf(s, sizeof(s), "Kill PID %u? Enter confirms; Esc cancels", selected_pid);
    else snprintf(s, sizeof(s), "%s", message);
    line(rows - 2, ANSI_YELLOW, s);
    line(rows - 1, "\x1b[30;46m", "h Help / Search \\ Filter t Tree s Sort K Kill p Pause q Quit");
    if (help) {
        line(header, ANSI_REVERSE, "htop for MyOS - press h or Esc to close help");
        const char *helptext[] = {
            "Up/Down, j/k: select; PgUp/PgDn, Home/End: scroll",
            "/: search by name; n: next match; \\: filter by name",
            "s: cycle sort; P: CPU; M: memory; N: PID; T: CPU time",
            "I: reverse sort; t: process tree; H: hide kernel threads",
            "Delete or K: kill selected task (Enter to confirm)",
            "p: pause updates; q or Ctrl+C: quit",
            "CPU0..CPU7: per-core usage; RES: kernel memory accounting.",
            "UID, priorities, swap and load averages are unavailable."
        };
        for (int i = 0; i < 8 && header + 1 + i < rows - 2; i++) line(header + 1 + i, ANSI_WHITE, helptext[i]);
    }
}
int main(int argc, char **argv) {
    if (argc > 1) {
        if (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h")) {
            puts("Usage: htop\nInteractive MyOS process monitor. Press h for keys."); return 0;
        }
        dprintf(STDERR, "htop: unknown option: %s\n", argv[1]); return 1;
    }
    int oldmode = ttymode(TTY_RAW | TTY_NONBLOCK | TTY_NOSIG), n = 0;
    unsigned last = ticks() - 1000;
    int dirty = 1;
    printf(ANSI_CLEAR ANSI_HIDE);
    for (;;) {
        if (!paused && ticks() - last >= 1000) {
            dirty = 1; last = ticks(); n = ps(procs, MAXP);
            if (n < 0) { snprintf(message, sizeof(message), "ps: %s", strerror(-n)); n = 0; }
            memset(&info, 0, sizeof(info)); sysinfo(&info, sizeof(info)); sample_cpus(); rebuild(n);
        }
        if (dirty) { draw(n); dirty = 0; }
        int key = readkey();
        if (key < 0) { sleep_ms(50); continue; }
        dirty = 1;
        if (key == KEY_RESIZE) { printf(ANSI_CLEAR); continue; }
        if (key == 3) break;
        if (editing) {
            int len = strlen(query);
            if (key == 27) { editing = 0; query[0] = 0; }
            else if (key == '\n' || key == '\r') {
                if (editing == 2) { strcpy(filter, query); selected_pid = 0; selected = scroll = 0; rebuild(n); }
                else for (int i = 0; i < count; i++) if (matches(procs[order[i]].name, query)) { selected = i; break; }
                editing = 0;
            } else if ((key == 8 || key == 127) && len) query[len-1] = 0;
            else if (key >= 32 && key < 127 && len < 63) { query[len] = key; query[len+1] = 0; }
        } else if (confirm) {
            if (key == '\n' || key == '\r') {
                int result = kill(selected_pid);
                snprintf(message, sizeof(message), "Kill PID %u: %s", selected_pid, result < 0 ? strerror(-result) : "terminated");
                confirm = 0; last = ticks() - 1000;
            } else confirm = 0;
        } else if (help) { if (key == 'h' || key == 27 || key == 'q') help = 0; }
        else {
            if (key == 'q' || key == 27) break;
            if (key == 'h' || key == '?') help = 1;
            else if (key == '/' || key == '\\') { editing = key == '/' ? 1 : 2; strcpy(query, editing == 2 ? filter : ""); }
            else if (key == KEY_UP || key == 'k') selected--;
            else if (key == KEY_DOWN || key == 'j') selected++;
            else if (key == KEY_PGUP) selected -= rows > cpu_count + 7 ? rows - cpu_count - 7 : 1;
            else if (key == KEY_PGDN) selected += rows > cpu_count + 7 ? rows - cpu_count - 7 : 1;
            else if (key == KEY_HOME) selected = 0;
            else if (key == KEY_END) selected = count - 1;
            else if (key == 'n') {
                for (int j = 1; j <= count; j++) { int i = (selected + j) % count;
                    if (matches(procs[order[i]].name, query)) { selected = i; break; } }
            } else if (key == 'p') paused = !paused;
            else if (key == 'K' || key == KEY_DEL) {
                if (count && procs[order[selected]].ring && selected_pid != (unsigned)getpid()) confirm = 1;
                else strcpy(message, "Select a user process other than htop.");
            } else {
                if (key == 's') sortby = (sortby + 1) % 5;
                else if (key == 'P') sortby = 0;
                else if (key == 'M') sortby = 1;
                else if (key == 'N') sortby = 2;
                else if (key == 'T') sortby = 3;
                else if (key == 'I') reverse = !reverse;
                else if (key == 't') tree = !tree;
                else if (key == 'H') hide_kernel = !hide_kernel;
                rebuild(n);
            }
        }
        if (selected < 0) selected = 0;
        if (count && selected >= count) selected = count - 1;
        selected_pid = count ? procs[order[selected]].pid : 0;
    }
    printf(ANSI_RESET ANSI_CLEAR ANSI_SHOW); ttymode(oldmode); return 0;
}


