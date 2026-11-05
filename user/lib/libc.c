/* Мини-libc для программ MyOS. Всё общение с миром — через int 0x80. */
#include "myos.h"

/* ================================================================ точка входа */

int main(int argc, char **argv);

#ifndef __MYOS_CC__
/* Ядро «вызывает» _start(argc, argv): кладёт их на стек как аргументы функции.
 * (Программам, собранным cc внутри MyOS, такую же точку входа делает сам cc.) */
__attribute__((noreturn, used, section(".text.start")))
void _start(int argc, char **argv) {
    exit(main(argc, argv));
}
#endif

/* ================================================================ системные вызовы */

#if defined(__LINUX_TEST__)
/* Только для проверки cc на Linux: те же вызовы, но в номерах Linux */
static int linux_brk;
static int syscall3(int n, int a, int b, int c) {
    switch (n) {
    case SYS_EXIT:  return __syscall(1, a);
    case SYS_WRITE: return __syscall(4, a, b, c);
    case SYS_READ:  return __syscall(3, a, b, c);
    case SYS_OPEN:  return __syscall(5, a, b == 0 ? 0 : b == 1 ? 0x241 : 0x441, 0755);
    case SYS_CLOSE: return __syscall(6, a);
    case SYS_GETPID: return __syscall(20);
    case SYS_UNLINK: return __syscall(10, a);
    case SYS_SBRK: {
        if (!linux_brk) linux_brk = __syscall(45, 0);
        int old = linux_brk;
        if (__syscall(45, old + a) != old + a) return -1;
        linux_brk = old + a;
        return old;
    }
    case SYS_TERMSIZE: return (80 << 16) | 25;
    case SYS_STAT: {                                /* только «есть ли такой файл» */
        int fd = __syscall(5, a, 0, 0);
        if (fd < 0) return fd;
        __syscall(6, fd);
        struct dirent_info *o = (struct dirent_info *)b;
        o->type = 1; o->size = 0;
        return 0;
    }
    default: return -38;
    }
}
static int syscall5(int n, int a, int b, int c, int d, int e) { (void)d; (void)e; return syscall3(n, a, b, c); }
#elif defined(__MYOS_CC__)
static int syscall3(int n, int a, int b, int c) { return __syscall(n, a, b, c); }
static int syscall5(int n, int a, int b, int c, int d, int e) { return __syscall(n, a, b, c, d, e); }
#else
static inline int syscall3(int n, int a, int b, int c) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(n), "b"(a), "c"(b), "d"(c) : "memory");
    return ret;
}

static inline int syscall5(int n, int a, int b, int c, int d, int e) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(n), "b"(a), "c"(b), "d"(c), "S"(d), "D"(e) : "memory");
    return ret;
}
#endif

void exit(int code) {
    syscall3(SYS_EXIT, code, 0, 0);
    for (;;) ;
}
int   write(int fd, const void *buf, size_t len) { return syscall3(SYS_WRITE, fd, (int)buf, len); }
int   read(int fd, void *buf, size_t len)        { return syscall3(SYS_READ, fd, (int)buf, len); }
int   open(const char *path, int mode)           { return syscall3(SYS_OPEN, (int)path, mode, 0); }
int   close(int fd)                              { return syscall3(SYS_CLOSE, fd, 0, 0); }
void *sbrk(int inc)                              { return (void *)syscall3(SYS_SBRK, inc, 0, 0); }
void  sleep_ms(unsigned ms)                      { syscall3(SYS_SLEEP, ms, 0, 0); }
int   getpid(void)                               { return syscall3(SYS_GETPID, 0, 0, 0); }
unsigned ticks(void)                             { return syscall3(SYS_TICKS, 0, 0, 0); }
void  yield(void)                                { syscall3(SYS_YIELD, 0, 0, 0); }
int   fork(void)                                 { return syscall3(SYS_FORK, 0, 0, 0); }
int   exec(const char *path, char **argv)        { return syscall3(SYS_EXEC, (int)path, (int)argv, 0); }
int   waitpid(int pid, int *status, int options) { return syscall3(SYS_WAITPID, pid, (int)status, options); }
int   getppid(void)                              { return syscall3(SYS_GETPPID, 0, 0, 0); }
int   kill(int pid)                              { return syscall3(SYS_KILL, pid, 0, 0); }
int   setpgid(int pid, int pgid)                 { return syscall3(SYS_SETPGID, pid, pgid, 0); }
int   setfg(int pgid)                            { return syscall3(SYS_SETFG, pgid, 0, 0); }
int   pipe(int fds[2])                           { return syscall3(SYS_PIPE, (int)fds, 0, 0); }
int   dup2(int oldfd, int newfd)                 { return syscall3(SYS_DUP2, oldfd, newfd, 0); }
int   chdir(const char *path)                    { return syscall3(SYS_CHDIR, (int)path, 0, 0); }
int   getcwd(char *buf, size_t len)              { return syscall3(SYS_GETCWD, (int)buf, len, 0); }
int   readdir(const char *p, int i, struct dirent_info *o) { return syscall3(SYS_READDIR, (int)p, i, (int)o); }
int   mkdir(const char *path)                    { return syscall3(SYS_MKDIR, (int)path, 0, 0); }
int   unlink(const char *path)                   { return syscall3(SYS_UNLINK, (int)path, 0, 0); }
int   ps(struct proc_info *buf, int max)         { return syscall3(SYS_PS, (int)buf, max, 0); }
int   stat(const char *p, struct dirent_info *o) { return syscall3(SYS_STAT, (int)p, (int)o, 0); }

static int term_size;
static void get_term_size(void) { if (!term_size) term_size = syscall3(SYS_TERMSIZE, 0, 0, 0); }
int term_cols(void) { get_term_size(); return term_size >> 16; }
int term_rows(void) { get_term_size(); return term_size & 0xFFFF; }

static int cur_tty;
int ttymode(int mode) { cur_tty = mode; return syscall3(SYS_TTYMODE, mode, 0, 0); }

void gotoxy(int x, int y) { printf("\x1b[%d;%dH", y + 1, x + 1); }

/* Разобрать escape-последовательность: ESC [ A ... ESC [ 5 ~ */
int readkey(void) {
    unsigned char c;
    if (read(STDIN, &c, 1) <= 0) return -1;
    if (c != 27) return c;
    /* одиночный Esc от начала последовательности отличаем так: остальные байты
     * последовательности ядро кладёт сразу, а после голого Esc — ничего нет */
    syscall3(SYS_TTYMODE, cur_tty | TTY_NONBLOCK, 0, 0);
    unsigned char a = 0, b = 0, d = 0;
    int n = read(STDIN, &a, 1);
    if (n > 0 && a == '[') read(STDIN, &b, 1);
    if (b >= '0' && b <= '9') read(STDIN, &d, 1);
    syscall3(SYS_TTYMODE, cur_tty, 0, 0);
    if (n <= 0 || a != '[') return 27;
    switch (b) {
    case 'A': return KEY_UP;
    case 'B': return KEY_DOWN;
    case 'C': return KEY_RIGHT;
    case 'D': return KEY_LEFT;
    case 'H': return KEY_HOME;
    case 'F': return KEY_END;
    case '2': return KEY_INS;
    case '3': return KEY_DEL;
    case '5': return KEY_PGUP;
    case '6': return KEY_PGDN;
    }
    return 27;
}

/* ================================================================ строки */

size_t strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
int strcmp(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return (uint8_t)*a - (uint8_t)*b; }
int strncmp(const char *a, const char *b, size_t n) {
    while (n && *a && *a == *b) { a++; b++; n--; }
    return n ? (uint8_t)*a - (uint8_t)*b : 0;
}
char *strcpy(char *d, const char *s) { char *r = d; while ((*d++ = *s++)) ; return r; }
char *strcat(char *d, const char *s) { strcpy(d + strlen(d), s); return d; }
char *strchr(const char *s, int c) { for (; *s; s++) if (*s == (char)c) return (char *)s; return c ? NULL : (char *)s; }
void *memmove(void *d, const void *s, size_t n) {
    uint8_t *p = d;
    const uint8_t *q = s;
    if (p == q || n == 0) return d;
    if (p < q || p >= q + n) {                      /* не перекрываются «назад» — можно вперёд */
        while (n--) *p++ = *q++;
    } else {
        p += n; q += n;
        while (n--) *--p = *--q;
    }
    return d;
}

/* memcpy/memset — по 4 байта за раз (окна и графика копируют много) */
void *memset(void *d, int c, size_t n) {
    uint8_t *p = d;
    while (n && ((unsigned)p & 3)) { *p++ = (uint8_t)c; n--; }
    uint32_t v = (uint8_t)c * 0x01010101u;
#ifndef __MYOS_CC__
    size_t words = n / 4;
    __asm__ volatile ("rep stosl" : "+D"(p), "+c"(words) : "a"(v) : "memory");
#else
    uint32_t *w = (uint32_t *)p;
    for (size_t i = n / 4; i; i--) *w++ = v;
    p = (uint8_t *)w;
#endif
    for (n &= 3; n; n--) *p++ = (uint8_t)c;
    return d;
}
void *memcpy(void *d, const void *s, size_t n) {
    uint8_t *p = d;
    const uint8_t *q = s;
    if ((((unsigned)p | (unsigned)q) & 3) == 0) {
#ifndef __MYOS_CC__
        size_t words = n / 4;
        __asm__ volatile ("rep movsl" : "+D"(p), "+S"(q), "+c"(words) : : "memory");
#else
        uint32_t *w = (uint32_t *)p;
        const uint32_t *r = (const uint32_t *)q;
        for (size_t i = n / 4; i; i--) *w++ = *r++;
        p = (uint8_t *)w; q = (const uint8_t *)r;
#endif
        n &= 3;
    }
    while (n--) *p++ = *q++;
    return d;
}
int memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *p = a, *q = b;
    for (; n; n--, p++, q++) if (*p != *q) return *p - *q;
    return 0;
}
int isdigit(int c) { return c >= '0' && c <= '9'; }
int isspace(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
int isalpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
int toupper(int c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }
int tolower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
char *strstr(const char *h, const char *n) {
    size_t ln = strlen(n);
    for (; *h; h++) if (!strncmp(h, n, ln)) return (char *)h;
    return ln ? NULL : (char *)h;
}
int isalnum(int c) { return isalpha(c) || isdigit(c); }
char *strrchr(const char *s, int c) {
    const char *r = NULL;
    for (; *s; s++) if (*s == (char)c) r = s;
    return (char *)(c ? r : s);
}
char *strncpy(char *d, const char *s, size_t n) {
    size_t i = 0;
    for (; i < n && s[i]; i++) d[i] = s[i];
    for (; i < n; i++) d[i] = 0;
    return d;
}
const char *strerror(int e) {
    switch (e) {
    case EPERM:     return "operation not permitted";
    case ENOENT:    return "no such file or directory";
    case ESRCH:     return "no such process";
    case EINTR:     return "interrupted";
    case EIO:       return "I/O error";
    case ENOEXEC:   return "not an executable";
    case EBADF:     return "bad file descriptor";
    case ECHILD:    return "no child processes";
    case EAGAIN:    return "too many processes";
    case ENOMEM:    return "out of memory";
    case EFAULT:    return "bad address";
    case EEXIST:    return "already exists";
    case ENOTDIR:   return "not a directory";
    case EISDIR:    return "is a directory";
    case EINVAL:    return "invalid argument";
    case EMFILE:    return "too many open files";
    case EFBIG:     return "file too large (MyFS max is 8 MB)";
    case ENOSPC:    return "no space left on disk";
    case EPIPE:     return "broken pipe";
    case ENOTEMPTY: return "directory not empty";
    case EMSGSIZE:  return "message too long";
    case EADDRINUSE:   return "address already in use";
    case ENETDOWN:     return "network is down";
    case ECONNRESET:   return "connection reset by peer";
    case EISCONN:      return "already connected";
    case ENOTCONN:     return "not connected";
    case ETIMEDOUT:    return "timed out";
    case ECONNREFUSED: return "connection refused";
    case EHOSTUNREACH: return "host unreachable";
    default:        return "unknown error";
    }
}
int atoi(const char *s) {
    int sign = 1, v = 0;
    while (isspace(*s)) s++;
    if (*s == '-') { sign = -1; s++; } else if (*s == '+') s++;
    while (isdigit(*s)) v = v * 10 + (*s++ - '0');
    return v * sign;
}

/* ================================================================ форматирование */

struct out { char *buf; size_t cap, len; };

static void emit(struct out *o, char c) {
    if (o->len + 1 < o->cap) o->buf[o->len] = c;
    o->len++;
}

static void emit_num(struct out *o, unsigned v, unsigned base, bool upper, bool neg,
                     int width, char pad, bool left) {
    char tmp[16];
    const char *dig = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    int n = 0;
    do { tmp[n++] = dig[v % base]; v /= base; } while (v);
    int total = n + (neg ? 1 : 0);
    if (!left && pad == ' ') for (; total < width; width--) emit(o, ' ');
    if (neg) emit(o, '-');
    if (!left && pad == '0') for (; total < width; width--) emit(o, '0');
    while (n) emit(o, tmp[--n]);
    if (left) for (; total < width; width--) emit(o, ' ');
}

/* %d %i %u %x %X %c %s %p %%; флаги '-' и '0', ширина, точность для %s */
int vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap) {
    struct out o = { buf, cap, 0 };
    for (; *fmt; fmt++) {
        if (*fmt != '%') { emit(&o, *fmt); continue; }
        fmt++;
        bool left = false;
        char pad = ' ';
        int width = 0, prec = -1;
        for (;; fmt++) {
            if (*fmt == '-') left = true;
            else if (*fmt == '0') pad = '0';
            else break;
        }
        if (*fmt == '*') { width = va_arg(ap, int); fmt++; if (width < 0) { left = true; width = -width; } }
        while (isdigit(*fmt)) width = width * 10 + (*fmt++ - '0');
        if (*fmt == '.') {
            prec = 0;
            fmt++;
            if (*fmt == '*') { prec = va_arg(ap, int); fmt++; if (prec < 0) prec = -1; }
            while (isdigit(*fmt)) prec = prec * 10 + (*fmt++ - '0');
        }
        while (*fmt == 'l') fmt++;
        switch (*fmt) {
        case 'd': case 'i': {
            int v = va_arg(ap, int);
            emit_num(&o, v < 0 ? -(unsigned)v : (unsigned)v, 10, false, v < 0, width, pad, left);
            break;
        }
        case 'u': emit_num(&o, va_arg(ap, unsigned), 10, false, false, width, pad, left); break;
        case 'x': emit_num(&o, va_arg(ap, unsigned), 16, false, false, width, pad, left); break;
        case 'X': emit_num(&o, va_arg(ap, unsigned), 16, true, false, width, pad, left); break;
        case 'p': emit(&o, '0'); emit(&o, 'x');
                  emit_num(&o, va_arg(ap, unsigned), 16, false, false, 8, '0', false); break;
        case 'f': {                              /* double: целая часть + prec знаков */
            double v = va_arg(ap, double);
            if (prec < 0) prec = 6;
            if (prec > 9) prec = 9;
            bool neg = v < 0;
            if (neg) v = -v;
            unsigned scale = 1;
            for (int k = 0; k < prec; k++) scale *= 10;
            if (v > 4e9) v = 4e9;
            unsigned ip = (unsigned)v;
            unsigned fp = (unsigned)((v - ip) * scale + 0.5);
            if (fp >= scale) { ip++; fp -= scale; }
            char tmp[32];
            int n = 0, w = width - (prec ? prec + 1 : 0);
            emit_num(&o, ip, 10, false, neg, w, pad, false);
            if (prec) {
                emit(&o, '.');
                for (int k = prec - 1; k >= 0; k--) { tmp[k] = '0' + fp % 10; fp /= 10; }
                for (; n < prec; n++) emit(&o, tmp[n]);
            }
            break;
        }
        case 'c': emit(&o, (char)va_arg(ap, int)); break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            int len = strlen(s);
            if (prec >= 0 && len > prec) len = prec;
            if (!left) for (int k = len; k < width; k++) emit(&o, ' ');
            for (int k = 0; k < len; k++) emit(&o, s[k]);
            if (left) for (int k = len; k < width; k++) emit(&o, ' ');
            break;
        }
        case '%': emit(&o, '%'); break;
        case 0: fmt--; break;
        default: emit(&o, '%'); emit(&o, *fmt);
        }
    }
    if (cap) buf[o.len < cap ? o.len : cap - 1] = 0;
    return o.len;
}

int snprintf(char *buf, size_t n, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, n, fmt, ap);
    va_end(ap);
    return r;
}

static int vdprintf(int fd, const char *fmt, va_list ap) {
    char buf[512];
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    write(fd, buf, n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1);
    return n;
}

int printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vdprintf(STDOUT, fmt, ap);
    va_end(ap);
    return r;
}

int dprintf(int fd, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vdprintf(fd, fmt, ap);
    va_end(ap);
    return r;
}

int puts(const char *s) { write(STDOUT, s, strlen(s)); write(STDOUT, "\n", 1); return 0; }
int putchar(int c) { char ch = c; write(STDOUT, &ch, 1); return c; }

/* Построчное чтение с буфером на каждый дескриптор: read() по байту был бы
 * системным вызовом на каждый символ. */
#define RL_FDS 16
static struct { char buf[256]; int pos, len; } rl[RL_FDS];

int readln(int fd, char *out, int max) {
    if (fd < 0 || fd >= RL_FDS) return -1;
    int n = 0;
    for (;;) {
        if (rl[fd].pos == rl[fd].len) {
            rl[fd].pos = 0;
            rl[fd].len = read(fd, rl[fd].buf, sizeof(rl[fd].buf));
            if (rl[fd].len <= 0) { rl[fd].len = 0; out[n] = 0; return n ? n : -1; }
        }
        char c = rl[fd].buf[rl[fd].pos++];
        if (c == '\n') { out[n] = 0; return n; }
        if (n < max - 1) out[n++] = c;
    }
}

int getline(char *buf, int max) {
    int n = read(STDIN, buf, max - 1);
    if (n <= 0) { buf[0] = 0; return -1; }
    if (buf[n - 1] == '\n') n--;
    buf[n] = 0;
    return n;
}

/* ================================================================ malloc
 * Размеры округляем до «классов» (16, 32, ... 256, затем 512, 1024, 2048 ...).
 * У каждого класса свой список освободившихся блоков: malloc берёт первый из
 * списка, free кладёт блок обратно — оба за постоянное время, сколько бы блоков
 * ни было. Новую память берём у ядра через sbrk() кусками по 64 КБ. */

struct hdr {
    size_t size;            /* размер данных (= размер класса) */
    size_t magic;
    struct hdr *next;       /* в списке свободных */
    size_t pad;             /* чтобы данные были выровнены на 16 */
};

#define HEAP_MAGIC 0x4D594F53u                      /* "MYOS" */
#define N_CLASSES 48
static struct hdr *free_list[N_CLASSES];
static char *wild, *wild_end;                       /* ещё не нарезанный кусок от sbrk */

static int size_class(size_t n, size_t *rounded) {
    if (n <= 256) { n = (n + 15) & ~15u; *rounded = n; return n / 16 - 1; }    /* 0..15 */
    size_t c = 512;
    int k = 16;
    while (c < n) { c <<= 1; k++; }
    *rounded = c;
    return k;
}

void *malloc(size_t n) {
    if (n == 0) n = 1;
    if (n > 0x40000000u) return NULL;
    size_t sz;
    int k = size_class(n, &sz);
    struct hdr *h = free_list[k];
    if (h) {
        free_list[k] = h->next;
        h->next = NULL;
        return h + 1;
    }
    size_t need = sizeof(struct hdr) + sz;
    if ((size_t)(wild_end - wild) < need) {
        size_t chunk = need > 65536 ? need : 65536;
        char *p = sbrk(chunk);
        if (p == (void *)-1) return NULL;
        if (p != wild_end) wild = p;                /* новый кусок не впритык к старому */
        wild_end = p + chunk;
    }
    h = (struct hdr *)wild;
    wild += need;
    h->size = sz;
    h->magic = HEAP_MAGIC;
    h->next = NULL;
    return h + 1;
}

void free(void *p) {
    if (!p) return;
    struct hdr *h = (struct hdr *)p - 1;
    if (h->magic != HEAP_MAGIC) return;             /* не наш указатель — не трогаем */
    size_t sz;
    int k = size_class(h->size, &sz);
    h->next = free_list[k];
    free_list[k] = h;
}

void *calloc(size_t n, size_t size) {
    void *p = malloc(n * size);
    if (p) memset(p, 0, n * size);
    return p;
}

void *realloc(void *p, size_t n) {
    if (!p) return malloc(n);
    struct hdr *h = (struct hdr *)p - 1;
    if (h->size >= n) return p;
    void *q = malloc(n);
    if (q) { memcpy(q, p, h->size); free(p); }
    return q;
}

/* ================================================================ случайные числа */

static unsigned rnd_state = 1;
void srand(unsigned seed) { rnd_state = seed ? seed : 1; }
unsigned rand(void) { rnd_state = rnd_state * 1103515245 + 12345; return (rnd_state >> 16) & 0x7FFF; }

/* ================================================================ сеть */

int socket(int type)                       { return syscall3(SYS_SOCKET, type, 0, 0); }
int connect(int fd, unsigned ip, int port) { return syscall3(SYS_CONNECT, fd, (int)ip, port); }
int bind(int fd, int port)                 { return syscall3(SYS_BIND, fd, port, 0); }
int listen(int fd, int backlog)            { return syscall3(SYS_LISTEN, fd, backlog, 0); }
int accept(int fd, struct sockaddr *from)  { return syscall3(SYS_ACCEPT, fd, (int)from, 0); }
int sendto(int fd, const void *buf, size_t len, unsigned ip, int port) {
    return syscall5(SYS_SENDTO, fd, (int)buf, (int)len, (int)ip, port);
}
int recvfrom(int fd, void *buf, size_t len, struct sockaddr *from, unsigned timeout_ms) {
    return syscall5(SYS_RECVFROM, fd, (int)buf, (int)len, (int)from, (int)timeout_ms);
}
int netinfo(struct net_info *out)          { return syscall3(SYS_NETINFO, (int)out, 0, 0); }
int ping(unsigned ip, int seq, unsigned t) { return syscall3(SYS_PING, (int)ip, seq, (int)t); }
int set_dns(unsigned ip)                   { return syscall3(SYS_NETCONFIG, 1, (int)ip, 0); }

int ip_parse(const char *s, unsigned *ip) {
    unsigned v = 0;
    for (int part = 0; part < 4; part++) {
        if (!isdigit(*s)) return 0;
        unsigned n = 0;
        while (isdigit(*s)) { n = n * 10 + (*s++ - '0'); if (n > 255) return 0; }
        v = (v << 8) | n;
        if (part < 3 && *s++ != '.') return 0;
    }
    if (*s) return 0;
    *ip = v;
    return 1;
}

char *ip_str(unsigned ip, char *buf) {
    snprintf(buf, 16, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
    return buf;
}

/* DNS: один UDP-запрос «дай A-запись» к серверу из netinfo, ответ ищем
 * среди записей. Имена в ответе бывают сжаты (ссылка 0xC0xx) — их пропускаем. */
static int dns_skip_name(const unsigned char *p, int pos, int len) {
    while (pos < len) {
        unsigned char c = p[pos];
        if (c == 0) return pos + 1;
        if ((c & 0xC0) == 0xC0) return pos + 2;
        pos += c + 1;
    }
    return -1;
}

int resolve(const char *name, unsigned *ip) {
    if (ip_parse(name, ip)) return 0;
    if (!strcmp(name, "localhost")) { *ip = IP(127, 0, 0, 1); return 0; }
    struct net_info ni;
    if (netinfo(&ni) < 0 || !ni.up) return -ENETDOWN;
    if (!ni.dns) return -EHOSTUNREACH;

    unsigned char q[300];
    int n = strlen(name);
    if (n == 0 || n > 250) return -EINVAL;
    unsigned short id = (unsigned short)(ticks() * 7919 + getpid());
    memset(q, 0, 12);
    q[0] = id >> 8; q[1] = id; q[2] = 0x01;          /* рекурсия, пожалуйста */
    q[5] = 1;                                         /* один вопрос */
    int pos = 12;
    const char *s = name;
    while (*s) {                                      /* www.example.com -> 3www7example3com0 */
        const char *dot = strchr(s, '.');
        int l = dot ? dot - s : (int)strlen(s);
        if (l == 0 || l > 63) return -EINVAL;
        q[pos++] = l;
        memcpy(q + pos, s, l); pos += l;
        s += l;
        if (*s == '.') s++;
    }
    q[pos++] = 0;
    q[pos++] = 0; q[pos++] = 1;                       /* тип A */
    q[pos++] = 0; q[pos++] = 1;                       /* класс IN */

    int fd = socket(SOCK_UDP);
    if (fd < 0) return fd;
    int err = -ETIMEDOUT;
    for (int attempt = 0; attempt < 2 && err == -ETIMEDOUT; attempt++) {
        int r = sendto(fd, q, pos, ni.dns, 53);
        if (r < 0 && r != -EHOSTUNREACH) { err = r; break; }   /* ARP ещё не знает сервер — повторим */
        unsigned char a[512];
        struct sockaddr from;
        for (;;) {
            r = recvfrom(fd, a, sizeof(a), &from, 1500);
            if (r < 0) { if (r != -ETIMEDOUT) err = r; break; }
            if (r < 12 || a[0] != q[0] || a[1] != q[1]) continue;   /* чужой ответ */
            if ((a[3] & 0x0F) == 3) { err = -ENOENT; break; }        /* NXDOMAIN: имени нет */
            if ((a[3] & 0x0F) != 0) { err = -EHOSTUNREACH; break; }  /* сервер не смог ответить */
            int qd = (a[4] << 8) | a[5], an = (a[6] << 8) | a[7], p = 12;
            for (int i = 0; i < qd && p > 0; i++) { p = dns_skip_name(a, p, r); if (p > 0) p += 4; }
            err = -ENOENT;
            for (int i = 0; i < an && p > 0 && p < r; i++) {
                p = dns_skip_name(a, p, r);
                if (p < 0 || p + 10 > r) break;
                int type = (a[p] << 8) | a[p + 1], rdlen = (a[p + 8] << 8) | a[p + 9];
                p += 10;
                if (type == 1 && rdlen == 4 && p + 4 <= r) {
                    *ip = ((unsigned)a[p] << 24) | (a[p + 1] << 16) | (a[p + 2] << 8) | a[p + 3];
                    err = 0;
                    break;
                }
                p += rdlen;                               /* CNAME и прочее — дальше */
            }
            break;
        }
    }
    close(fd);
    return err;
}

int tcp_connect(const char *host, int port) {
    unsigned ip;
    int r = resolve(host, &ip);
    if (r < 0) return r;
    int fd = socket(SOCK_TCP);
    if (fd < 0) return fd;
    for (int attempt = 0; ; attempt++) {
        r = connect(fd, ip, port);
        if (r != -EHOSTUNREACH || attempt == 2) break;   /* ARP ещё не ответил — чуть подождём */
        sleep_ms(200);
    }
    if (r < 0) { close(fd); return r; }
    return fd;
}

/* ================================================================ общение процессов */

int msg_send(int pid, struct msg *m)            { return syscall3(SYS_MSG_SEND, pid, (int)m, 0); }
int msg_recv(struct msg *m, int timeout_ms)     { return syscall3(SYS_MSG_RECV, (int)m, timeout_ms, 0); }
int shm_alloc(unsigned size)                    { return syscall3(SYS_SHM_ALLOC, (int)size, 0, 0); }
void *shm_map(int id) {
    int r = syscall3(SYS_SHM_MAP, id, 0, 0);
    return (r < 0 && r > -4096) ? NULL : (void *)r;  /* адреса общей памяти «отрицательные» как int */
}
int shm_unmap(void *addr)                       { return syscall3(SYS_SHM_UNMAP, (int)addr, 0, 0); }
int service_register(const char *name)          { return syscall3(SYS_SERVICE, 1, (int)name, 0); }
int service_find(const char *name)              { return syscall3(SYS_SERVICE, 2, (int)name, 0); }
int get_time(struct datetime *t)                { return syscall3(SYS_TIME, (int)t, 0, 0); }
