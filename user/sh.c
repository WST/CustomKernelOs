/* sh — командная оболочка MyOS. Обычная программа в кольце 3:
 * всё, что она умеет, сделано через fork, exec, pipe, dup2 и waitpid.
 *
 *   ls /bin | grep c | wc        конвейер
 *   echo hello > f.txt           вывод в файл (>> — дописать)
 *   wc < f.txt                   ввод из файла
 *   sleep 5 &                    в фоне
 *   echo "a  b" 'c'              кавычки
 *   echo $?                      код выхода последней команды
 *   ls /bin/c*  cat /ego/?.txt   шаблоны * и ?
 *   cd, pwd, exit, help, history встроенные команды
 *   sh script.sh                 выполнить команды из файла
 *
 * Строка ввода: ←→ Home End, ↑↓ — история, Tab — дополнение команды или
 * имени файла, Ctrl+U/K/W — стереть, Ctrl+L — очистить экран, Ctrl+D — выход.
 */
#include "myos.h"

#define MAX_LINE 256
#define MAX_TOK  48
#define MAX_ARGS 32
#define HISTORY  32
#define MAX_CMDS 8

enum { T_WORD, T_PIPE, T_IN, T_OUT, T_APPEND, T_BG };

struct cmd {
    char *argv[MAX_ARGS + 1];
    int   argc;
    char *in, *out;
    bool  append;
};

static int  last_status;
static bool interactive = true;
static char status_str[12];

/* ------------------------------------------------------------ разбор строки */

static int tok_type[MAX_TOK];
static char *tok_val[MAX_TOK];
static bool tok_quoted[MAX_TOK];      /* в кавычках — шаблоны не раскрываем */
static char words[MAX_LINE * 2];

/* разбить строку на слова и операторы; кавычки склеивают слова с пробелами */
static int tokenize(const char *s) {
    int n = 0;
    char *w = words;
    while (*s) {
        while (isspace(*s)) s++;
        if (!*s || *s == '#') break;                      /* # — комментарий */
        if (n == MAX_TOK) return -1;
        if (*s == '|') { tok_type[n++] = T_PIPE; s++; continue; }
        if (*s == '&') { tok_type[n++] = T_BG; s++; continue; }
        if (*s == '<') { tok_type[n++] = T_IN; s++; continue; }
        if (*s == '>') {
            if (s[1] == '>') { tok_type[n++] = T_APPEND; s += 2; }
            else             { tok_type[n++] = T_OUT; s++; }
            continue;
        }
        tok_type[n] = T_WORD;
        tok_quoted[n] = false;
        tok_val[n++] = w;
        while (*s && !isspace(*s) && !strchr("|&<>", *s)) {
            if (*s == '"' || *s == '\'') {                /* "..." или '...' */
                tok_quoted[n - 1] = true;
                char q = *s++;
                while (*s && *s != q) *w++ = *s++;
                if (*s == q) s++;
            } else {
                *w++ = *s++;
            }
        }
        *w++ = 0;
        if (!strcmp(tok_val[n - 1], "$?")) tok_val[n - 1] = status_str;
    }
    return n;
}

/* ------------------------------------------------------------ шаблоны * и ? */

static bool match(const char *p, const char *s) {
    if (*p == 0) return *s == 0;
    if (*p == '*') {
        for (;; s++) {
            if (match(p + 1, s)) return true;
            if (!*s) return false;
        }
    }
    if (*s && (*p == '?' || *p == *s)) return match(p + 1, s + 1);
    return false;
}

static char glob_pool[4096];
static int  glob_used;

/* Раскрыть шаблон в последней части пути: /bin/c* -> /bin/cat /bin/clear ...
 * Результаты — в glob_pool, отсортированы. -> сколько добавлено в out (0 = нет совпадений) */
static int glob(const char *word, char **out, int max) {
    const char *slash = strrchr(word, '/');
    char dir[128];
    const char *pat = word;
    int dlen = 0;
    if (slash) {
        dlen = slash - word + 1;                             /* вместе с '/' */
        if (dlen >= (int)sizeof(dir)) return 0;
        memcpy(dir, word, dlen);
        dir[dlen] = 0;
        pat = slash + 1;
    } else {
        strcpy(dir, ".");
    }
    if (strchr(dir, '*') || strchr(dir, '?')) return 0;      /* шаблоны только в имени */

    int n = 0;
    struct dirent_info e;
    for (int i = 0; n < max && readdir(dir, i, &e) == 1; i++) {
        if (!match(pat, e.name)) continue;
        int need = dlen + strlen(e.name) + 1;
        if (glob_used + need > (int)sizeof(glob_pool)) break;
        char *r = glob_pool + glob_used;
        memcpy(r, word, dlen);
        strcpy(r + dlen, e.name);
        glob_used += need;
        int k = n++;                                         /* вставка с сортировкой */
        while (k > 0 && strcmp(out[k - 1], r) > 0) { out[k] = out[k - 1]; k--; }
        out[k] = r;
    }
    return n;
}

/* токены -> команды конвейера. -> число команд, -1 ошибка */
static int parse(int ntok, struct cmd *cmds, bool *bg) {
    int nc = 0;
    glob_used = 0;
    *bg = false;
    memset(cmds, 0, sizeof(struct cmd) * MAX_CMDS);
    struct cmd *c = &cmds[0];
    if (ntok == 0) return 0;
    for (int i = 0; i < ntok; i++) {
        switch (tok_type[i]) {
        case T_WORD:
            if (c->argc == MAX_ARGS) { dprintf(STDERR, "sh: too many arguments\n"); return -1; }
            if (!tok_quoted[i] && (strchr(tok_val[i], '*') || strchr(tok_val[i], '?'))) {
                int k = glob(tok_val[i], c->argv + c->argc, MAX_ARGS - c->argc);
                if (k > 0) { c->argc += k; break; }          /* нет совпадений — оставим как есть */
            }
            c->argv[c->argc++] = tok_val[i];
            break;
        case T_PIPE:
            if (!c->argc) { dprintf(STDERR, "sh: empty command before '|'\n"); return -1; }
            if (++nc == MAX_CMDS) { dprintf(STDERR, "sh: pipeline too long\n"); return -1; }
            c = &cmds[nc];
            break;
        case T_IN: case T_OUT: case T_APPEND:
            if (i + 1 >= ntok || tok_type[i + 1] != T_WORD) {
                dprintf(STDERR, "sh: missing file name after redirection\n");
                return -1;
            }
            if (tok_type[i] == T_IN) c->in = tok_val[++i];
            else { c->out = tok_val[++i]; c->append = tok_type[i - 1] == T_APPEND; }
            break;
        case T_BG:
            if (i != ntok - 1) { dprintf(STDERR, "sh: '&' must be at the end\n"); return -1; }
            *bg = true;
            break;
        }
    }
    if (!c->argc) { dprintf(STDERR, "sh: empty command\n"); return -1; }
    return nc + 1;
}

/* ------------------------------------------------------------ строка ввода
 * Терминал в сыром режиме: каждая клавиша приходит сразу, а рисуем строку мы
 * сами — так работают readline в bash и linenoise. Строка показывается в одной
 * экранной строке; если она длиннее, видимое окно сдвигается за курсором. */

static char history[HISTORY][MAX_LINE];
static int  hist_n;

static void add_history(const char *line) {
    if (!line[0] || (hist_n && !strcmp(history[(hist_n - 1) % HISTORY], line))) return;
    strncpy(history[hist_n % HISTORY], line, MAX_LINE - 1);
    hist_n++;
}

static int visible_len(const char *s) {          /* длина без escape-последовательностей */
    int n = 0;
    for (; *s; s++) {
        if (*s == 27) { while (*s && !isalpha(*s)) s++; if (!*s) break; continue; }
        n++;
    }
    return n;
}

/* длина в буквах (UTF-8: продолжения 10xxxxxx не считаем) */
static int u8len(const char *s, int n) {
    int k = 0;
    for (int i = 0; i < n; i++) if (((unsigned char)s[i] & 0xC0) != 0x80) k++;
    return k;
}

static void render(const char *prompt, const char *buf, int len, int cur) {
    static char out[1024];
    int plen = visible_len(prompt);
    int avail = SCREEN_W - 1 - plen;
    if (avail < 10) avail = 10;
    int ccur = u8len(buf, cur), off = 0;
    for (int skip = ccur > avail ? ccur - avail : 0; skip > 0 && off < len; skip--) {   /* целыми буквами */
        off++;
        while (off < len && ((unsigned char)buf[off] & 0xC0) == 0x80) off++;
    }
    int show = 0;
    for (int k = 0; off + show < len && k < avail; k++) {
        show++;
        while (off + show < len && ((unsigned char)buf[off + show] & 0xC0) == 0x80) show++;
    }
    int n = snprintf(out, sizeof(out), "\r%s%.*s\x1b[K\r", prompt, show, buf + off);
    int col = plen + ccur - u8len(buf, off);
    if (col > 0) n += snprintf(out + n, sizeof(out) - n, "\x1b[%dC", col);
    write(STDOUT, out, n);
}

/* --- Tab: дополнение --- */
static char *cand[64];
static char  cand_pool[2048];
static bool  cand_dir[64];

static int collect(const char *dir, const char *prefix, bool only_bin) {
    int n = 0, used = 0, plen = strlen(prefix);
    static const char *builtins[] = { "cd", "pwd", "exit", "help", "history" };
    if (only_bin)
        for (int i = 0; i < 5; i++)
            if (!strncmp(builtins[i], prefix, plen)) { cand[n] = (char *)builtins[i]; cand_dir[n++] = false; }
    struct dirent_info e;
    for (int i = 0; n < 64 && readdir(dir, i, &e) == 1; i++) {
        if (strncmp(e.name, prefix, plen)) continue;
        int l = strlen(e.name) + 1;
        if (used + l > (int)sizeof(cand_pool)) break;
        cand[n] = strcpy(cand_pool + used, e.name);
        cand_dir[n++] = e.type == 2;
        used += l;
    }
    return n;
}

static void insert(char *buf, int *len, int *cur, int max, const char *s, int n) {
    if (*len + n >= max) n = max - 1 - *len;
    if (n <= 0) return;
    for (int i = *len - *cur - 1; i >= 0; i--) buf[*cur + n + i] = buf[*cur + i];   /* сдвиг вправо — с конца */
    memcpy(buf + *cur, s, n);
    *len += n;
    *cur += n;
}

/* -> true, если нужно перерисовать приглашение заново (был выведен список) */
static bool complete(const char *prompt, char *buf, int *len, int *cur, int max, bool second_tab) {
    int ws = *cur;
    while (ws > 0 && buf[ws - 1] != ' ') ws--;
    int pv = ws - 1;                                           /* первое слово (или после |) = команда */
    while (pv >= 0 && buf[pv] == ' ') pv--;
    bool first = pv < 0 || buf[pv] == '|';

    char word[MAX_LINE];
    int wlen = *cur - ws;
    memcpy(word, buf + ws, wlen);
    word[wlen] = 0;

    char dir[128] = ".";
    const char *prefix = word;
    bool cmd = first && !strchr(word, '/');
    char *sl = strrchr(word, '/');
    if (sl) {
        int dl = sl - word + 1;
        memcpy(dir, word, dl);
        dir[dl] = 0;
        prefix = sl + 1;
    }
    int n = collect(cmd ? "/bin" : dir, prefix, cmd);
    if (n == 0) return false;

    int plen = strlen(prefix), common = strlen(cand[0]);
    for (int i = 1; i < n; i++) {
        int k = 0;
        while (k < common && cand[i][k] == cand[0][k]) k++;
        common = k;
    }
    if (common > plen) insert(buf, len, cur, max, cand[0] + plen, common - plen);
    if (n == 1) { insert(buf, len, cur, max, cand_dir[0] ? "/" : " ", 1); return false; }
    if (common > plen || !second_tab) return false;

    write(STDOUT, "\n", 1);                                    /* второй Tab — показать варианты */
    for (int i = 0; i < n; i++) printf("%s%s%s  ", cand_dir[i] ? ANSI_BLUE : "", cand[i], ANSI_RESET);
    write(STDOUT, "\n", 1);
    (void)prompt;
    return true;
}

/* -> длина строки, -1 = конец ввода (Ctrl+D на пустой строке) */
static int edit_line(const char *prompt, char *buf, int max) {
    int len = 0, cur = 0, hpos = hist_n;
    char draft[MAX_LINE];
    bool last_tab = false;
    buf[0] = 0;
    ttymode(TTY_RAW);
    render(prompt, buf, len, cur);
    for (;;) {
        int k = readkey();
        bool was_tab = last_tab;
        last_tab = false;
        if (k == '\r' || k == '\n') break;
        if (k >= 32 && k < 127) { char ch = k; insert(buf, &len, &cur, max, &ch, 1); }
        else if (k >= 0xA0 && k < 0x10000 && (k < KEY_UP || k > KEY_INS)) {   /* русская буква и т.п. */
            char u[4];
            insert(buf, &len, &cur, max, u, utf8_put(u, k));
        }
        else if (k == '\b' || k == 127) {
            if (cur > 0) {
                int st = cur - 1;
                while (st > 0 && ((unsigned char)buf[st] & 0xC0) == 0x80) st--;
                for (int i = 0; i < len - cur; i++) buf[st + i] = buf[cur + i];
                len -= cur - st; cur = st;
            }
        }
        else if (k == KEY_DEL || (k == CTRL('d') && len > 0)) {
            if (cur < len) {
                int e = cur + 1;
                while (e < len && ((unsigned char)buf[e] & 0xC0) == 0x80) e++;
                for (int i = 0; i < len - e; i++) buf[cur + i] = buf[e + i];
                len -= e - cur;
            }
        }
        else if (k == CTRL('d')) { ttymode(0); write(STDOUT, "\n", 1); return -1; }
        else if (k == KEY_LEFT)  { if (cur > 0) { cur--; while (cur > 0 && ((unsigned char)buf[cur] & 0xC0) == 0x80) cur--; } }
        else if (k == KEY_RIGHT) { if (cur < len) { cur++; while (cur < len && ((unsigned char)buf[cur] & 0xC0) == 0x80) cur++; } }
        else if (k == KEY_HOME || k == CTRL('a')) cur = 0;
        else if (k == KEY_END  || k == CTRL('e')) cur = len;
        else if (k == CTRL('u')) { for (int i = 0; i < len - cur; i++) buf[i] = buf[cur + i]; len -= cur; cur = 0; }
        else if (k == CTRL('k')) len = cur;
        else if (k == CTRL('w')) {
            int st = cur;
            while (st > 0 && buf[st - 1] == ' ') st--;
            while (st > 0 && buf[st - 1] != ' ') st--;
            for (int i = 0; i < len - cur; i++) buf[st + i] = buf[cur + i];
            len -= cur - st; cur = st;
        }
        else if (k == CTRL('l')) write(STDOUT, ANSI_CLEAR, strlen(ANSI_CLEAR));
        else if (k == CTRL('c')) { write(STDOUT, "^C\n", 3); len = cur = 0; }
        else if (k == KEY_UP || k == KEY_DOWN) {
            int oldest = hist_n > HISTORY ? hist_n - HISTORY : 0;
            if (hpos == hist_n) { buf[len] = 0; strcpy(draft, buf); }
            if (k == KEY_UP && hpos > oldest) hpos--;
            else if (k == KEY_DOWN && hpos < hist_n) hpos++;
            else continue;
            strcpy(buf, hpos == hist_n ? draft : history[hpos % HISTORY]);
            len = cur = strlen(buf);
        }
        else if (k == '\t') {
            buf[len] = 0;
            complete(prompt, buf, &len, &cur, max, was_tab);
            last_tab = true;
        }
        buf[len] = 0;
        render(prompt, buf, len, cur);
    }
    buf[len] = 0;
    ttymode(0);                       /* программы получат обычный терминал */
    write(STDOUT, "\n", 1);
    add_history(buf);
    return len;
}

/* ------------------------------------------------------------ встроенные */

static void help(void) {
    printf("MyOS sh. Built-ins: cd [dir], pwd, exit [code], history, help\n");
    printf("Syntax: cmd args | cmd > file >> file < file &   \"quotes\"  * ?  $?  # comment\n");
    printf("Keys: arrows, Home/End, Up/Down = history, Tab = complete, Ctrl+L/U/K/W, Ctrl+D = exit\n");
    printf("Programs in /bin:");
    struct dirent_info e;
    for (int i = 0; readdir("/bin", i, &e) == 1; i++) printf(" %s", e.name);
    printf("\n");
}

/* -> true, если команда встроенная (и уже выполнена) */
static bool builtin(struct cmd *c) {
    char *n = c->argv[0];
    if (!strcmp(n, "cd")) {
        int r = chdir(c->argc > 1 ? c->argv[1] : "/");
        if (r < 0) dprintf(STDERR, "cd: %s: %s\n", c->argv[1], strerror(-r));
        last_status = r < 0;
        return true;
    }
    if (!strcmp(n, "pwd")) {
        char buf[128];
        getcwd(buf, sizeof(buf));
        printf("%s\n", buf);
        last_status = 0;
        return true;
    }
    if (!strcmp(n, "exit")) exit(c->argc > 1 ? atoi(c->argv[1]) : last_status);
    if (!strcmp(n, "help")) { help(); last_status = 0; return true; }
    if (!strcmp(n, "history")) {
        int from = hist_n > HISTORY ? hist_n - HISTORY : 0;
        for (int i = from; i < hist_n; i++) printf("%4d  %s\n", i + 1, history[i % HISTORY]);
        last_status = 0;
        return true;
    }
    return false;
}

/* ------------------------------------------------------------ запуск */

/* Код ПОТОМКА после fork: настроить stdin/stdout и превратиться в программу */
static void child(struct cmd *c, int in_fd, int out_fd, int pgid) {
    setpgid(0, pgid);                       /* вся команда — одна группа процессов */
    if (in_fd != STDIN)   { dup2(in_fd, STDIN);   close(in_fd); }
    if (out_fd != STDOUT) { dup2(out_fd, STDOUT); close(out_fd); }
    if (c->in) {
        int fd = open(c->in, O_READ);
        if (fd < 0) { dprintf(STDERR, "sh: %s: %s\n", c->in, strerror(-fd)); exit(1); }
        dup2(fd, STDIN);
        close(fd);
    }
    if (c->out) {
        int fd = open(c->out, c->append ? O_APPEND : O_WRITE);
        if (fd < 0) { dprintf(STDERR, "sh: %s: %s\n", c->out, strerror(-fd)); exit(1); }
        dup2(fd, STDOUT);
        close(fd);
    }
    char path[64];
    if (strchr(c->argv[0], '/')) strncpy(path, c->argv[0], sizeof(path) - 1);
    else snprintf(path, sizeof(path), "/bin/%s", c->argv[0]);
    path[sizeof(path) - 1] = 0;

    int r = exec(path, c->argv);            /* при успехе сюда не вернёмся */
    struct dirent_info st;
    if (r == -ENOENT && !strchr(c->argv[0], '/') && stat(c->argv[0], &st) == 0 && st.type == 1)
        dprintf(STDERR, "sh: %s: command not found (the file is here - run it as ./%s)\n", c->argv[0], c->argv[0]);
    else if (r == -ENOENT) dprintf(STDERR, "sh: %s: command not found\n", c->argv[0]);
    else dprintf(STDERR, "sh: %s: %s\n", c->argv[0], strerror(-r));
    exit(127);
}

static void run(struct cmd *cmds, int n, bool bg) {
    if (n == 1 && !bg && builtin(&cmds[0])) return;

    int pids[MAX_CMDS], pgid = 0, prev_rd = STDIN, started = 0;
    for (int i = 0; i < n; i++) {
        int p[2] = { -1, -1 };
        if (i < n - 1 && pipe(p) < 0) { dprintf(STDERR, "sh: pipe failed\n"); break; }

        int pid = fork();
        if (pid < 0) { dprintf(STDERR, "sh: fork: %s\n", strerror(-pid)); break; }
        if (pid == 0) {
            if (p[0] >= 0) close(p[0]);     /* читающий конец нужен следующему, не нам */
            child(&cmds[i], prev_rd, i < n - 1 ? p[1] : STDOUT, pgid);
        }
        /* родитель */
        if (!pgid) pgid = pid;              /* группа = номер первого процесса */
        setpgid(pid, pgid);                 /* и сами, на случай если ребёнок ещё не успел */
        pids[i] = pid;
        if (prev_rd != STDIN) close(prev_rd);
        if (i < n - 1) { close(p[1]); prev_rd = p[0]; }
        started = i + 1;
    }

    if (prev_rd != STDIN) close(prev_rd);   /* если цикл оборвался на ошибке */
    if (!started) { last_status = 1; return; }
    if (bg) { printf("[%d] started in background\n", pgid); return; }

    setfg(pgid);                            /* Ctrl+C теперь убьёт эту группу */
    for (int i = 0; i < started; i++) {
        int st = 0;
        if (pids[i] > 0 && waitpid(pids[i], &st, 0) > 0) last_status = st;
    }
    setfg(0);                               /* а саму оболочку — никогда */
}

/* забрать завершившиеся фоновые задачи */
static void reap_background(void) {
    int st, pid;
    while ((pid = waitpid(-1, &st, WNOHANG)) > 0)
        printf("[%d] done, exit code %d\n", pid, st);
}

int main(int argc, char **argv) {
    int in = STDIN;
    bool term_mode = false;                  /* sh -t: строку редактирует окно терминала, мы только печатаем приглашение */
    if (argc > 1 && !strcmp(argv[1], "-t")) { term_mode = true; argc = 1; }
    if (argc > 1) {                          /* sh script.sh */
        in = open(argv[1], O_READ);
        if (in < 0) { dprintf(STDERR, "sh: %s: %s\n", argv[1], strerror(-in)); return 1; }
        interactive = false;
    }
    setfg(0);
    if (term_mode) { interactive = false; printf("MyOS sh (pid %d). Type 'help'.\n", getpid()); }
    else if (interactive && ttyctl(STDIN, TTYCTL_GETSIZE, 0) >= 0)   /* в окне терминала (pty) */
        printf("MyOS sh (pid %d). Type 'help'.\n", getpid());
    else if (interactive) printf("MyOS sh (pid %d). Type 'help'. 'desktop' starts the graphical desktop.\n", getpid());

    char line[MAX_LINE], cwd[128];
    struct cmd cmds[MAX_CMDS];
    for (;;) {
        reap_background();
        if (interactive) {
            char prompt[160];
            getcwd(cwd, sizeof(cwd));
            snprintf(prompt, sizeof(prompt), ANSI_GREEN ANSI_BOLD "sh" ANSI_RESET ":"
                     ANSI_BLUE ANSI_BOLD "%s" ANSI_RESET "$ ", cwd);
            if (edit_line(prompt, line, sizeof(line)) < 0) break;   /* Ctrl+D */
        } else {
            if (term_mode) {
                getcwd(cwd, sizeof(cwd));
                printf(ANSI_GREEN ANSI_BOLD "sh" ANSI_RESET ":" ANSI_BLUE ANSI_BOLD "%s" ANSI_RESET "$ ", cwd);
            }
            if (readln(in, line, sizeof(line)) < 0) break;          /* конец файла */
        }

        snprintf(status_str, sizeof(status_str), "%d", last_status);
        int ntok = tokenize(line);
        if (ntok < 0) { dprintf(STDERR, "sh: line too complex\n"); continue; }
        bool bg;
        int n = parse(ntok, cmds, &bg);
        if (n > 0) run(cmds, n, bg);
    }
    if (interactive) printf("exit\n");
    return last_status;
}
