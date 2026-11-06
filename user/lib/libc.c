/* Мини-libc для программ MyOS. Всё общение с миром — через int 0x80. */
#include "myos.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"
#include "time.h"
#include "sys/stat.h"
#include "dirent.h"
#include "libgen.h"
#include "wchar.h"
#include "util.h"

/* Filesystem administration wrappers are defined below syscall3. */

/* ================================================================ точка входа */

int main(int argc, char **argv);

int __libc_start(int argc, char **argv) __attribute__((noreturn));

#ifndef __MYOS_CC__
/* Ядро «вызывает» _start(argc, argv): кладёт их на стек как аргументы функции.
 * (Программам, собранным cc внутри MyOS, такую же точку входа делает сам cc.) */
__attribute__((noreturn, used, section(".text.start")))
void _start(int argc, char **argv) {
    __libc_start(argc, argv);
}
#endif

/* ================================================================ системные вызовы */

#if defined(__LINUX_TEST__)
/* Только для проверки cc на Linux: те же вызовы, но в номерах Linux */
static int linux_brk;

/* Поиндексное чтение каталога через getdents64. Точки пропускаются, как в ядре. */
static int linux_dir_at(const char *path, int index, struct dirent_info *o) {
    unsigned char buf[1024];
    int fd = __syscall(5, (int)path, 0, 0);
    int seen = 0;
    if (fd < 0) return fd;
    for (;;) {
        int n = __syscall(220, fd, (int)buf, (int)sizeof(buf));
        int off = 0;
        if (n < 0) { __syscall(6, fd); return n; }
        if (n == 0) { __syscall(6, fd); return 0; }
        while (off + 19 < n) {
            unsigned short reclen;
            unsigned char typ;
            char *name;
            int i;
            memcpy(&reclen, buf + off + 16, 2);
            if (reclen < 19 || off + reclen > n) { __syscall(6, fd); return -EIO; }
            typ = buf[off + 18];
            name = (char *)(buf + off + 19);
            off += reclen;
            if (name[0] == '.' && (name[1] == 0 || (name[1] == '.' && name[2] == 0))) continue;
            if (seen++ != index) continue;
            memset(o, 0, sizeof *o);
            for (i = 0; i < 27 && name[i]; i++) o->name[i] = name[i];
            if (typ == 4) o->type = 2;
            else if (typ == 2) o->type = 4;
            else if (typ == 6) o->type = 3;
            else o->type = 1;
            {
                char full[192];
                unsigned char st[64];
                unsigned mode;
                if (snprintf(full, sizeof full, "%s/%s", path, o->name) < (int)sizeof full &&
                    __syscall(106, (int)full, (int)st, 0) == 0) {
                    mode = *(unsigned short *)(st + 8);
                    o->size = *(unsigned *)(st + 20);
                    if ((mode & 0170000) == 0040000) o->type = 2;
                    else if ((mode & 0170000) == 0020000) o->type = 4;
                    else if ((mode & 0170000) == 0060000) o->type = 3;
                    else o->type = 1;
                }
            }
            __syscall(6, fd);
            return 1;
        }
    }
}

static int syscall3(int n, int a, int b, int c) {
    switch (n) {
    case SYS_EXIT:  return __syscall(1, a);
    case SYS_WRITE: return __syscall(4, a, b, c);
    case SYS_READ:  return __syscall(3, a, b, c);
    case SYS_OPEN:  return __syscall(5, a, b, c);
    case SYS_CLOSE: return __syscall(6, a);
    case SYS_LSEEK: return __syscall(19, a, b, c);
    case SYS_FSTAT: {                           /* i386 struct stat -> {mode, size} */
        unsigned char st[64];
        int r = __syscall(108, a, (int)st, 0);
        if (r < 0) return r;
        unsigned *out = (unsigned *)b;
        out[0] = *(unsigned short *)(st + 8);
        out[1] = *(unsigned *)(st + 20);
        return 0;
    }
    case SYS_GETPID: return __syscall(20);
    case SYS_UNLINK: {
        int r = __syscall(10, a);
        if (r == -EISDIR) r = __syscall(40, a);   /* пустой каталог, как unlink в MyOS */
        return r;
    }
    case SYS_MKDIR: return __syscall(39, a, 0755);
    case SYS_RENAME: return __syscall(38, a, b);
    case SYS_SBRK: {
        if (!linux_brk) linux_brk = __syscall(45, 0);
        int old = linux_brk;
        if (__syscall(45, old + a) != old + a) return -1;
        linux_brk = old + a;
        return old;
    }
    case SYS_TERMSIZE: return (80 << 16) | 25;
    case SYS_STAT: {
        unsigned char st[64];
        int r = __syscall(106, a, (int)st, 0);
        unsigned mode;
        struct dirent_info *o;
        if (r < 0) return r;
        mode = *(unsigned short *)(st + 8);
        o = (struct dirent_info *)b;
        memset(o, 0, sizeof *o);
        o->size = *(unsigned *)(st + 20);
        if ((mode & 0170000) == 0040000) o->type = 2;
        else if ((mode & 0170000) == 0020000) o->type = 4;
        else if ((mode & 0170000) == 0060000) o->type = 3;
        else o->type = 1;
        return 0;
    }
    case SYS_READDIR: return linux_dir_at((const char *)a, b, (struct dirent_info *)c);
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

int errno;

/* POSIX: -1 и errno. Остальные вызовы по-прежнему возвращают -код. */
static int ioerr(int r) {
    if (r < 0) { errno = -r; return -1; }
    return r;
}

int   write(int fd, const void *buf, size_t len) { return ioerr(syscall3(SYS_WRITE, fd, (int)buf, len)); }
int   read(int fd, void *buf, size_t len)        { return ioerr(syscall3(SYS_READ, fd, (int)buf, len)); }
int   open(const char *path, int flags, ...)     { return ioerr(syscall3(SYS_OPEN, (int)path, flags, 0755)); }
int   close(int fd)                              { return ioerr(syscall3(SYS_CLOSE, fd, 0, 0)); }
int   lseek(int fd, long offset, int whence)     { return ioerr(syscall3(SYS_LSEEK, fd, (int)offset, whence)); }
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
int   dirread(const char *p, int i, struct dirent_info *o) { return syscall3(SYS_READDIR, (int)p, i, (int)o); }
int   mkdir(const char *path)                    { return syscall3(SYS_MKDIR, (int)path, 0, 0); }
int   unlink(const char *path)                   { return syscall3(SYS_UNLINK, (int)path, 0, 0); }
int   ps(struct proc_info *buf, int max)         { return syscall3(SYS_PS, (int)buf, max, 0); }
int   dirstat(const char *p, struct dirent_info *o) { return syscall3(SYS_STAT, (int)p, (int)o, 0); }

/* размер не запоминаем: окно терминала могут растянуть в любой момент */
int term_cols(void) { return syscall3(SYS_TERMSIZE, 0, 0, 0) >> 16; }
int term_rows(void) { return syscall3(SYS_TERMSIZE, 0, 0, 0) & 0xFFFF; }

int pty_open(int fds[2]) { return syscall3(SYS_PTY_OPEN, (int)fds, 0, 0); }
int ttyctl(int fd, int op, int arg) { return syscall3(SYS_TTYCTL, fd, op, arg); }
int kblayout(int l) { return syscall3(SYS_KBLAYOUT, l, 0, 0); }
int sockctl(int fd, int op, unsigned arg) { return syscall3(SYS_SOCKCTL, fd, op, (int)arg); }

int utf8_put(char *o, int cp) {
    if (cp < 0x80) { o[0] = cp; return 1; }
    if (cp < 0x800) { o[0] = 0xC0 | cp >> 6; o[1] = 0x80 | (cp & 0x3F); return 2; }
    o[0] = 0xE0 | cp >> 12; o[1] = 0x80 | (cp >> 6 & 0x3F); o[2] = 0x80 | (cp & 0x3F); return 3;
}

static int cur_tty;
int ttymode(int mode) { cur_tty = mode; return syscall3(SYS_TTYMODE, mode, 0, 0); }

void gotoxy(int x, int y) { printf("\x1b[%d;%dH", y + 1, x + 1); }

/* Разобрать escape-последовательность: ESC [ A ... ESC [ 5 ~ */
int readkey(void) {
    unsigned char c;
    if (read(STDIN, &c, 1) <= 0) return -1;
    if (c >= 0xC0) {                                /* UTF-8: остальные байты буквы уже в буфере */
        int n = c >= 0xE0 ? 2 : 1, cp = c & (c >= 0xE0 ? 0x0F : 0x1F);
        for (int i = 0; i < n; i++) {
            unsigned char d;
            if (read(STDIN, &d, 1) <= 0) break;
            cp = cp << 6 | (d & 0x3F);
        }
        return cp;
    }
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
int isspace(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }
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
    case EBUSY: return "mountpoint or device busy";
    case EXDEV: return "cross-device rename";
    case EROFS: return "read-only filesystem/device";
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
    case ERANGE:    return "result out of range";
    case ESPIPE:    return "illegal seek";
    case EMFILE:    return "too many open files";
    case EFBIG:     return "file or filesystem too large";
    case ENOSPC:    return "no space left on disk";
    case EPIPE:     return "broken pipe";
    case ENAMETOOLONG: return "file name too long";
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

static void emit_u64(struct out *o, unsigned long long v, unsigned base, bool upper, bool neg,
                     int width, char pad, bool left) {
    char tmp[32];
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

/* %d %i %u %x %X %c %s %p %lld %%; флаги '-' и '0', ширина, точность для %s */
int vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap) {
    /* «o += snprintf(buf + o, max - o, ...)» после переполнения даёт отрицательный размер,
     * который превращается в огромный size_t: считаем его нулём, а не «писать сколько угодно» */
    if ((int)cap < 0) cap = 0;
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
        int lng = 0;
        while (*fmt == 'l') { lng++; fmt++; }
        if (*fmt == 'z') { fmt++; lng = lng < 1 ? 1 : lng; }
        switch (*fmt) {
        case 'd': case 'i': {
            if (lng >= 2) {
                long long sv = va_arg(ap, long long);
                unsigned long long u = (unsigned long long)sv;
                if (sv < 0) u = -u;
                emit_u64(&o, u, 10, false, sv < 0, width, pad, left);
            } else {
                int v = va_arg(ap, int);
                emit_num(&o, v < 0 ? -(unsigned)v : (unsigned)v, 10, false, v < 0, width, pad, left);
            }
            break;
        }
        case 'u':
            if (lng >= 2) emit_u64(&o, va_arg(ap, unsigned long long), 10, false, false, width, pad, left);
            else emit_num(&o, va_arg(ap, unsigned), 10, false, false, width, pad, left);
            break;
        case 'x':
            if (lng >= 2) emit_u64(&o, va_arg(ap, unsigned long long), 16, false, false, width, pad, left);
            else emit_num(&o, va_arg(ap, unsigned), 16, false, false, width, pad, left);
            break;
        case 'X':
            if (lng >= 2) emit_u64(&o, va_arg(ap, unsigned long long), 16, true, false, width, pad, left);
            else emit_num(&o, va_arg(ap, unsigned), 16, true, false, width, pad, left);
            break;
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

void __libc_init(void);
int printf(const char *fmt, ...) {
    va_list ap;
    __libc_init();
    va_start(ap, fmt);
    int r = vfprintf(stdout, fmt, ap);
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

int getln(char *buf, int max) {
    int n = read(STDIN, buf, max - 1);
    if (n <= 0) { buf[0] = 0; return -1; }
    if (buf[n - 1] == '\n') n--;
    buf[n] = 0;
    return n;
}

/* ================================================================ malloc
 * Размеры округляем до «классов» (16, 32, ... 256, затем 512, 1024, ... 64 КБ;
 * крупнее — шагами в четверть: 80, 96, 112, 128 КБ, 160, 192 ... — иначе буфер
 * экрана 1920x1200 в 9 МБ занимал бы 16 МБ).
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
#define N_CLASSES 96
static struct hdr *free_list[N_CLASSES];
static char *wild, *wild_end;                       /* ещё не нарезанный кусок от sbrk */

static int size_class(size_t n, size_t *rounded) {
    if (n <= 256) { n = (n + 15) & ~15u; *rounded = n; return n / 16 - 1; }    /* 0..15 */
    size_t c = 512;
    int k = 16;
    if (n <= 65536) {
        while (c < n) { c <<= 1; k++; }
        *rounded = c;
        return k;                                   /* 16..23 */
    }
    size_t base = 65536;
    k = 24;
    for (;;) {
        for (int q = 5; q <= 8; q++, k++) {
            c = base / 4 * q;
            if (c >= n) { *rounded = c; return k; }
        }
        base <<= 1;
    }
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
        if (p == (void *)-1 && chunk > need) { chunk = need; p = sbrk(need); }   /* хоть сколько-нибудь */
        if (p == (void *)-1) {
            /* память у ядра кончилась: разрезать свободный блок побольше — он станет новым
             * «неразрезанным куском» (остаток старого куска пропадёт, он маленький) */
            int j = k + 1;
            while (j < N_CLASSES && !free_list[j]) j++;
            if (j == N_CLASSES) return NULL;
            struct hdr *big = free_list[j];
            free_list[j] = big->next;
            wild = (char *)big;
            wild_end = (char *)(big + 1) + big->size;
        } else {
            if (p != wild_end) wild = p;            /* новый кусок не впритык к старому */
            wild_end = p + chunk;
        }
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

int mount(const char *device,const char *target) { return syscall3(SYS_MOUNT,(int)device,(int)target,0); }
int mkfs(const char *device) { return syscall3(SYS_MKFS,(int)device,0,0); }
int partctl(const char *device,unsigned op,unsigned slot,unsigned start,unsigned sectors) {
    unsigned args[3]; args[0]=slot; args[1]=start; args[2]=sectors;
    return syscall3(SYS_PARTCTL,(int)device,op,(int)args);
}
int mountinfo(unsigned index,char device[128],char target[128]) { return syscall3(SYS_MOUNTINFO,index,(int)device,(int)target); }
int blockinfo(unsigned index,struct block_info *info) { return syscall3(SYS_BLOCKINFO,index,(int)info,0); }
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

/* кэш DNS: браузер спрашивает одно и то же имя на каждую картинку и скрипт */
#define DNS_CACHE 32
static struct { char name[64]; unsigned ip, until; int err; } dns_cache[DNS_CACHE];
static int dns_next;

static int dns_resolve(const char *name, unsigned *ip);

int resolve(const char *name, unsigned *ip) {
    if (ip_parse(name, ip)) return 0;
    if (!strcmp(name, "localhost")) { *ip = IP(127, 0, 0, 1); return 0; }
    char low[64];
    int n = 0;
    for (; name[n] && n < 63; n++) low[n] = tolower(name[n]);
    low[n] = 0;
    unsigned now = ticks();
    if (!name[n])
        for (int i = 0; i < DNS_CACHE; i++)
            if (dns_cache[i].name[0] && !strcmp(dns_cache[i].name, low) && (int)(dns_cache[i].until - now) > 0) {
                *ip = dns_cache[i].ip;
                return dns_cache[i].err;
            }
    int r = dns_resolve(name, ip);
    if (!name[n] && (r == 0 || r == -ENOENT)) {           /* ответ (или «нет такого имени») запомним */
        int k = dns_next++ % DNS_CACHE;
        strcpy(dns_cache[k].name, low);
        dns_cache[k].ip = r ? 0 : *ip;
        dns_cache[k].err = r;
        dns_cache[k].until = now + (r ? 30000 : 300000);  /* 30 с для ошибки, 5 минут для адреса */
    }
    return r;
}

static int dns_resolve(const char *name, unsigned *ip) {
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

int tcp_connect(const char *host, int port) { return tcp_connect_timeout(host, port, 0); }

int tcp_connect_timeout(const char *host, int port, unsigned ms) {
    unsigned ip;
    int r = resolve(host, &ip);
    if (r < 0) return r;
    int fd = socket(SOCK_TCP);
    if (fd < 0) return fd;
    if (ms) sockctl(fd, SOCK_CONNTIMEO, ms);
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



int account(int op,const char *name,const char *password) { return syscall3(SYS_ACCOUNT,op,(int)name,(int)password); }

/* ================================================================ 64-битная арифметика
 * Имена — как у libgcc: gcc -m32 без libgcc зовёт их из программ и из самого cc.
 * Внутри только 32-битные операции, иначе помощник вызвал бы сам себя. */

typedef union {
    unsigned long long u;
    struct { unsigned lo, hi; } p;
} di_parts;

static void mul32_full(unsigned a, unsigned b, unsigned *lo, unsigned *hi) {
    unsigned a0 = a & 0xFFFF, a1 = a >> 16, b0 = b & 0xFFFF, b1 = b >> 16;
    unsigned p0 = a0 * b0, p1 = a0 * b1, p2 = a1 * b0, p3 = a1 * b1;
    unsigned mid = (p0 >> 16) + (p1 & 0xFFFF) + (p2 & 0xFFFF);
    *lo = (p0 & 0xFFFF) | (mid << 16);
    *hi = p3 + (p1 >> 16) + (p2 >> 16) + (mid >> 16);
}

unsigned long long __muldi3(unsigned long long a, unsigned long long b) {
    di_parts A, B, R;
    unsigned hi, extra, ign;
    A.u = a; B.u = b;
    mul32_full(A.p.lo, B.p.lo, &R.p.lo, &hi);
    mul32_full(A.p.lo, B.p.hi, &extra, &ign); hi += extra;
    mul32_full(A.p.hi, B.p.lo, &extra, &ign); hi += extra;
    R.p.hi = hi;
    return R.u;
}

long long __ashldi3(long long a, int n) {
    di_parts x; x.u = (unsigned long long)a;
    if (n <= 0) return a;
    n &= 63;
    if (n >= 32) { x.p.hi = x.p.lo << (n - 32); x.p.lo = 0; }
    else { x.p.hi = (x.p.hi << n) | (x.p.lo >> (32 - n)); x.p.lo <<= n; }
    return (long long)x.u;
}

unsigned long long __lshrdi3(unsigned long long a, int n) {
    di_parts x; x.u = a;
    if (n <= 0) return a;
    n &= 63;
    if (n >= 32) { x.p.lo = x.p.hi >> (n - 32); x.p.hi = 0; }
    else { x.p.lo = (x.p.lo >> n) | (x.p.hi << (32 - n)); x.p.hi >>= n; }
    return x.u;
}

long long __ashrdi3(long long a, int n) {
    di_parts x; x.u = (unsigned long long)a;
    if (n <= 0) return a;
    n &= 63;
    if (n >= 32) { x.p.lo = (unsigned)((int)x.p.hi >> (n - 32)); x.p.hi = (unsigned)((int)x.p.hi >> 31); }
    else {
        x.p.lo = (x.p.lo >> n) | (x.p.hi << (32 - n));
        x.p.hi = (unsigned)((int)x.p.hi >> n);
    }
    return (long long)x.u;
}

unsigned long long __udivdi3(unsigned long long n, unsigned long long d) {
    di_parts N, D, Q, R;
    if (!d) return 0;
    N.u = n; D.u = d; Q.u = 0; R.u = 0;
    for (int i = 63; i >= 0; i--) {
        unsigned bit = i >= 32 ? (N.p.hi >> (i - 32)) & 1 : (N.p.lo >> i) & 1;
        R.p.hi = (R.p.hi << 1) | (R.p.lo >> 31);
        R.p.lo = (R.p.lo << 1) | bit;
        if (R.p.hi > D.p.hi || (R.p.hi == D.p.hi && R.p.lo >= D.p.lo)) {
            unsigned borrow = R.p.lo < D.p.lo;
            R.p.lo -= D.p.lo;
            R.p.hi = R.p.hi - D.p.hi - borrow;
            if (i >= 32) Q.p.hi |= 1u << (i - 32);
            else Q.p.lo |= 1u << i;
        }
    }
    return Q.u;
}

unsigned long long __umoddi3(unsigned long long a, unsigned long long b) {
    unsigned long long q = __udivdi3(a, b);
    return a - q * b;
}

/* gcc -m32 складывает «частное и остаток» в один вызов */
unsigned long long __udivmoddi4(unsigned long long n, unsigned long long d, unsigned long long *rp) {
    unsigned long long q = __udivdi3(n, d);
    if (rp) *rp = n - q * d;
    return q;
}

static unsigned long long di_neg(unsigned long long u) { return ~u + 1; }

long long __divdi3(long long a, long long b) {
    di_parts A, B, Q;
    int neg = 0;
    A.u = (unsigned long long)a; B.u = (unsigned long long)b;
    if (A.p.hi & 0x80000000u) { neg ^= 1; A.u = di_neg(A.u); }
    if (B.p.hi & 0x80000000u) { neg ^= 1; B.u = di_neg(B.u); }
    Q.u = __udivdi3(A.u, B.u);
    if (neg) Q.u = di_neg(Q.u);
    return (long long)Q.u;
}

long long __moddi3(long long a, long long b) {
    di_parts A, B, R;
    int neg = 0;
    A.u = (unsigned long long)a; B.u = (unsigned long long)b;
    if (A.p.hi & 0x80000000u) { neg = 1; A.u = di_neg(A.u); }
    if (B.p.hi & 0x80000000u) B.u = di_neg(B.u);
    R.u = __umoddi3(A.u, B.u);
    if (neg) R.u = di_neg(R.u);
    return (long long)R.u;
}

/* ================================================================ stdio */

enum { FF_READ = 1, FF_WRITE = 2, FF_EOF = 4, FF_ERR = 8, FF_UNGET = 16 };

static FILE stdfiles[8];
static FILE *stdpool;
static int stdused;
FILE *stdin, *stdout, *stderr;
char *__progname;
char *optarg;
int optind = 1, opterr = 1, optopt;

void __libc_init(void) {
    static int once;
    if (once) return;
    once = 1;
    stdfiles[0].fd = 0; stdfiles[0].flags = FF_READ;
    stdfiles[1].fd = 1; stdfiles[1].flags = FF_WRITE;
    stdfiles[2].fd = 2; stdfiles[2].flags = FF_WRITE;
    stdin = &stdfiles[0];
    stdout = &stdfiles[1];
    stderr = &stdfiles[2];
    stdpool = stdfiles + 3;
    stdused = 0;
}

int __libc_start(int argc, char **argv) {
    __libc_init();
    if (argc > 0 && argv && argv[0]) {
        char *s = argv[0], *b = s;
        while (*s) { if (*s == '/') b = s + 1; s++; }
        __progname = b;
    }
    /* main бывает и main(void): зовём через указатель, лишние аргументы просто лежат на стеке */
    int (*entry)(int, char **) = (int (*)(int, char **))main;
    exit(entry(argc, argv));
}

static FILE *file_new(int fd, int flags) {
    __libc_init();
    FILE *f;
    if (stdused < 5) f = &stdpool[stdused++];
    else {
        f = malloc(sizeof *f);
        if (!f) return NULL;
    }
    memset(f, 0, sizeof *f);
    f->fd = fd;
    f->flags = flags;
    return f;
}

static int mode_flags(const char *mode, int *acc) {
    int wr = 0, rd = 0, plus = 0;
    if (!mode || !mode[0]) return -1;
    if (mode[0] == 'r') rd = 1;
    else if (mode[0] == 'w' || mode[0] == 'a') wr = 1;
    else return -1;
    for (const char *p = mode + 1; *p; p++) if (*p == '+') plus = 1;
    if (plus) rd = wr = 1;
    *acc = (rd && wr) ? FF_READ | FF_WRITE : wr ? FF_WRITE : FF_READ;
    if (mode[0] == 'w') return (plus ? O_RDWR : O_WRONLY) | O_CREAT | O_TRUNC;
    if (mode[0] == 'a') return (plus ? O_RDWR : O_WRONLY) | O_CREAT | O_APPEND;
    return plus ? O_RDWR : O_RDONLY;
}

FILE *fopen(const char *path, const char *mode) {
    int acc, flags = mode_flags(mode, &acc);
    if (flags < 0) return NULL;
    int fd = open(path, flags);
    if (fd < 0) return NULL;
    return file_new(fd, acc);
}

FILE *fdopen(int fd, const char *mode) {
    int acc, flags = mode_flags(mode, &acc);
    if (flags < 0 || fd < 0) return NULL;
    return file_new(fd, acc);
}

int fclose(FILE *f) {
    int r = 0;
    if (!f) return EOF;
    if (f->mem) { free(f->mem); f->mem = NULL; }
    if (f->fd >= 0) r = close(f->fd);
    f->fd = -1;
    return r < 0 ? EOF : 0;
}

FILE *fmemopen(void *buf, size_t size, const char *mode) {
    FILE *f;
    char *m;
    (void)mode;
    if (size > 1024 * 1024) return NULL;
    f = file_new(-1, FF_READ);
    if (!f) return NULL;
    m = malloc(size ? size : 1);
    if (!m) return NULL;
    if (size && buf) memcpy(m, buf, size);
    f->mem = m;
    f->mem_pos = 0;
    f->mem_len = (int)size;
    return f;
}

int fflush(FILE *f) { (void)f; return 0; }
int fileno(FILE *f) { return f ? f->fd : -1; }
int feof(FILE *f) { return f && (f->flags & FF_EOF); }
int ferror(FILE *f) { return f && (f->flags & FF_ERR); }
void clearerr(FILE *f) { if (f) f->flags &= ~(FF_EOF | FF_ERR); }
void rewind(FILE *f) { fseek(f, 0, SEEK_SET); }

int fseek(FILE *f, long off, int whence) {
    if (!f) return -1;
    int r = lseek(f->fd, off, whence);
    if (r < 0) { f->flags |= FF_ERR; return -1; }
    f->flags &= ~(FF_EOF | FF_UNGET);
    return 0;
}

long ftell(FILE *f) {
    if (!f) return -1;
    int r = lseek(f->fd, 0, SEEK_CUR);
    return r < 0 ? -1 : r;
}

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *f) {
    __libc_init();
    if (!f || !size || !nmemb) return 0;
    unsigned char *d = ptr;
    int want = (int)(size * nmemb), got = 0;
    if (f->flags & FF_UNGET) {
        d[got++] = (unsigned char)(f->flags >> 16);
        f->flags &= ~FF_UNGET;
    }
    while (got < want) {
        int r;
        if (f->mem) {
            if (f->mem_pos >= f->mem_len) { f->flags |= FF_EOF; break; }
            d[got++] = (unsigned char)f->mem[f->mem_pos++];
            continue;
        }
        r = read(f->fd, d + got, (size_t)(want - got));
        if (r < 0) { f->flags |= FF_ERR; break; }
        if (r == 0) { f->flags |= FF_EOF; break; }
        got += r;
    }
    return (size_t)got / size;
}

size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *f) {
    __libc_init();
    if (!f || !size || !nmemb) return 0;
    int want = (int)(size * nmemb), off = 0;
    const char *s = ptr;
    while (off < want) {
        int r = write(f->fd, s + off, (size_t)(want - off));
        if (r <= 0) { f->flags |= FF_ERR; break; }
        off += r;
    }
    return (size_t)off / size;
}

int ungetc(int c, FILE *f) {
    if (!f || c == EOF || (f->flags & FF_UNGET)) return EOF;
    f->flags = (f->flags & 0xFFFF) | FF_UNGET | ((c & 0xFF) << 16);
    f->flags &= ~FF_EOF;
    return c & 0xFF;
}

int fgetc(FILE *f) {
    unsigned char c;
    if (fread(&c, 1, 1, f) != 1) return EOF;
    return c;
}

int fputc(int c, FILE *f) {
    unsigned char b = (unsigned char)c;
    return fwrite(&b, 1, 1, f) == 1 ? b : EOF;
}

char *fgets(char *s, int n, FILE *f) {
    int i = 0;
    if (!s || n <= 0) return NULL;
    while (i < n - 1) {
        int c = fgetc(f);
        if (c == EOF) { if (i == 0) return NULL; break; }
        s[i++] = (char)c;
        if (c == '\n') break;
    }
    s[i] = 0;
    return s;
}

int fputs(const char *s, FILE *f) {
    int n = (int)strlen(s);
    return fwrite(s, 1, (size_t)n, f) == (size_t)n ? 0 : EOF;
}

int vfprintf(FILE *f, const char *fmt, va_list ap) {
    char stack[256];
    va_list ap2;
    __libc_init();
    va_copy(ap2, ap);
    int n = vsnprintf(stack, sizeof stack, fmt, ap);
    if (n < 0) { va_end(ap2); return -1; }
    if (n < (int)sizeof stack) {
        va_end(ap2);
        return fwrite(stack, 1, (size_t)n, f) == (size_t)n ? n : -1;
    }
    char *b = malloc((size_t)n + 1);
    if (!b) { va_end(ap2); return -1; }
    vsnprintf(b, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    int rc = fwrite(b, 1, (size_t)n, f) == (size_t)n ? n : -1;
    free(b);
    return rc;
}

int fprintf(FILE *f, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vfprintf(f, fmt, ap);
    va_end(ap);
    return n;
}

int vprintf(const char *fmt, va_list ap) { __libc_init(); return vfprintf(stdout, fmt, ap); }

int vsprintf(char *buf, const char *fmt, va_list ap) { return vsnprintf(buf, 0x7fffffff, fmt, ap); }

int sprintf(char *buf, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsprintf(buf, fmt, ap);
    va_end(ap);
    return n;
}

void perror(const char *s) {
    __libc_init();
    if (s && *s) fprintf(stderr, "%s: %s\n", s, strerror(errno));
    else fprintf(stderr, "%s\n", strerror(errno));
}

ssize_t getline(char **line, size_t *n, FILE *stream) {
    size_t i = 0;
    if (!line || !n) return -1;
    if (!*line || *n < 64) {
        size_t cap = 64;
        char *p = realloc(*line, cap);
        if (!p) return -1;
        *line = p; *n = cap;
    }
    for (;;) {
        int c = fgetc(stream);
        if (c == EOF) return i ? (ssize_t)i : -1;
        if (i + 2 > *n) {
            size_t cap = *n * 2;
            char *p = realloc(*line, cap);
            if (!p) return -1;
            *line = p; *n = cap;
        }
        (*line)[i++] = (char)c;
        if (c == '\n') break;
    }
    (*line)[i] = 0;
    return (ssize_t)i;
}

int fstat(int fd, struct stat *st) { return ioerr(syscall3(SYS_FSTAT, fd, (int)st, 0)); }

int stat(const char *path, struct stat *st) {
    struct dirent_info info;
    int r = syscall3(SYS_STAT, (int)path, (int)&info, 0);
    if (r < 0) { errno = -r; return -1; }
    if (info.type == 2) st->st_mode = S_IFDIR;
    else if (info.type == 4) st->st_mode = S_IFCHR;
    else if (info.type == 3) st->st_mode = 0060000;
    else st->st_mode = S_IFREG;
    st->st_size = info.size;
    return 0;
}

int rename(const char *old, const char *new) {
    return ioerr(syscall3(SYS_RENAME, (int)old, (int)new, 0));
}

int remove(const char *path) {
    return ioerr(syscall3(SYS_UNLINK, (int)path, 0, 0));
}

struct DIR {
    char path[128];
    int index;
    struct dirent ent;
};

DIR *opendir(const char *name) {
    struct dirent_info info;
    DIR *d;
    size_t n;
    int r = syscall3(SYS_STAT, (int)name, (int)&info, 0);
    if (r < 0) { errno = -r; return NULL; }
    if (info.type != 2) { errno = ENOTDIR; return NULL; }
    n = strlen(name);
    if (n == 0 || n >= 128) { errno = n ? ENAMETOOLONG : ENOENT; return NULL; }
    d = malloc(sizeof *d);
    if (!d) { errno = ENOMEM; return NULL; }
    memcpy(d->path, name, n + 1);
    d->index = 0;
    return d;
}

struct dirent *readdir(DIR *dir) {
    struct dirent_info info;
    int r, i;
    if (!dir) { errno = EBADF; return NULL; }
    r = syscall3(SYS_READDIR, (int)dir->path, dir->index, (int)&info);
    if (r == 0) return NULL;
    if (r < 0) { errno = -r; return NULL; }
    dir->index++;
    dir->ent.d_ino = 0;
    if (info.type == 2) dir->ent.d_type = DT_DIR;
    else if (info.type == 4) dir->ent.d_type = DT_CHR;
    else if (info.type == 3) dir->ent.d_type = DT_BLK;
    else dir->ent.d_type = DT_REG;
    for (i = 0; i < 255 && info.name[i]; i++) dir->ent.d_name[i] = info.name[i];
    dir->ent.d_name[i] = 0;
    return &dir->ent;
}

int closedir(DIR *dir) {
    if (!dir) { errno = EBADF; return -1; }
    free(dir);
    return 0;
}

static char *strip_slashes(char *path) {
    size_t n = strlen(path);
    while (n > 1 && path[n - 1] == '/') path[--n] = 0;
    return path;
}

char *basename(char *path) {
    char *s;
    if (!path || !*path) return ".";
    strip_slashes(path);
    if (!path[0] || (path[0] == '/' && path[1] == 0)) return path[0] ? path : "/";
    s = path + strlen(path);
    while (s > path && s[-1] != '/') s--;
    return s;
}

char *dirname(char *path) {
    char *s;
    if (!path || !*path) return ".";
    strip_slashes(path);
    if (!*path) return "/";
    s = path + strlen(path);
    while (s > path && s[-1] != '/') s--;
    if (s == path) return ".";
    if (s == path + 1 && path[0] == '/') return "/";
    s[-1] = 0;
    return path;
}

void abort(void) { exit(134); }

int getopt(int argc, char *const argv[], const char *optstring) {
    static int sp = 1;
    if (optind >= argc || !argv[optind] || argv[optind][0] != '-' || argv[optind][1] == 0) return -1;
    if (argv[optind][1] == '-' && argv[optind][2] == 0) { optind++; return -1; }
    char c = argv[optind][sp];
    const char *p = strchr(optstring, c);
    optopt = c;
    if (!p || c == ':') {
        if (opterr && optstring[0] != ':') fprintf(stderr, "%s: illegal option -- %c\n", __progname ? __progname : "", c);
        if (!argv[optind][++sp]) { optind++; sp = 1; }
        return '?';
    }
    if (p[1] == ':') {
        if (argv[optind][sp + 1]) { optarg = &argv[optind][sp + 1]; optind++; sp = 1; }
        else if (optind + 1 < argc) { optarg = argv[++optind]; optind++; sp = 1; }
        else {
            if (opterr && optstring[0] != ':') fprintf(stderr, "%s: option requires an argument -- %c\n", __progname ? __progname : "", c);
            optind++; sp = 1;
            return optstring[0] == ':' ? ':' : '?';
        }
    } else if (!argv[optind][++sp]) { optind++; sp = 1; }
    return (unsigned char)c;
}

static void vmsg(int status, const char *fmt, va_list ap, int with_errno) {
    __libc_init();
    fprintf(stderr, "%s: ", __progname ? __progname : "");
    if (fmt && *fmt) {
        vfprintf(stderr, fmt, ap);
        if (with_errno) fprintf(stderr, ": ");
    }
    if (with_errno) fprintf(stderr, "%s", strerror(errno));
    fprintf(stderr, "\n");
    if (status) exit(status);
}

void warn(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vmsg(0, fmt, ap, 1); va_end(ap); }
void warnx(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vmsg(0, fmt, ap, 0); va_end(ap); }
void err(int status, const char *fmt, ...) { va_list ap; va_start(ap, fmt); vmsg(status ? status : 1, fmt, ap, 1); va_end(ap); exit(1); }
void errx(int status, const char *fmt, ...) { va_list ap; va_start(ap, fmt); vmsg(status ? status : 1, fmt, ap, 0); va_end(ap); exit(1); }

int pledge(const char *promises, const char *execpromises) { (void)promises; (void)execpromises; return 0; }
char *setlocale(int category, const char *locale) { (void)category; (void)locale; return (char *)"C"; }
int mbtowc(wchar_t *wc, const char *s, size_t n) {
    if (!s) return 0;
    if (!n || !*s) return 0;
    if (wc) *wc = (unsigned char)*s;
    return 1;
}
int iswspace(wchar_t wc) { return isspace((unsigned char)wc); }
int fmt_scaled(long long number, char *result) {
    snprintf(result, FMT_SCALED_STRSIZE, "%lld", number);
    return 0;
}

/* ================================================================ числа, сортировка, POSIX-мелочь */

static int conv_digit(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'z') return c - 'a' + 10;
    if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
    return -1;
}

/* lim — наибольшее по модулю значение. Для знакового отрицательного это 2^63. */
static unsigned long long parse_int(const char *nptr, char **end, int base, int *neg, int *ovf) {
    const char *s = nptr;
    while (isspace((unsigned char)*s)) s++;
    *neg = 0;
    if (*s == '+' || *s == '-') { *neg = *s == '-'; s++; }
    if (base == 0) {
        if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X') && conv_digit((unsigned char)s[2]) >= 0 && conv_digit((unsigned char)s[2]) < 16) {
            base = 16; s += 2;
        } else if (s[0] == '0') base = 8;
        else base = 10;
    } else if (base == 16 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X') && conv_digit((unsigned char)s[2]) >= 0 && conv_digit((unsigned char)s[2]) < 16)
        s += 2;
    *ovf = 0;
    if (base < 2 || base > 36) { errno = EINVAL; if (end) *end = (char *)nptr; return 0; }
    unsigned long long acc = 0;
    int any = 0;
    for (;;) {
        int d = conv_digit((unsigned char)*s);
        if (d < 0 || d >= base) break;
        any = 1;
        if (acc > (~0ULL - (unsigned)d) / (unsigned)base) *ovf = 1;
        else acc = acc * (unsigned)base + (unsigned)d;
        s++;
    }
    if (end) *end = (char *)(any ? s : nptr);
    return any ? acc : 0;
}

long long strtoll(const char *nptr, char **end, int base) {
    int neg, ovf;
    unsigned long long lim, acc = parse_int(nptr, end, base, &neg, &ovf);
    if (errno == EINVAL && end && *end == nptr && acc == 0) return 0;
    lim = neg ? 9223372036854775808ULL : 9223372036854775807ULL;
    if (ovf || acc > lim) {
        errno = ERANGE;
        return neg ? (-9223372036854775807LL - 1) : 9223372036854775807LL;
    }
    return neg ? -(long long)acc : (long long)acc;
}

long strtol(const char *nptr, char **end, int base) {
    int saved = errno;
    errno = 0;
    long long v = strtoll(nptr, end, base);
    if (errno == EINVAL) return 0;
    if (errno == ERANGE || v > 2147483647LL || v < -2147483647LL - 1) {
        errno = ERANGE;
        return v < 0 ? (long)(-2147483647L - 1) : 2147483647L;
    }
    errno = saved;
    return (long)v;
}

unsigned long long strtoull(const char *nptr, char **end, int base) {
    int neg, ovf;
    unsigned long long acc = parse_int(nptr, end, base, &neg, &ovf);
    if (ovf) { errno = ERANGE; return ~0ULL; }
    if (neg) acc = -acc;
    return acc;
}

unsigned long strtoul(const char *nptr, char **end, int base) {
    int neg, ovf, saved = errno;
    unsigned long long acc;
    errno = 0;
    acc = parse_int(nptr, end, base, &neg, &ovf);
    if (errno == EINVAL) return 0;
    if (ovf || acc > 4294967295ULL) { errno = ERANGE; return 4294967295UL; }
    errno = saved;
    if (neg) return (unsigned long)(0UL - (unsigned long)acc);
    return (unsigned long)acc;
}

char *strdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

char *strndup(const char *s, size_t n) {
    size_t i = 0;
    char *p;
    while (i < n && s[i]) i++;
    p = malloc(i + 1);
    if (!p) return NULL;
    memcpy(p, s, i);
    p[i] = 0;
    return p;
}

static int fold_ascii(int ch) { return (ch >= 'A' && ch <= 'Z') ? ch - 'A' + 'a' : ch; }

int strcasecmp(const char *a, const char *b) {
    for (;;) {
        int ca = fold_ascii((unsigned char)*a), cb = fold_ascii((unsigned char)*b);
        if (ca != cb || !ca) return ca - cb;
        a++; b++;
    }
}

int strncasecmp(const char *a, const char *b, size_t n) {
    while (n--) {
        int ca = fold_ascii((unsigned char)*a), cb = fold_ascii((unsigned char)*b);
        if (ca != cb || !ca) return ca - cb;
        a++; b++;
    }
    return 0;
}

char *strcasestr(const char *h, const char *n) {
    size_t l = strlen(n);
    if (!l) return (char *)h;
    for (; *h; h++) if (!strncasecmp(h, n, l)) return (char *)h;
    return NULL;
}

long long strtonum(const char *nptr, long long minval, long long maxval, const char **errstr) {
    char *ep;
    int saved = errno;
    errno = 0;
    long long v = strtoll(nptr, &ep, 10);
    if (errstr) *errstr = NULL;
    if (nptr == ep || *ep) {
        errno = EINVAL;
        if (errstr) *errstr = "invalid";
        return 0;
    }
    if (errno == ERANGE || v < minval || v > maxval) {
        errno = ERANGE;
        if (errstr) *errstr = v < minval ? "too small" : "too large";
        return 0;
    }
    errno = saved;
    return v;
}

static void memswap(char *a, char *b, size_t n) {
    while (n--) { char t = *a; *a++ = *b; *b++ = t; }
}

static void sift_down(char *b, size_t root, size_t count, size_t size, int (*cmp)(const void *, const void *)) {
    while (root * 2 + 1 < count) {
        size_t child = root * 2 + 1;
        if (child + 1 < count && cmp(b + child * size, b + (child + 1) * size) < 0) child++;
        if (cmp(b + root * size, b + child * size) >= 0) return;
        memswap(b + root * size, b + child * size, size);
        root = child;
    }
}

void qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *)) {
    char *b = base;
    size_t start, end;
    if (n < 2 || !size || !cmp) return;
    for (start = n / 2; start > 0; start--) sift_down(b, start - 1, n, size, cmp);
    for (end = n; end > 1; end--) {
        memswap(b, b + (end - 1) * size, size);
        sift_down(b, 0, end - 1, size, cmp);
    }
}

char *getenv(const char *name) { (void)name; return NULL; }

int setvbuf(FILE *f, char *buf, int mode, size_t size) {
    (void)f; (void)buf; (void)mode; (void)size;
    return 0;
}
void setbuf(FILE *f, char *buf) { setvbuf(f, buf, buf ? _IOFBF : _IONBF, BUFSIZ); }

int isatty(int fd) {
#if defined(__LINUX_TEST__)
    struct { unsigned short row, col, xpixel, ypixel; } ws;
    return __syscall(54, fd, 0x5413, (int)&ws) == 0;
#else
    struct stat st;
    if (fstat(fd, &st) < 0) return 0;
    return S_ISCHR(st.st_mode);
#endif
}

unsigned sleep(unsigned seconds) {
#if defined(__LINUX_TEST__)
    struct { long sec, nsec; } req, rem;
    req.sec = (long)seconds; req.nsec = 0; rem.sec = 0; rem.nsec = 0;
    if (__syscall(162, (int)&req, (int)&rem) < 0) return seconds;
    return (unsigned)rem.sec;
#else
    while (seconds) {
        unsigned chunk = seconds > 1000u ? 1000u : seconds;
        sleep_ms(chunk * 1000u);
        seconds -= chunk;
    }
    return 0;
#endif
}

uid_t getuid(void) { return 0; }
uid_t geteuid(void) { return 0; }
gid_t getgid(void) { return 0; }

static int leap(unsigned y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

time_t time(time_t *tloc) {
    time_t t;
#if defined(__LINUX_TEST__)
    t = (time_t)__syscall(13, 0);
#else
    static const unsigned md[12] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
    struct datetime d;
    unsigned y;
    long long days;
    if (get_time(&d) < 0 || d.year < 1970 || d.month < 1 || d.month > 12) return (time_t)-1;
    days = 0;
    for (y = 1970; y < d.year; y++) days += leap(y) ? 366 : 365;
    days += md[d.month - 1];
    if (d.month > 2 && leap(d.year)) days++;
    days += (long)d.day - 1;
    days = days * 86400LL + (long long)d.hour * 3600 + (long long)d.min * 60 + d.sec;
    if (days < 0 || days > 2147483647LL) return (time_t)-1;
    t = (time_t)days;
#endif
    if (tloc && t != (time_t)-1) *tloc = t;
    return t;
}

#include "regex.c"
