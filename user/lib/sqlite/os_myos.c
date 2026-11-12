/* Файлы SQLite поверх open/read/write/lseek.
 * Блокировки живут в этом процессе: ядро не умеет F_SETLK.
 * Смещение 32-битное, файл длиннее 2 ГиБ не пишем.
 * Усечь можно только до нуля: повторное открытие с O_TRUNC. */
#include <sqlite3.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#define MYOS_FILE_MAX 0x7fffffff

typedef struct Node Node;
typedef struct MyFile MyFile;

struct Node {
    char path[512];
    int shared;
    int refs;
    MyFile *reserved;
    MyFile *pending;
    MyFile *excl;
    Node *next;
};

struct MyFile {
    sqlite3_file base;
    int fd;
    int level;
    int del;
    Node *node;
    char path[512];
};

static Node *nodes;

static int fit(sqlite3_int64 off, int len) {
    if (off < 0 || len < 0) return 0;
    if (off > MYOS_FILE_MAX) return 0;
    if ((sqlite3_int64)len > MYOS_FILE_MAX - off) return 0;
    return 1;
}

static Node *node_get(const char *path) {
    Node *n;
    for (n = nodes; n; n = n->next)
        if (strcmp(n->path, path) == 0) { n->refs++; return n; }
    n = malloc(sizeof *n);
    if (!n) return 0;
    memset(n, 0, sizeof *n);
    snprintf(n->path, sizeof n->path, "%s", path);
    n->refs = 1;
    n->next = nodes;
    nodes = n;
    return n;
}

static void node_put(Node *n) {
    Node **pp;
    if (!n) return;
    if (--n->refs) return;
    for (pp = &nodes; *pp; pp = &(*pp)->next)
        if (*pp == n) { *pp = n->next; break; }
    free(n);
}

static int myClose(sqlite3_file *pf) {
    MyFile *p = (MyFile *)pf;
    if (p->fd >= 0) close(p->fd);
    p->fd = -1;
    if (p->node) {
        if (p->node->excl == p) p->node->excl = 0;
        if (p->node->pending == p) p->node->pending = 0;
        if (p->node->reserved == p) p->node->reserved = 0;
        if (p->level >= SQLITE_LOCK_SHARED && p->node->shared) p->node->shared--;
        p->level = SQLITE_LOCK_NONE;
    }
    if (p->del && p->path[0]) unlink(p->path);
    node_put(p->node);
    p->node = 0;
    return SQLITE_OK;
}

static int myRead(sqlite3_file *pf, void *buf, int amt, sqlite3_int64 off) {
    MyFile *p = (MyFile *)pf;
    int got = 0;
    if (!fit(off, amt)) return SQLITE_IOERR_READ;
    if (lseek(p->fd, (long)off, SEEK_SET) < 0) return SQLITE_IOERR_READ;
    while (got < amt) {
        int n = read(p->fd, (char *)buf + got, (size_t)(amt - got));
        if (n < 0) return SQLITE_IOERR_READ;
        if (n == 0) {
            memset((char *)buf + got, 0, (size_t)(amt - got));
            return SQLITE_IOERR_SHORT_READ;
        }
        got += n;
    }
    return SQLITE_OK;
}

static int myWrite(sqlite3_file *pf, const void *buf, int amt, sqlite3_int64 off) {
    MyFile *p = (MyFile *)pf;
    int put = 0;
    if (!fit(off, amt)) return SQLITE_FULL;
    if (lseek(p->fd, (long)off, SEEK_SET) < 0) return SQLITE_IOERR_WRITE;
    while (put < amt) {
        int n = write(p->fd, (const char *)buf + put, (size_t)(amt - put));
        if (n < 0) return SQLITE_IOERR_WRITE;
        if (n == 0) return SQLITE_IOERR_WRITE;
        put += n;
    }
    return SQLITE_OK;
}

static int myTruncate(sqlite3_file *pf, sqlite3_int64 size) {
    MyFile *p = (MyFile *)pf;
    struct stat st;
    int t;
    if (size < 0 || size > MYOS_FILE_MAX) return SQLITE_IOERR_TRUNCATE;
    if (fstat(p->fd, &st) < 0) return SQLITE_IOERR_TRUNCATE;
    if ((sqlite3_int64)st.st_size == size) return SQLITE_OK;
    if (size != 0 || !p->path[0]) return SQLITE_IOERR_TRUNCATE;
    t = open(p->path, O_WRONLY | O_TRUNC, 0666);
    if (t < 0) return SQLITE_IOERR_TRUNCATE;
    close(t);
    return SQLITE_OK;
}

static int mySync(sqlite3_file *pf, int flags) {
    (void)pf;
    (void)flags;
    return SQLITE_OK;
}

static int myFileSize(sqlite3_file *pf, sqlite3_int64 *out) {
    struct stat st;
    if (fstat(((MyFile *)pf)->fd, &st) < 0) return SQLITE_IOERR;
    *out = st.st_size;
    return SQLITE_OK;
}

static int myLock(sqlite3_file *pf, int level) {
    MyFile *p = (MyFile *)pf;
    Node *n = p->node;
    if (!n || level <= p->level) return SQLITE_OK;
    if (level >= SQLITE_LOCK_SHARED && p->level < SQLITE_LOCK_SHARED) {
        if ((n->excl && n->excl != p) || (n->pending && n->pending != p)) return SQLITE_BUSY;
    }
    if (level >= SQLITE_LOCK_RESERVED && p->level < SQLITE_LOCK_RESERVED) {
        if (n->reserved && n->reserved != p) return SQLITE_BUSY;
    }
    if (level >= SQLITE_LOCK_EXCLUSIVE && p->level < SQLITE_LOCK_EXCLUSIVE) {
        int others = n->shared - (p->level >= SQLITE_LOCK_SHARED ? 1 : 0);
        if (others > 0) return SQLITE_BUSY;
    }
    if (p->level < SQLITE_LOCK_SHARED && level >= SQLITE_LOCK_SHARED) n->shared++;
    if (p->level < SQLITE_LOCK_RESERVED && level >= SQLITE_LOCK_RESERVED) n->reserved = p;
    if (p->level < SQLITE_LOCK_PENDING && level >= SQLITE_LOCK_PENDING) n->pending = p;
    if (p->level < SQLITE_LOCK_EXCLUSIVE && level >= SQLITE_LOCK_EXCLUSIVE) n->excl = p;
    p->level = level;
    return SQLITE_OK;
}

static int myUnlock(sqlite3_file *pf, int level) {
    MyFile *p = (MyFile *)pf;
    Node *n = p->node;
    if (!n || level >= p->level) return SQLITE_OK;
    if (p->level >= SQLITE_LOCK_EXCLUSIVE && level < SQLITE_LOCK_EXCLUSIVE && n->excl == p) n->excl = 0;
    if (p->level >= SQLITE_LOCK_PENDING && level < SQLITE_LOCK_PENDING && n->pending == p) n->pending = 0;
    if (p->level >= SQLITE_LOCK_RESERVED && level < SQLITE_LOCK_RESERVED && n->reserved == p) n->reserved = 0;
    if (p->level >= SQLITE_LOCK_SHARED && level < SQLITE_LOCK_SHARED && n->shared) n->shared--;
    p->level = level;
    return SQLITE_OK;
}

static int myCheckReserved(sqlite3_file *pf, int *out) {
    MyFile *p = (MyFile *)pf;
    Node *n = p->node;
    *out = n && ((n->reserved && n->reserved != p) || (n->pending && n->pending != p) ||
                 (n->excl && n->excl != p));
    return SQLITE_OK;
}

static int myFileControl(sqlite3_file *pf, int op, void *arg) {
    (void)pf;
    (void)op;
    (void)arg;
    return SQLITE_NOTFOUND;
}

static int mySectorSize(sqlite3_file *pf) {
    (void)pf;
    return 512;
}

static int myDevice(sqlite3_file *pf) {
    (void)pf;
    return 0;
}

static const sqlite3_io_methods methods = {
    1, myClose, myRead, myWrite, myTruncate, mySync, myFileSize,
    myLock, myUnlock, myCheckReserved, myFileControl, mySectorSize, myDevice,
    0, 0, 0, 0, 0, 0
};

static int store_path(char *dst, int cap, const char *name) {
    if (!name || !name[0]) return SQLITE_CANTOPEN;
    if (name[0] == '/') {
        if ((int)strlen(name) >= cap) return SQLITE_CANTOPEN;
        snprintf(dst, (size_t)cap, "%s", name);
        return SQLITE_OK;
    }
    if (!getcwd(dst, (size_t)cap)) return SQLITE_CANTOPEN;
    if ((int)strlen(dst) + 1 + (int)strlen(name) >= cap) return SQLITE_CANTOPEN;
    strcat(dst, "/");
    strcat(dst, name);
    return SQLITE_OK;
}

static int myOpen(sqlite3_vfs *vfs, sqlite3_filename name, sqlite3_file *pf, int flags, int *out) {
    MyFile *p = (MyFile *)pf;
    int of, fd;
    static unsigned seq;
    (void)vfs;
    memset(p, 0, sizeof *p);
    p->fd = -1;
    if (!name || !name[0])
        snprintf(p->path, sizeof p->path, "/tmp/sqlite-%u-%u", (unsigned)getpid(), ++seq);
    else if (store_path(p->path, (int)sizeof p->path, name)) return SQLITE_CANTOPEN;
    if (flags & SQLITE_OPEN_READONLY) of = O_RDONLY;
    else of = O_RDWR;
    if (flags & SQLITE_OPEN_CREATE) of |= O_CREAT;
    if (flags & SQLITE_OPEN_EXCLUSIVE) of |= O_EXCL;
    fd = open(p->path, of, 0666);
    if (fd < 0) return SQLITE_CANTOPEN;
    p->fd = fd;
    p->del = (flags & SQLITE_OPEN_DELETEONCLOSE) != 0;
    p->node = node_get(p->path);
    if (!p->node) { close(fd); p->fd = -1; return SQLITE_NOMEM; }
    p->base.pMethods = &methods;
    if (out) *out = flags & (SQLITE_OPEN_READONLY | SQLITE_OPEN_READWRITE);
    return SQLITE_OK;
}

static int myDelete(sqlite3_vfs *vfs, const char *name, int sync) {
    char path[512];
    (void)vfs;
    (void)sync;
    if (store_path(path, (int)sizeof path, name)) return SQLITE_IOERR_DELETE;
    if (unlink(path) < 0 && errno != ENOENT) return SQLITE_IOERR_DELETE;
    return SQLITE_OK;
}

static int myAccess(sqlite3_vfs *vfs, const char *name, int flags, int *out) {
    char path[512];
    (void)vfs;
    *out = 0;
    if (store_path(path, (int)sizeof path, name)) return SQLITE_OK;
    if (flags == SQLITE_ACCESS_READWRITE) *out = access(path, R_OK | W_OK) == 0;
    else if (flags == SQLITE_ACCESS_READ) *out = access(path, R_OK) == 0;
    else *out = access(path, F_OK) == 0;
    return SQLITE_OK;
}

static int myFullPath(sqlite3_vfs *vfs, const char *name, int nout, char *out) {
    (void)vfs;
    if (store_path(out, nout, name)) return SQLITE_CANTOPEN;
    return SQLITE_OK;
}

static void *myDlOpen(sqlite3_vfs *vfs, const char *name) {
    (void)vfs;
    (void)name;
    return 0;
}
static void myDlError(sqlite3_vfs *vfs, int n, char *msg) {
    (void)vfs;
    if (n > 0) msg[0] = 0;
}
static void (*myDlSym(sqlite3_vfs *vfs, void *h, const char *sym))(void) {
    (void)vfs;
    (void)h;
    (void)sym;
    return 0;
}
static void myDlClose(sqlite3_vfs *vfs, void *h) {
    (void)vfs;
    (void)h;
}
static int myRandom(sqlite3_vfs *vfs, int n, char *out) {
    struct timeval tv;
    unsigned mix;
    int i;
    (void)vfs;
    gettimeofday(&tv, 0);
    mix = (unsigned)tv.tv_sec ^ (unsigned)tv.tv_usec ^ (unsigned)getpid() ^ (unsigned)ticks();
    for (i = 0; i < n; i++) {
        mix = mix * 1664525u + 1013904223u;
        out[i] = (char)(mix >> 16);
    }
    return n;
}
static int mySleep(sqlite3_vfs *vfs, int us) {
    (void)vfs;
    if (us > 0) sleep_ms((unsigned)((us + 999) / 1000));
    return us;
}
static int myTime(sqlite3_vfs *vfs, double *out) {
    struct timeval tv;
    (void)vfs;
    gettimeofday(&tv, 0);
    *out = (double)tv.tv_sec / 86400.0 + (double)tv.tv_usec / 86400000000.0 + 2440587.5;
    return SQLITE_OK;
}
static int myGetLastError(sqlite3_vfs *vfs, int n, char *msg) {
    (void)vfs;
    (void)n;
    (void)msg;
    return 0;
}

static sqlite3_vfs myvfs = {
    1, sizeof(MyFile), 512, 0, "myos", 0,
    myOpen, myDelete, myAccess, myFullPath,
    myDlOpen, myDlError, myDlSym, myDlClose,
    myRandom, mySleep, myTime, myGetLastError,
    0, 0, 0, 0
};

int sqlite3_os_init(void) { return sqlite3_vfs_register(&myvfs, 1); }
int sqlite3_os_end(void) { return SQLITE_OK; }
