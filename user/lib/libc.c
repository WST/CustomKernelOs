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
#include "math.h"
#include "locale.h"
#include "signal.h"
#include "fcntl.h"
#include "pwd.h"

/* Filesystem administration wrappers are defined below syscall3. */

/* ================================================================ точка входа */

int main(int argc, char **argv);

int __libc_start(int argc, char **argv) __attribute__((noreturn));

#if !defined(__MYOS_CC__) && !defined(__LIBC_SO__)
/* Ядро «вызывает» _start(argc, argv): кладёт их на стек как аргументы функции.
 * (Программам, собранным cc внутри MyOS, такую же точку входа делает сам cc.
 *  В libc.so точки входа нет: её даёт crt0 самой программы.) */
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
            for (i = 0; i < 255 && name[i]; i++) o->name[i] = name[i];
            if (typ == 4) o->type = 2;
            else if (typ == 2) o->type = 4;
            else if (typ == 6) o->type = 3;
            else if (typ == 10) o->type = 5;
            else o->type = 1;
            {
                char full[512];
                unsigned char st[64];
                unsigned mode;
                if (snprintf(full, sizeof full, "%s/%s", path, o->name) < (int)sizeof full &&
                    __syscall(106, (int)full, (int)st, 0) == 0) {
                    mode = *(unsigned short *)(st + 8);
                    o->size = *(unsigned *)(st + 20);
                    if ((mode & 0170000) == 0040000) o->type = 2;
                    else if ((mode & 0170000) == 0020000) o->type = 4;
                    else if ((mode & 0170000) == 0060000) o->type = 3;
                    else if ((mode & 0170000) == 0120000) o->type = 5;
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
    case SYS_CHDIR: return __syscall(12, a);
    case SYS_GETCWD: return __syscall(183, a, b);
    case SYS_FORK:  return __syscall(2);
    case SYS_EXEC: {
        static char *envp[] = { 0 };
        return __syscall(11, a, b, (int)envp);
    }
    case SYS_WAITPID: {
        int st = 0, r = __syscall(7, a, (int)&st, c);
        if (r < 0) return r;
        if (b) {
            int code = (st & 0x7f) ? 128 + (st & 0x7f) : (st >> 8) & 0xff;
            *(int *)b = code;
        }
        return r;
    }
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
        else if ((mode & 0170000) == 0120000) o->type = 5;
        else o->type = 1;
        return 0;
    }
    case SYS_LSTAT: {
        unsigned char st[64];
        int r = __syscall(107, a, (int)st, 0);
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
        else if ((mode & 0170000) == 0120000) o->type = 5;
        else o->type = 1;
        return 0;
    }
    case SYS_SYMLINK: return __syscall(83, a, b);
    case SYS_READLINK: return __syscall(85, a, b, c);
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

struct mmap_args { unsigned addr, len, prot, flags, fd, off; };

void *mmap(void *addr, unsigned len, int prot, int flags, int fd, unsigned off) {
    struct mmap_args a;
    a.addr = (unsigned)addr; a.len = len; a.prot = prot; a.flags = flags; a.fd = fd; a.off = off;
    unsigned r = syscall3(SYS_MMAP, (int)&a, 0, 0);
    if (r >= 0xfffff001u) { errno = -(int)r; return (void *)-1; }
    return (void *)r;
}
int munmap(void *addr, unsigned len)             { return syscall3(SYS_MUNMAP, (int)addr, len, 0); }
int mprotect(void *addr, unsigned len, int prot) { return syscall3(SYS_MPROTECT, (int)addr, len, prot); }
void  sleep_ms(unsigned ms)                      { syscall3(SYS_SLEEP, ms, 0, 0); }
int   getpid(void)                               { return syscall3(SYS_GETPID, 0, 0, 0); }
unsigned ticks(void)                             { return syscall3(SYS_TICKS, 0, 0, 0); }
void  yield(void)                                { syscall3(SYS_YIELD, 0, 0, 0); }
int   spawn(const char *path, char **argv, int stdin_fd) { return syscall3(SYS_SPAWN, (int)path, (int)argv, stdin_fd); }
static void thread_entry(void (*fn)(void *), void *arg) {
    fn(arg); thread_exit(0);
}
int thread_create(void (*fn)(void *), void *arg, void *stack, unsigned size) {
    return syscall5(SYS_THREAD_CREATE,(int)thread_entry,(int)fn,(int)arg,(int)stack,size);
}
int thread_join(int tid,int *code) { return syscall3(SYS_THREAD_JOIN,tid,(int)code,0); }
void thread_exit(int code) { syscall3(SYS_THREAD_EXIT,code,0,0); for (;;) {} }
int gettid(void) { return syscall3(SYS_GETTID,0,0,0); }
int thread_mutex_lock(thread_mutex_t *m) { return syscall3(SYS_THREAD_MUTEX,(int)m,0,0); }
int thread_mutex_unlock(thread_mutex_t *m) { return syscall3(SYS_THREAD_MUTEX,(int)m,1,0); }
int   fork(void)                                 { return syscall3(SYS_FORK, 0, 0, 0); }
int   exec(const char *path, char **argv)        { return syscall3(SYS_EXEC, (int)path, (int)argv, 0); }
int   execve(const char *path, char *const argv[], char *const envp[]) {
    (void)envp; return exec(path, (char **)argv);
}
int   waitpid(int pid, int *status, int options) { return syscall3(SYS_WAITPID, pid, (int)status, options); }
int   getppid(void)                              { return syscall3(SYS_GETPPID, 0, 0, 0); }
int   kill(int pid, ...)                         { return syscall3(SYS_KILL, pid, 0, 0); }
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
int clipboard_set(const char *s, unsigned n) { return syscall3(SYS_CLIPBOARD, 1, (int)s, n); }
int clipboard_get(char *s, unsigned n) { return syscall3(SYS_CLIPBOARD, 0, (int)s, n); }

static int key_termsize;
int readkey(void) {
    int size = syscall3(SYS_TERMSIZE, 0, 0, 0);
    if (key_termsize && size != key_termsize) { key_termsize = size; return KEY_RESIZE; }
    key_termsize = size;
    unsigned char c;
    /* Only readkey opts into resize interruption. Raw read callers (notably
     * login/password input) must never see resize as EOF or an input error. */
    syscall3(SYS_TTYMODE, cur_tty | TTY_RESIZE_EVENT, 0, 0);
    int count = read(STDIN, &c, 1);
    syscall3(SYS_TTYMODE, cur_tty, 0, 0);
    if (count <= 0) {
        size = syscall3(SYS_TERMSIZE, 0, 0, 0);
        if (size != key_termsize) { key_termsize = size; return KEY_RESIZE; }
        return -1;
    }
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
    if (b >= 'a' && b <= 'l') {
        static const int shifted[] = { KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT,
            KEY_HOME, KEY_END, KEY_PGUP, KEY_PGDN, KEY_DEL, KEY_INS };
        if (b == 'k') return KEY_COPY;
        if (b == 'l') return KEY_PASTE;
        return shifted[b - 'a'] | KEY_SHIFT;
    }
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
int islower(int c) { return c >= 'a' && c <= 'z'; }
int isupper(int c) { return c >= 'A' && c <= 'Z'; }
int isxdigit(int c) { return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
int iscntrl(int c) { return (c >= 0 && c < 32) || c == 127; }
int isprint(int c) { return c >= 32 && c < 127; }
int isgraph(int c) { return c > 32 && c < 127; }
int ispunct(int c) { return isgraph(c) && !isalnum(c); }
int abs(int x) { return x < 0 ? -x : x; }
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
void *memchr(const void *s, int c, size_t n) {
    const unsigned char *p = s;
    while (n--) { if (*p == (unsigned char)c) return (void *)p; p++; }
    return NULL;
}
char *strncat(char *d, const char *s, size_t n) {
    size_t i = strlen(d), k = 0;
    while (k < n && s[k]) d[i++] = s[k++];
    d[i] = 0;
    return d;
}
static int in_set(const char *set, int c) {
    while (*set) if (*set++ == c) return 1;
    return 0;
}
size_t strspn(const char *s, const char *accept) {
    size_t n = 0;
    while (s[n] && in_set(accept, (unsigned char)s[n])) n++;
    return n;
}
size_t strcspn(const char *s, const char *reject) {
    size_t n = 0;
    while (s[n] && !in_set(reject, (unsigned char)s[n])) n++;
    return n;
}
char *strpbrk(const char *s, const char *accept) {
    while (*s) { if (in_set(accept, (unsigned char)*s)) return (char *)s; s++; }
    return NULL;
}
int strcoll(const char *a, const char *b) { return strcmp(a, b); }
char *strtok(char *s, const char *delim) {
    static char *save;
    char *start;
    if (s) save = s;
    if (!save) return NULL;
    while (*save && in_set(delim, (unsigned char)*save)) save++;
    if (!*save) { save = NULL; return NULL; }
    start = save;
    while (*save && !in_set(delim, (unsigned char)*save)) save++;
    if (*save) *save++ = 0;
    else save = NULL;
    return start;
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
    case E2BIG:     return "argument list too long";
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
    case ELOOP:     return "too many levels of symbolic links";
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
                     int width, char pad, bool left, bool plus, bool blank, int prec, bool alt) {
    char tmp[16], pfx[2];
    const char *dig = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char sign = neg ? '-' : plus ? '+' : blank ? ' ' : 0;
    unsigned orig = v;
    int n = 0, zeros, pn = 0, total, i;
    if (!(prec == 0 && v == 0)) {
        do { tmp[n++] = dig[v % base]; v /= base; } while (v);
    }
    if (prec >= 0 && pad == '0') pad = ' ';
    zeros = prec > n ? prec - n : 0;
    if (alt && base == 8) {
        if (zeros == 0 && (n == 0 || tmp[n - 1] != '0')) zeros = 1;
    } else if (alt && base == 16 && orig != 0) {
        pfx[pn++] = '0';
        pfx[pn++] = upper ? 'X' : 'x';
    }
    total = n + zeros + pn + (sign ? 1 : 0);
    if (!left && pad == ' ') for (; total < width; width--) emit(o, ' ');
    if (sign) emit(o, sign);
    for (i = 0; i < pn; i++) emit(o, pfx[i]);
    if (!left && pad == '0') for (; total < width; width--) emit(o, '0');
    while (zeros--) emit(o, '0');
    while (n) emit(o, tmp[--n]);
    if (left) for (; total < width; width--) emit(o, ' ');
}

static void emit_u64(struct out *o, unsigned long long v, unsigned base, bool upper, bool neg,
                     int width, char pad, bool left, bool plus, bool blank, int prec, bool alt) {
    char tmp[32], pfx[2];
    const char *dig = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char sign = neg ? '-' : plus ? '+' : blank ? ' ' : 0;
    unsigned long long orig = v;
    int n = 0, zeros, pn = 0, total, i;
    if (!(prec == 0 && v == 0)) {
        do { tmp[n++] = dig[v % base]; v /= base; } while (v);
    }
    if (prec >= 0 && pad == '0') pad = ' ';
    zeros = prec > n ? prec - n : 0;
    if (alt && base == 8) {
        if (zeros == 0 && (n == 0 || tmp[n - 1] != '0')) zeros = 1;
    } else if (alt && base == 16 && orig != 0) {
        pfx[pn++] = '0';
        pfx[pn++] = upper ? 'X' : 'x';
    }
    total = n + zeros + pn + (sign ? 1 : 0);
    if (!left && pad == ' ') for (; total < width; width--) emit(o, ' ');
    if (sign) emit(o, sign);
    for (i = 0; i < pn; i++) emit(o, pfx[i]);
    if (!left && pad == '0') for (; total < width; width--) emit(o, '0');
    while (zeros--) emit(o, '0');
    while (n) emit(o, tmp[--n]);
    if (left) for (; total < width; width--) emit(o, ' ');
}

/* 10^e для |e| <= 308. Нужен печати и разбору вещественных, без math.c. */
static double dpow10(int e) {
    static const double p[] = { 1e1, 1e2, 1e4, 1e8, 1e16, 1e32, 1e64, 1e128, 1e256 };
    int neg = e < 0, i;
    double r = 1;
    if (neg) e = -e;
    if (e > 308) return neg ? 0 : 1e300 * 1e300;
    for (i = 0; e && i < 9; i++, e >>= 1)
        if (e & 1) r *= p[i];
    return neg ? 1.0 / r : r;
}

static int fkind(double v, int *sign) {
    unsigned long long u;
    memcpy(&u, &v, 8);
    *sign = (int)(u >> 63);
    if (((u >> 52) & 0x7ff) == 0x7ff) return (u << 12) ? 2 : 1; /* nan, inf */
    if ((u << 1) == 0) return 0;
    return 3;
}

/* Старшая десятичная степень: 10^e <= v < 10^(e+1), v > 0. */
static int dec_exp(double v) {
    int lo = -324, hi = 308, ans = -324;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (dpow10(mid) <= v) { ans = mid; lo = mid + 1; }
        else hi = mid - 1;
    }
    return ans;
}

/* prec значащих цифр числа v > 0. Возвращает степень первой цифры. */
static int sig_digits(double v, int prec, unsigned long long *digs) {
    int e = dec_exp(v), guard = 0;
    while (guard++ < 6) {
        double s = v / dpow10(e - (prec - 1));
        double hi = dpow10(prec);
        double lo = prec > 1 ? dpow10(prec - 1) : 1;
        unsigned long long n;
        if (!(s < hi)) { e++; continue; }
        if (s < lo) { e--; continue; }
        n = (unsigned long long)(s + 0.5);
        if ((double)n >= hi) { e++; continue; }
        if (prec > 1 && (double)n < lo) { e--; continue; }
        *digs = n;
        return e;
    }
    *digs = 0;
    return e;
}

static int ull_to_digits(unsigned long long v, char *dst) {
    char tmp[24];
    int n = 0, i;
    if (!v) { dst[0] = '0'; return 1; }
    while (v) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
    for (i = 0; i < n; i++) dst[i] = tmp[n - 1 - i];
    return n;
}

static void emit_text(struct out *o, const char *s, int width, bool left) {
    int n = (int)strlen(s), i;
    if (!left) for (i = n; i < width; i++) emit(o, ' ');
    for (i = 0; i < n; i++) emit(o, s[i]);
    if (left) for (i = n; i < width; i++) emit(o, ' ');
}

#define BIG_N 96
typedef struct { unsigned v[BIG_N]; int n; } Big;

static void big_set(Big *b, unsigned long long x) {
    int i;
    for (i = 0; i < BIG_N; i++) b->v[i] = 0;
    b->v[0] = (unsigned)x;
    b->v[1] = (unsigned)(x >> 32);
    b->n = b->v[1] ? 2 : (b->v[0] ? 1 : 1);
}

static void big_mul(Big *b, unsigned m) {
    unsigned c = 0;
    int i;
    for (i = 0; i < b->n; i++) {
        unsigned x = b->v[i];
        unsigned p0 = (x & 0xffff) * m;
        unsigned p1 = (x >> 16) * m;
        unsigned lo = (p0 & 0xffff) + (c & 0xffff);
        unsigned mid = (p0 >> 16) + (p1 & 0xffff) + (c >> 16) + (lo >> 16);
        unsigned hi = (p1 >> 16) + (mid >> 16);
        b->v[i] = (lo & 0xffff) | ((mid & 0xffff) << 16);
        c = hi;
    }
    if (c && b->n < BIG_N) b->v[b->n++] = c;
}

static void big_shl(Big *b, int sh) {
    int i;
    while (sh-- > 0) {
        unsigned c = 0;
        for (i = 0; i < b->n; i++) {
            unsigned n = (b->v[i] << 1) | c;
            c = b->v[i] >> 31;
            b->v[i] = n;
        }
        if (c && b->n < BIG_N) b->v[b->n++] = c;
    }
}

static int big_bit(const Big *b, int k) {
    int w, bit;
    if (k < 0) return 0;
    w = k >> 5;
    bit = k & 31;
    if (w >= b->n) return 0;
    return (int)((b->v[w] >> bit) & 1);
}

static void big_add1(Big *b) {
    unsigned c = 1;
    int i;
    for (i = 0; c && i < BIG_N; i++) {
        unsigned long long t = (unsigned long long)b->v[i] + c;
        if (i >= b->n) b->n = i + 1;
        b->v[i] = (unsigned)t;
        c = (unsigned)(t >> 32);
    }
}

static void big_shr_round(Big *b, int sh) {
    int words, bits, i, bit, sticky = 0;
    unsigned c;
    if (sh <= 0) return;
    for (i = 0; i < sh - 1; i++) if (big_bit(b, i)) { sticky = 1; break; }
    bit = big_bit(b, sh - 1);
    words = sh >> 5;
    bits = sh & 31;
    if (words >= b->n) {
        for (i = 0; i < BIG_N; i++) b->v[i] = 0;
        b->n = 1;
        if (bit && sticky) b->v[0] = 1;
        return;
    }
    if (words) {
        for (i = 0; i < b->n - words; i++) b->v[i] = b->v[i + words];
        for (; i < b->n; i++) b->v[i] = 0;
        b->n -= words;
    }
    if (bits) {
        c = 0;
        for (i = b->n - 1; i >= 0; i--) {
            unsigned n = (b->v[i] >> bits) | c;
            c = b->v[i] << (32 - bits);
            b->v[i] = n;
        }
        while (b->n > 1 && !b->v[b->n - 1]) b->n--;
    }
    if (bit && (sticky || (b->v[0] & 1))) big_add1(b);
}

static unsigned big_div10(Big *b) {
    unsigned rem = 0;
    int i;
    for (i = b->n - 1; i >= 0; i--) {
        unsigned long long cur = ((unsigned long long)rem << 32) | b->v[i];
        b->v[i] = (unsigned)(cur / 10);
        rem = (unsigned)(cur % 10);
    }
    while (b->n > 1 && !b->v[b->n - 1]) b->n--;
    return rem;
}

/* %.Nf для больших чисел: точное значение * 10^N, чтобы tonumber вернул тот же double. */
static void emit_fixed_big(struct out *o, double v, int prec, int sch) {
    unsigned long long u, mant;
    int be, e, i, nd, pos;
    char digs[640];
    Big b;
    memcpy(&u, &v, 8);
    be = (int)((u >> 52) & 0x7ff);
    mant = u & ((1ULL << 52) - 1);
    if (be == 0) e = -1022 - 52;
    else { mant |= 1ULL << 52; e = be - 1023 - 52; }
    big_set(&b, mant);
    for (i = 0; i < prec; i++) big_mul(&b, 5);
    if (e + prec >= 0) big_shl(&b, e + prec);
    else big_shr_round(&b, -(e + prec));
    nd = 0;
    do { digs[nd++] = (char)('0' + big_div10(&b)); } while ((b.n > 1 || b.v[0]) && nd < (int)sizeof digs);
    if (sch) emit(o, (char)sch);
    if (nd <= prec) {
        emit(o, '0');
        if (prec > 0) {
            emit(o, '.');
            for (i = nd; i < prec; i++) emit(o, '0');
            for (i = nd - 1; i >= 0; i--) emit(o, digs[i]);
        }
    } else {
        for (pos = nd - 1; pos >= prec; pos--) emit(o, digs[pos]);
        if (prec > 0) {
            emit(o, '.');
            for (; pos >= 0; pos--) emit(o, digs[pos]);
        }
    }
}
static void emit_float(struct out *o, double v, int spec, int prec, int width, char pad,
                       bool left, bool alt, bool plus, bool blank) {
    int sign = 0, kind = fkind(v, &sign), exp, nd, i;
    unsigned long long digs = 0;
    char num[40];
    char sch = sign ? '-' : plus ? '+' : blank ? ' ' : 0;
    int upper = spec == 'E' || spec == 'G';
    if (spec == 'E') spec = 'e';
    if (spec == 'G') spec = 'g';
    if (kind == 2 || kind == 1) {
        char tmp[8];
        int n = 0;
        if (sch) tmp[n++] = sch;
        tmp[n++] = kind == 2 ? 'n' : 'i';
        tmp[n++] = kind == 2 ? 'a' : 'n';
        tmp[n++] = kind == 2 ? 'n' : 'f';
        tmp[n] = 0;
        emit_text(o, tmp, width, left);
        return;
    }
    if (spec == 'g') {
        if (prec < 0) prec = 6;
        if (prec == 0) prec = 1;
        if (prec > 17) prec = 17;
    } else {
        if (prec < 0) prec = 6;
        if (prec > 500) prec = 500;
    }
    if (spec == 'f' && kind == 3 && prec > 15) {
        emit_fixed_big(o, sign ? -v : v, prec, sch);
        return;
    }
    if (kind == 0) {
        exp = 0;
        digs = 0;
        nd = 1;
        num[0] = '0';
    } else {
        int use = spec == 'e' ? prec + 1 : spec == 'g' ? prec : 17;
        double av = sign ? -v : v;
        if (use < 1) use = 1;
        if (use > 17) use = 17;
        if (spec == 'f' && prec >= 0 && av < 1e15 && (prec == 0 || av * dpow10(prec) < 1e15)) {
            double scale = prec ? dpow10(prec) : 1;
            unsigned long long n = (unsigned long long)(av * scale + 0.5);
            nd = ull_to_digits(n, num);
            exp = nd - (prec > 0 ? prec : 0) - 1;
            if (prec > 0 && nd <= prec) exp = -1 - (prec - nd);
        } else {
            exp = sig_digits(av, use, &digs);
            nd = ull_to_digits(digs, num);
            while (nd < use) {
                memmove(num + 1, num, (size_t)nd);
                num[0] = '0';
                nd++;
            }
        }
    }
    {
        int sci = spec == 'e' || (spec == 'g' && (exp < -4 || exp >= prec));
        int frac = sci ? (spec == 'g' ? prec - 1 : prec) : prec;
        int before = exp + 1;
        int total, padn;
        if (sci) {
            int body = 1 + ((frac > 0 || alt) ? 1 + frac : 0);
            if (spec == 'g' && !alt) {
                int last = 1 + frac;
                while (last > 1 && (last - 1 >= nd || num[last - 1] == '0')) {
                    if (last - 1 < nd && num[last - 1] != '0') break;
                    if (last - 1 >= nd || num[last - 1] == '0') last--;
                    else break;
                }
                /* last is the count of significant characters before the exponent, without the dot if it would be last */
                frac = last - 1;
                if (frac < 0) frac = 0;
                body = 1 + (frac > 0 ? 1 + frac : 0);
            }
            total = (sch ? 1 : 0) + body + 2 + (exp < 0 || exp >= 100 ? 3 : 2);
            if (exp <= -100 || exp >= 100) total = (sch ? 1 : 0) + body + 2 + 3;
            else if (exp <= -10 || exp >= 10) total = (sch ? 1 : 0) + body + 2 + 2;
            else total = (sch ? 1 : 0) + body + 2 + 2; /* exponent is at least two digits */
        } else {
            int digits = before > 0 ? before : 1;
            total = (sch ? 1 : 0) + digits + ((frac > 0 || alt) ? 1 + frac : 0);
        }
        padn = width > total ? width - total : 0;
        if (!left && pad == ' ') for (; padn; padn--) emit(o, ' ');
        if (sch) emit(o, sch);
        if (!left && pad == '0') for (; padn; padn--) emit(o, '0');
        if (sci) {
            int en = exp, ei = 0, show;
            char eb[8];
            emit(o, nd > 0 ? num[0] : '0');
            show = frac;
            if (spec == 'g' && !alt) {
                while (show > 0 && (show >= nd || num[show] == '0')) show--;
            }
            if (show > 0 || alt) {
                emit(o, '.');
                for (i = 0; i < show; i++) emit(o, i + 1 < nd ? num[i + 1] : '0');
                if (alt) for (; i < frac; i++) emit(o, '0');
            }
            emit(o, upper ? 'E' : 'e');
            emit(o, en < 0 ? '-' : '+');
            if (en < 0) en = -en;
            if (!en) eb[ei++] = '0';
            while (en) { eb[ei++] = (char)('0' + en % 10); en /= 10; }
            if (ei < 2) eb[ei++] = '0';
            while (ei) emit(o, eb[--ei]);
        } else if (before <= 0) {
            emit(o, '0');
            if (frac > 0 || alt) {
                int z = -before, k;
                emit(o, '.');
                if (spec == 'g' && !alt) {
                    int last = frac;
                    while (last > 0) {
                        int idx = last - 1 + before;
                        char c = (idx >= 0 && idx < nd) ? num[idx] : '0';
                        if (c != '0') break;
                        last--;
                    }
                    frac = last;
                }
                for (k = 0; k < z && k < frac; k++) emit(o, '0');
                for (; k < frac; k++) {
                    int idx = k + before;
                    emit(o, (idx >= 0 && idx < nd) ? num[idx] : '0');
                }
            }
        } else {
            int cut = before + frac;
            if (spec == 'g' && !alt) {
                while (cut > before && (cut - 1 >= nd || num[cut - 1] == '0')) {
                    if (cut - 1 < nd && num[cut - 1] != '0') break;
                    cut--;
                }
                if (cut == before) frac = 0;
                else frac = cut - before;
            }
            for (i = 0; i < before; i++) emit(o, i < nd ? num[i] : '0');
            if (frac > 0 || alt) {
                emit(o, '.');
                for (i = 0; i < frac; i++) emit(o, before + i < nd ? num[before + i] : '0');
            }
        }
        if (left) for (; padn; padn--) emit(o, ' ');
    }
}

/* %d %i %u %x %X %c %s %p %lld %f %e %g %%; флаги '-'+' '#' и '0', ширина, точность */
int vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap) {
    /* «o += snprintf(buf + o, max - o, ...)» после переполнения даёт отрицательный размер,
     * который превращается в огромный size_t: считаем его нулём, а не «писать сколько угодно» */
    if ((int)cap < 0) cap = 0;
    struct out o = { buf, cap, 0 };
    for (; *fmt; fmt++) {
        if (*fmt != '%') { emit(&o, *fmt); continue; }
        fmt++;
        bool left = false, alt = false, plus = false, blank = false;
        char pad = ' ';
        int width = 0, prec = -1;
        for (;; fmt++) {
            if (*fmt == '-') left = true;
            else if (*fmt == '+') plus = true;
            else if (*fmt == ' ') blank = true;
            else if (*fmt == '#') alt = true;
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
                emit_u64(&o, u, 10, false, sv < 0, width, pad, left, plus, blank, prec, alt);
            } else {
                int v = va_arg(ap, int);
                emit_num(&o, v < 0 ? -(unsigned)v : (unsigned)v, 10, false, v < 0, width, pad, left, plus, blank, prec, alt);
            }
            break;
        }
        case 'u':
            if (lng >= 2) emit_u64(&o, va_arg(ap, unsigned long long), 10, false, false, width, pad, left, false, false, prec, alt);
            else emit_num(&o, va_arg(ap, unsigned), 10, false, false, width, pad, left, false, false, prec, alt);
            break;
        case 'o':
            if (lng >= 2) emit_u64(&o, va_arg(ap, unsigned long long), 8, false, false, width, pad, left, false, false, prec, alt);
            else emit_num(&o, va_arg(ap, unsigned), 8, false, false, width, pad, left, false, false, prec, alt);
            break;
        case 'x':
            if (lng >= 2) emit_u64(&o, va_arg(ap, unsigned long long), 16, false, false, width, pad, left, false, false, prec, alt);
            else emit_num(&o, va_arg(ap, unsigned), 16, false, false, width, pad, left, false, false, prec, alt);
            break;
        case 'X':
            if (lng >= 2) emit_u64(&o, va_arg(ap, unsigned long long), 16, true, false, width, pad, left, false, false, prec, alt);
            else emit_num(&o, va_arg(ap, unsigned), 16, true, false, width, pad, left, false, false, prec, alt);
            break;
        case 'p': {
            unsigned v = va_arg(ap, unsigned);
            char tmp[16];
            int n = 0, k;
            tmp[n++] = '0';
            tmp[n++] = 'x';
            for (k = 28; k >= 0; k -= 4) {
                int d = (v >> k) & 0xf;
                tmp[n++] = (char)(d < 10 ? '0' + d : 'a' + d);
            }
            if (width < n) width = n;
            if (!left) while (width > n) { emit(&o, ' '); width--; }
            for (k = 0; k < n; k++) emit(&o, tmp[k]);
            if (left) while (width > n) { emit(&o, ' '); width--; }
            break;
        }
        case 'f': case 'e': case 'E': case 'g': case 'G':
            emit_float(&o, va_arg(ap, double), *fmt, prec, width, pad, left, alt, plus, blank);
            break;
        case 'c': {
            char ch = (char)va_arg(ap, int);
            int k;
            if (!left) for (k = 1; k < width; k++) emit(&o, ' ');
            emit(&o, ch);
            if (left) for (k = 1; k < width; k++) emit(&o, ' ');
            break;
        }
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

static void *malloc_unlocked(size_t n) {
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

static void free_unlocked(void *p) {
    if (!p) return;
    struct hdr *h = (struct hdr *)p - 1;
    if (h->magic != HEAP_MAGIC) return;             /* не наш указатель — не трогаем */
    size_t sz;
    int k = size_class(h->size, &sz);
    h->next = free_list[k];
    free_list[k] = h;
}

static thread_mutex_t heap_lock;
void *malloc(size_t n) {
    thread_mutex_lock(&heap_lock);
    void *p=malloc_unlocked(n);
    thread_mutex_unlock(&heap_lock);
    return p;
}
void free(void *p) {
    if (!p) return;
    thread_mutex_lock(&heap_lock);
    free_unlocked(p);
    thread_mutex_unlock(&heap_lock);
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
int sysinfo(struct system_info *out, unsigned size) { return syscall3(SYS_SYSINFO, (int)out, size, 0); }
int cpuinfo(struct cpu_info *out, unsigned capacity) { return syscall3(SYS_CPUINFO, (int)out, capacity, 0); }
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
int unitctl(unsigned op, const char *name, struct unit_info *out, unsigned index) {
    return syscall3(SYS_UNITCTL, op, op == UNIT_LIST ? (int)index : (int)name, (int)out);
}

int service_register(const char *name)          { return syscall3(SYS_SERVICE, 1, (int)name, 0); }
int service_find(const char *name)              { return syscall3(SYS_SERVICE, 2, (int)name, 0); }
int get_time(struct datetime *t)                { return syscall3(SYS_TIME, (int)t, 0, 0); }



int account(int op,const char *name,const char *password) { return syscall3(SYS_ACCOUNT,op,(int)name,(int)password); }
int account_list(char *name,unsigned index) { return syscall3(SYS_ACCOUNT,7,(int)name,index); }

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

/* Новые gcc зовут эту функцию даже для одного деления signed long long. */
long long __divmoddi4(long long a, long long b, long long *rp) {
    di_parts A, B, Q, R;
    int qneg = 0, rneg = 0;
    A.u = (unsigned long long)a; B.u = (unsigned long long)b;
    if (A.p.hi & 0x80000000u) { qneg ^= 1; rneg = 1; A.u = di_neg(A.u); }
    if (B.p.hi & 0x80000000u) { qneg ^= 1; B.u = di_neg(B.u); }
    Q.u = __udivdi3(A.u, B.u);
    R.u = A.u - Q.u * B.u;
    if (qneg) Q.u = di_neg(Q.u);
    if (rneg) R.u = di_neg(R.u);
    if (rp) *rp = (long long)R.u;
    return (long long)Q.u;
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

FILE *freopen(const char *path, const char *mode, FILE *f) {
    int acc, flags, fd;
    if (!f) return NULL;
    if (f->fd >= 0) close(f->fd);
    f->fd = -1;
    f->flags = 0;
    if (!path) { errno = EINVAL; return NULL; }
    flags = mode_flags(mode, &acc);
    if (flags < 0) { errno = EINVAL; return NULL; }
    fd = open(path, flags);
    if (fd < 0) return NULL;
    f->fd = fd;
    f->flags = acc;
    return f;
}

FILE *tmpfile(void) {
    char name[L_tmpnam];
    FILE *f;
    if (!tmpnam(name)) return NULL;
    f = fopen(name, "w+");
    return f;
}

char *tmpnam(char *s) {
    static char buf[L_tmpnam];
    static unsigned n;
    char *p = s ? s : buf;
    snprintf(p, L_tmpnam, "/tmp/myos-%u-%u", (unsigned)getpid(), ++n);
    return p;
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

static mode_t current_umask = 022;
mode_t umask(mode_t mask) {
    mode_t old = current_umask;
    current_umask = mask & 0777;
    return old;
}
int chmod(const char *path, mode_t mode) { (void)path; (void)mode; return 0; }
int fchmod(int fd, mode_t mode) { (void)fd; (void)mode; return 0; }

int fstat(int fd, struct stat *st) {
    int r;
    memset(st, 0, sizeof *st);
    r = ioerr(syscall3(SYS_FSTAT, fd, (int)st, 0));
    if (r == 0) {
        st->st_dev = 1;
        st->st_blocks = (st->st_size + 511) / 512;
    }
    return r;
}

char *ttyname(int fd) { (void)fd; return (char *)"/dev/tty"; }

/* Ядро не хранит флаги дескриптора. F_DUPFD ищет свободный номер и делает dup2.
 * F_GETFD/F_SETFD пока ничего не запоминают: close-on-exec нет. */
int fcntl(int fd, int cmd, ...) {
    struct stat st;
    if (cmd == F_GETFD || cmd == F_GETFL || cmd == F_SETFD || cmd == F_SETFL) {
        if (fstat(fd, &st) < 0) return -1;
        return 0;
    }
    if (cmd == F_DUPFD) {
        va_list ap;
        int i, min;
        va_start(ap, cmd);
        min = va_arg(ap, int);
        va_end(ap);
        if (min < 0) { errno = EINVAL; return -1; }
        if (fstat(fd, &st) < 0) return -1;
        for (i = min; i < 16; i++) {
            if (fstat(i, &st) == 0) continue;
            if (errno != EBADF) return -1;
            return ioerr(syscall3(SYS_DUP2, fd, i, 0));
        }
        errno = EMFILE;
        return -1;
    }
    errno = EINVAL;
    return -1;
}

static unsigned path_stamp(const char *path) {
    unsigned h = 2166136261u;
    if (!path) return 1;
    for (const char *p = path; *p; p++) {
        h ^= (unsigned char)*p;
        h *= 16777619u;
    }
    return h ? h : 1;
}

int stat(const char *path, struct stat *st) {
    struct dirent_info info;
    int r = syscall3(SYS_STAT, (int)path, (int)&info, 0);
    if (r < 0) { errno = -r; return -1; }
    memset(st, 0, sizeof *st);
    if (info.type == 2) st->st_mode = S_IFDIR;
    else if (info.type == 4) st->st_mode = S_IFCHR;
    else if (info.type == 3) st->st_mode = S_IFBLK;
    else if (info.type == 5) st->st_mode = S_IFLNK;
    else st->st_mode = S_IFREG;
    st->st_size = info.size;
    st->st_dev = 1;
    st->st_ino = path_stamp(path);
    st->st_blocks = (st->st_size + 511) / 512;
    return 0;
}

int lstat(const char *path, struct stat *st) {
    struct dirent_info info;
    int r = syscall3(SYS_LSTAT, (int)path, (int)&info, 0);
    if (r < 0) { errno = -r; return -1; }
    memset(st, 0, sizeof *st);
    if (info.type == 2) st->st_mode = S_IFDIR;
    else if (info.type == 4) st->st_mode = S_IFCHR;
    else if (info.type == 3) st->st_mode = S_IFBLK;
    else if (info.type == 5) st->st_mode = S_IFLNK;
    else st->st_mode = S_IFREG;
    st->st_size = info.size;
    st->st_dev = 1;
    st->st_ino = path_stamp(path);
    st->st_blocks = (st->st_size + 511) / 512;
    return 0;
}

int symlink(const char *target, const char *linkpath) {
    return ioerr(syscall3(SYS_SYMLINK, (int)target, (int)linkpath, 0));
}

ssize_t readlink(const char *path, char *buf, size_t bufsiz) {
    int r = syscall3(SYS_READLINK, (int)path, (int)buf, (int)bufsiz);
    if (r < 0) { errno = -r; return -1; }
    return r;
}

int rename(const char *old, const char *new) {
    return ioerr(syscall3(SYS_RENAME, (int)old, (int)new, 0));
}

int remove(const char *path) {
    return ioerr(syscall3(SYS_UNLINK, (int)path, 0, 0));
}

struct DIR {
    char path[1024];
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
    if (n == 0 || n >= 1024) { errno = n ? ENAMETOOLONG : ENOENT; return NULL; }
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
    else if (info.type == 5) dir->ent.d_type = DT_LNK;
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
char *setlocale(int category, const char *locale) {
    (void)category;
    if (locale && locale[0] && strcmp(locale, "C") && strcmp(locale, "POSIX")) return NULL;
    return (char *)"C";
}

struct lconv *localeconv(void) {
    static struct lconv lc;
    static int ready;
    if (!ready) {
        memset(&lc, 0, sizeof lc);
        lc.decimal_point = ".";
        lc.thousands_sep = "";
        lc.grouping = "";
        lc.int_curr_symbol = "";
        lc.currency_symbol = "";
        lc.mon_decimal_point = "";
        lc.mon_thousands_sep = "";
        lc.mon_grouping = "";
        lc.positive_sign = "";
        lc.negative_sign = "";
        lc.int_frac_digits = lc.frac_digits = 127;
        lc.p_cs_precedes = lc.p_sep_by_space = lc.n_cs_precedes = lc.n_sep_by_space = 127;
        lc.p_sign_posn = lc.n_sign_posn = 127;
        ready = 1;
    }
    return &lc;
}
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

static void big_add_small(Big *b, unsigned a) {
    unsigned c = a;
    int i;
    for (i = 0; i < b->n; i++) {
        unsigned t = b->v[i] + c;
        c = t < b->v[i];
        b->v[i] = t;
        if (!c) return;
    }
    if (c && b->n < BIG_N) b->v[b->n++] = c;
}

/* Ближайший double к целому * 2^extra. extra < 0 — это сдвиг вправо всего числа. */
static double big_to_double(Big *b, int sign, int extra) {
    int bits, i, be, roundb, sticky;
    unsigned long long mant, u;
    double d;
    bits = b->n * 32;
    while (bits > 0 && !big_bit(b, bits - 1)) bits--;
    if (bits <= 0) return copysign(0.0, sign < 0 ? -1.0 : 1.0);
    roundb = 0;
    sticky = 0;
    if (bits > 53) {
        roundb = big_bit(b, bits - 54);
        for (i = 0; i < bits - 54; i++) if (big_bit(b, i)) { sticky = 1; break; }
        mant = 0;
        for (i = 0; i < 53; i++) mant = (mant << 1) | (unsigned)big_bit(b, bits - 1 - i);
        if (roundb && (sticky || (mant & 1))) {
            mant++;
            if (mant == (1ULL << 53)) { mant >>= 1; bits++; }
        }
    } else {
        mant = 0;
        for (i = 0; i < bits; i++) mant = (mant << 1) | (unsigned)big_bit(b, bits - 1 - i);
    }
    be = bits - 1 + extra;
    if (be > 1023) { errno = ERANGE; return copysign(__myos_inf(), sign < 0 ? -1.0 : 1.0); }
    if (be < -1022) { errno = ERANGE; return copysign(0.0, sign < 0 ? -1.0 : 1.0); }
    u = ((unsigned long long)(be + 1023) << 52) | (mant & ((1ULL << 52) - 1));
    if (sign < 0) u |= 1ULL << 63;
    memcpy(&d, &u, 8);
    return d;
}

static double strtod_precise(const char *nptr, char **end) {
    const char *s = nptr;
    char digs[400];
    int sign = 1, nd = 0, exp = 0, sticky = 0, esign, ev, i, k;
    Big b;
    while (isspace((unsigned char)*s)) s++;
    if (*s == '+' || *s == '-') { if (*s == '-') sign = -1; s++; }
    while (*s == '0') s++;
    while (isdigit((unsigned char)*s)) {
        if (nd < (int)sizeof digs) digs[nd++] = (char)(*s - '0');
        else { exp++; if (*s != '0') sticky = 1; }
        s++;
    }
    if (*s == '.') {
        s++;
        if (!nd) while (*s == '0') { exp--; s++; }
        while (isdigit((unsigned char)*s)) {
            if (nd < (int)sizeof digs) { digs[nd++] = (char)(*s - '0'); exp--; }
            else if (*s != '0') sticky = 1;
            s++;
        }
    }
    if (*s == 'e' || *s == 'E') {
        const char *e0 = s++;
        esign = 1;
        ev = 0;
        if (*s == '+' || *s == '-') { if (*s == '-') esign = -1; s++; }
        if (!isdigit((unsigned char)*s)) s = e0;
        else {
            while (isdigit((unsigned char)*s)) {
                if (ev < 100000) ev = ev * 10 + (*s - '0');
                s++;
            }
            exp += esign * ev;
        }
    }
    if (end) *end = (char *)s;
    while (nd > 0 && digs[nd - 1] == 0) { nd--; exp++; }
    if (!nd) return copysign(0.0, sign < 0 ? -1.0 : 1.0);
    big_set(&b, 0);
    for (i = 0; i < nd; i++) {
        big_mul(&b, 10);
        big_add_small(&b, (unsigned)digs[i]);
    }
    if (sticky) big_add_small(&b, 1);
    if (exp >= 0) {
        for (k = 0; k < exp; k++) big_mul(&b, 5);
        return big_to_double(&b, sign, exp);
    }
    k = -exp;
    for (i = 0; i < k; i++) big_mul(&b, 5);
    return big_to_double(&b, sign, -k);
}

double strtod(const char *nptr, char **end) {
    const char *s = nptr;
    int sign = 1, exp = 0, nd = 0, saw = 0, esign, ev, trunc = 0;
    unsigned long long acc = 0;
    double x;
    while (isspace((unsigned char)*s)) s++;
    if (*s == '+' || *s == '-') { if (*s == '-') sign = -1; s++; }
    if ((s[0] == 'i' || s[0] == 'I') && (s[1] == 'n' || s[1] == 'N') && (s[2] == 'f' || s[2] == 'F')) {
        s += 3;
        if ((s[0] == 'i' || s[0] == 'I') && (s[1] == 'n' || s[1] == 'N') && (s[2] == 'i' || s[2] == 'I') &&
            (s[3] == 't' || s[3] == 'T') && (s[4] == 'y' || s[4] == 'Y')) s += 5;
        if (end) *end = (char *)s;
        return copysign(__myos_inf(), sign < 0 ? -1.0 : 1.0);
    }
    if ((s[0] == 'n' || s[0] == 'N') && (s[1] == 'a' || s[1] == 'A') && (s[2] == 'n' || s[2] == 'N')) {
        s += 3;
        if (end) *end = (char *)s;
        return __myos_nan();
    }
    while (isdigit((unsigned char)*s)) {
        saw = 1;
        if (nd < 15) { acc = acc * 10 + (unsigned)(*s - '0'); nd++; }
        else { exp++; trunc = 1; }
        s++;
    }
    if (*s == '.') {
        s++;
        while (isdigit((unsigned char)*s)) {
            saw = 1;
            if (nd < 15) { acc = acc * 10 + (unsigned)(*s - '0'); nd++; exp--; }
            else trunc = 1;
            s++;
        }
    }
    if (*s == 'e' || *s == 'E') {
        const char *e0 = s++;
        esign = 1;
        ev = 0;
        if (*s == '+' || *s == '-') { if (*s == '-') esign = -1; s++; }
        if (!isdigit((unsigned char)*s)) s = e0;
        else {
            while (isdigit((unsigned char)*s)) {
                if (ev < 100000) ev = ev * 10 + (*s - '0');
                s++;
            }
            exp += esign * ev;
        }
    }
    if (!saw) { if (end) *end = (char *)nptr; return 0; }
    if (trunc) return strtod_precise(nptr, end);
    if (end) *end = (char *)s;
    if (exp > 350) { errno = ERANGE; return copysign(__myos_inf(), sign < 0 ? -1.0 : 1.0); }
    if (exp < -350) { errno = ERANGE; return copysign(0.0, sign < 0 ? -1.0 : 1.0); }
    x = (double)acc;
    if (exp > 0) x *= dpow10(exp);
    else if (exp < 0) x /= dpow10(-exp);
    return sign < 0 ? -x : x;
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
gid_t getegid(void) { return 0; }
unsigned alarm(unsigned seconds) { (void)seconds; return 0; }
int setuid(uid_t uid) { (void)uid; return 0; }
int setgid(gid_t gid) { (void)gid; return 0; }

static struct passwd only_user = { "root", "", 0, 0, "root", "/root", "/bin/sh" };
struct passwd *getpwuid(uid_t uid) { (void)uid; return &only_user; }
struct passwd *getpwnam(const char *name) { (void)name; return &only_user; }
void endpwent(void) {}

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

double difftime(time_t a, time_t b) { return (double)(a - b); }

clock_t clock(void) {
#if defined(__LINUX_TEST__)
    struct { long sec, nsec; } ts;
    if (__syscall(265, 1, (int)&ts) < 0) return (clock_t)-1;
    return (clock_t)(ts.sec * 1000 + ts.nsec / 1000000);
#else
    return (clock_t)ticks();
#endif
}

static long long days_from_civil(int y, unsigned m, unsigned d) {
    long long era, yoe, doy, doe;
    y -= m <= 2;
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = (unsigned)(y - era * 400);
    doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static void civil_from_days(long long z, int *y, unsigned *m, unsigned *d) {
    long long era;
    unsigned doe, yoe, doy, mp;
    z += 719468;
    era = (z >= 0 ? z : z - 146096) / 146097;
    doe = (unsigned)(z - era * 146097);
    yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = (int)yoe + (int)era * 400 + (*m <= 2);
}

static int yday_of(int y, unsigned m, unsigned d) {
    static const unsigned md[12] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
    int n = (int)md[m - 1] + (int)d - 1;
    if (m > 2 && leap((unsigned)y)) n++;
    return n;
}

struct tm *gmtime_r(const time_t *tp, struct tm *out) {
    long long secs, days;
    int rem, y;
    unsigned m, d;
    if (!tp || !out) return NULL;
    secs = *tp;
    days = secs / 86400;
    rem = (int)(secs % 86400);
    if (rem < 0) { rem += 86400; days--; }
    out->tm_hour = rem / 3600;
    rem %= 3600;
    out->tm_min = rem / 60;
    out->tm_sec = rem % 60;
    civil_from_days(days, &y, &m, &d);
    out->tm_year = y - 1900;
    out->tm_mon = (int)m - 1;
    out->tm_mday = (int)d;
    out->tm_yday = yday_of(y, m, d);
    out->tm_wday = (int)((days % 7 + 7 + 4) % 7);
    out->tm_isdst = 0;
    return out;
}

struct tm *localtime_r(const time_t *tp, struct tm *out) { return gmtime_r(tp, out); }

struct tm *gmtime(const time_t *tp) {
    static struct tm buf;
    return gmtime_r(tp, &buf);
}

struct tm *localtime(const time_t *tp) { return gmtime(tp); }

static int norm_field(int *field, int *carry, int base) {
    int v = *field;
    if (v >= base || v < 0) {
        int q = v / base;
        int r = v % base;
        if (r < 0) { r += base; q--; }
        *field = r;
        *carry += q;
    }
    return 0;
}

time_t mktime(struct tm *tm) {
    int sec, min, hour, day, mon, year, mdays;
    long long days, secs;
    static const int mlen[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (!tm) { errno = EINVAL; return (time_t)-1; }
    sec = tm->tm_sec;
    min = tm->tm_min;
    hour = tm->tm_hour;
    day = tm->tm_mday;
    mon = tm->tm_mon;
    year = tm->tm_year + 1900;
    norm_field(&sec, &min, 60);
    norm_field(&min, &hour, 60);
    norm_field(&hour, &day, 24);
    mon += 1;
    while (mon > 12) { mon -= 12; year++; }
    while (mon < 1) { mon += 12; year--; }
    for (;;) {
        mdays = mlen[mon - 1] + (mon == 2 && leap((unsigned)year));
        if (day > mdays) { day -= mdays; mon++; if (mon > 12) { mon = 1; year++; } }
        else if (day < 1) {
            mon--;
            if (mon < 1) { mon = 12; year--; }
            day += mlen[mon - 1] + (mon == 2 && leap((unsigned)year));
        } else break;
        if (year < 1 || year > 9999) { errno = ERANGE; return (time_t)-1; }
    }
    days = days_from_civil(year, (unsigned)mon, (unsigned)day);
    secs = days * 86400LL + hour * 3600LL + min * 60LL + sec;
    if (secs < -2147483647LL - 1 || secs > 2147483647LL) { errno = ERANGE; return (time_t)-1; }
    tm->tm_sec = sec;
    tm->tm_min = min;
    tm->tm_hour = hour;
    tm->tm_mday = day;
    tm->tm_mon = mon - 1;
    tm->tm_year = year - 1900;
    tm->tm_yday = yday_of(year, (unsigned)mon, (unsigned)day);
    tm->tm_wday = (int)((days % 7 + 7 + 4) % 7);
    tm->tm_isdst = 0;
    return (time_t)secs;
}

static int fmt_put(char *s, int n, int max, const char *t) {
    while (*t) {
        if (n + 1 >= max) return -1;
        s[n++] = *t++;
    }
    return n;
}

static int fmt_num(char *s, int n, int max, int width, int v) {
    char tmp[16];
    int i = 0, neg = v < 0;
    if (neg) v = -v;
    if (!v) tmp[i++] = '0';
    while (v) { tmp[i++] = (char)('0' + v % 10); v /= 10; }
    if (neg) { if (n + 1 >= max) return -1; s[n++] = '-'; }
    while (width > i) { if (n + 1 >= max) return -1; s[n++] = '0'; width--; }
    while (i) { if (n + 1 >= max) return -1; s[n++] = tmp[--i]; }
    return n;
}

size_t strftime(char *s, size_t max, const char *fmt, const struct tm *tm) {
    static const char *wshort[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    static const char *wlong[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
    static const char *mshort[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    static const char *mlong[] = { "January", "February", "March", "April", "May", "June", "July",
        "August", "September", "October", "November", "December" };
    int n = 0, w, mon, y;
    char piece[64];
    if (!s || !fmt || !tm || max == 0) return 0;
    w = tm->tm_wday;
    mon = tm->tm_mon;
    y = tm->tm_year + 1900;
    if (w < 0 || w > 6) w = 0;
    if (mon < 0 || mon > 11) mon = 0;
    for (; *fmt; fmt++) {
        const char *txt = NULL;
        if (*fmt != '%') {
            if (n + 1 >= (int)max) return 0;
            s[n++] = *fmt;
            continue;
        }
        fmt++;
        if (*fmt == 'E' || *fmt == 'O') fmt++;
        if (!*fmt) break;
        switch (*fmt) {
        case 'a': txt = wshort[w]; break;
        case 'A': txt = wlong[w]; break;
        case 'b': case 'h': txt = mshort[mon]; break;
        case 'B': txt = mlong[mon]; break;
        case 'c':
            snprintf(piece, sizeof piece, "%s %s %2d %02d:%02d:%02d %04d",
                     wshort[w], mshort[mon], tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec, y);
            txt = piece;
            break;
        case 'C': n = fmt_num(s, n, (int)max, 2, y / 100); if (n < 0) return 0; continue;
        case 'd': n = fmt_num(s, n, (int)max, 2, tm->tm_mday); if (n < 0) return 0; continue;
        case 'D':
            snprintf(piece, sizeof piece, "%02d/%02d/%02d", mon + 1, tm->tm_mday, y % 100);
            txt = piece;
            break;
        case 'e':
            snprintf(piece, sizeof piece, "%2d", tm->tm_mday);
            txt = piece;
            break;
        case 'F':
            snprintf(piece, sizeof piece, "%04d-%02d-%02d", y, mon + 1, tm->tm_mday);
            txt = piece;
            break;
        case 'H': n = fmt_num(s, n, (int)max, 2, tm->tm_hour); if (n < 0) return 0; continue;
        case 'I': {
            int h = tm->tm_hour % 12;
            if (!h) h = 12;
            n = fmt_num(s, n, (int)max, 2, h);
            if (n < 0) return 0;
            continue;
        }
        case 'j': n = fmt_num(s, n, (int)max, 3, tm->tm_yday + 1); if (n < 0) return 0; continue;
        case 'm': n = fmt_num(s, n, (int)max, 2, mon + 1); if (n < 0) return 0; continue;
        case 'M': n = fmt_num(s, n, (int)max, 2, tm->tm_min); if (n < 0) return 0; continue;
        case 'n': txt = "\n"; break;
        case 'p': txt = tm->tm_hour < 12 ? "AM" : "PM"; break;
        case 'R':
            snprintf(piece, sizeof piece, "%02d:%02d", tm->tm_hour, tm->tm_min);
            txt = piece;
            break;
        case 'S': n = fmt_num(s, n, (int)max, 2, tm->tm_sec); if (n < 0) return 0; continue;
        case 't': txt = "\t"; break;
        case 'T': case 'X':
            snprintf(piece, sizeof piece, "%02d:%02d:%02d", tm->tm_hour, tm->tm_min, tm->tm_sec);
            txt = piece;
            break;
        case 'u': n = fmt_num(s, n, (int)max, 1, w == 0 ? 7 : w); if (n < 0) return 0; continue;
        case 'U': n = fmt_num(s, n, (int)max, 2, (tm->tm_yday + 7 - w) / 7); if (n < 0) return 0; continue;
        case 'w': n = fmt_num(s, n, (int)max, 1, w); if (n < 0) return 0; continue;
        case 'W': n = fmt_num(s, n, (int)max, 2, (tm->tm_yday + 7 - (w == 0 ? 6 : w - 1)) / 7); if (n < 0) return 0; continue;
        case 'x':
            snprintf(piece, sizeof piece, "%02d/%02d/%02d", mon + 1, tm->tm_mday, y % 100);
            txt = piece;
            break;
        case 'y': n = fmt_num(s, n, (int)max, 2, y % 100); if (n < 0) return 0; continue;
        case 'Y': n = fmt_num(s, n, (int)max, 4, y); if (n < 0) return 0; continue;
        case 'z': txt = "+0000"; break;
        case 'Z': txt = "UTC"; break;
        case '%': txt = "%"; break;
        default: txt = ""; break;
        }
        if (txt) {
            n = fmt_put(s, n, (int)max, txt);
            if (n < 0) return 0;
        }
    }
    if (n >= (int)max) return 0;
    s[n] = 0;
    return (size_t)n;
}

static void (*sig_handlers[32])(int);

int sigblock(int mask) { (void)mask; return 0; }
int sigsetmask(int mask) { (void)mask; return 0; }
int siginterrupt(int sig, int flag) { (void)sig; (void)flag; return 0; }

void (*signal(int sig, void (*handler)(int)))(int) {
    void (*old)(int);
    if (sig < 0 || sig >= 32) { errno = EINVAL; return SIG_ERR; }
    old = sig_handlers[sig];
    sig_handlers[sig] = handler;
    return old;
}

int system(const char *cmd) {
    int pid, st = 0;
    char *av[4];
    if (!cmd) return 1;
    if (strlen(cmd) >= 256) { errno = EINVAL; return -1; }
    pid = fork();
    if (pid < 0) { errno = -pid; return -1; }
    if (pid == 0) {
        av[0] = "sh";
        av[1] = "-c";
        av[2] = (char *)cmd;
        av[3] = NULL;
        exec("/bin/sh", av);
        exit(127);
    }
    if (waitpid(pid, &st, 0) < 0) return -1;
    errno = 0;
    return st;
}

#include "math.c"
#include "regex.c"
