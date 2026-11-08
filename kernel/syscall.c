/* Системные вызовы: единственная дверь из программы (ring 3) в ядро.
 *
 * Программа делает int 0x80. Шлюз 0x80 в IDT имеет DPL 3 — это единственное
 * прерывание, которое ей разрешено вызывать самой. Процессор переключается на
 * стек ядра этого процесса (из TSS), isr_common сохраняет регистры, и мы
 * попадаем сюда: номер вызова в eax, аргументы в ebx, ecx, edx.
 *
 * ГЛАВНОЕ ПРАВИЛО: ядро не доверяет ничему, что пришло от программы. Каждый
 * указатель проверяется — он должен лежать в памяти программы, а не ядра,
 * и эта память должна быть отображена (и доступна на запись, если пишем). */
#include "syscall.h"
#include "sysinfo.h"
#include "tty.h"
#include "keyboard.h"
#include "task.h"
#include "paging.h"
#include "pmm.h"
#include "fs.h"
#include "file.h"
#include "heap.h"
#include "timer.h"
#include "string.h"
#include "process.h"
#include "fb.h"
#include "mouse.h"
#include "vga.h"
#include "net.h"
#include "ipc.h"
#include "e1000.h"
#include "display.h"
#include "block.h"
#include "account.h"
#include "services.h"

#define SBRK_LIMIT SHM_BASE         /* выше — общая память (ipc.c) и стек */
#define PATH_MAX   128

/* ---------------------------------------------------------------- проверки */

static bool user_buf_ok(u32 addr, u32 len, bool write) {
    if (len == 0) return true;
    if (addr < USER_BASE || addr >= USER_TOP || len > USER_TOP - addr) return false;
    u32 dir = task_current()->page_dir;
    for (u32 p = addr & ~(PAGE_SIZE - 1); p < addr + len; p += PAGE_SIZE) {
        u32 flags;
        if (!vmm_lookup(dir, p, NULL, &flags) || !(flags & PG_USER)) return false;
        if (write && !(flags & PG_WRITE)) return false;
    }
    return true;
}

/* скопировать строку из программы в буфер ядра (с проверкой каждой страницы) */
static bool user_str(u32 addr, char *out, u32 max) {
    for (u32 i = 0; i < max; i++) {
        if (!user_buf_ok(addr + i, 1, false)) return false;
        out[i] = *(const char *)(addr + i);
        if (!out[i]) return true;
    }
    return false;   /* слишком длинная */
}

/* ошибки файловой системы и загрузчика -> errno */
static int fs_errno(int e) {
    switch (e) {
    case FS_ENOENT:       return -ENOENT;
    case FS_EEXIST:       return -EEXIST;
    case FS_ENOTDIR:      return -ENOTDIR;
    case FS_EISDIR:       return -EISDIR;
    case FS_ENOSPC:       return -ENOSPC;
    case FS_ENOTEMPTY:    return -ENOTEMPTY;
    case FS_EINVAL:       return -EINVAL;
    case FS_EIO:          return -EIO;
    case FS_ENODEV:       return -ENODEV;
    case FS_EFBIG:        return -EFBIG;
    case FS_ENAMETOOLONG: return -ENAMETOOLONG;
    case FS_EROFS:        return -30;
    case FS_EBUSY:        return -16;
    case FS_EXDEV:        return -18;
    default:              return e < 0 ? -EIO : e;
    }
}

static int proc_errno(int e) {
    switch (e) {
    case PROC_ENOENT:  return -ENOENT;
    case PROC_ENOEXEC: return -ENOEXEC;
    case PROC_ENOMEM:  return -ENOMEM;
    case PROC_EAGAIN:  return -EAGAIN;
    case PROC_EISDIR:  return -EISDIR;
    default:           return e;
    }
}

static struct file *get_fd(u32 fd) {
    struct task *t = task_current();
    return fd < MAX_FDS ? t->fds[fd] : NULL;
}

static int alloc_fd(struct file *f) {
    struct task *t = task_current();
    for (int i = 0; i < MAX_FDS; i++) if (!t->fds[i]) { t->fds[i] = f; return i; }
    return -EMFILE;
}

/* ---------------------------------------------------------------- файлы */

/* Copied into kernel storage: survives the source application's exit.
 * A different account must not read the previous account's clipboard. */
static char clipboard[65536];
static u32 clipboard_length, clipboard_uid;
static struct mutex clipboard_lock;
static int sys_clipboard(u32 op, u32 addr, u32 size) {
    if (op > 1) return -EINVAL;
    if (size > sizeof(clipboard)) return -EFBIG;
    if (size && !user_buf_ok(addr, size, op == 0)) return -EFAULT;
    mutex_lock(&clipboard_lock);
    u32 uid = task_current()->uid;
    int result;
    if (op == 1) {
        if (size) memcpy(clipboard, (void *)addr, size);
        clipboard_length = size;
        clipboard_uid = uid;
        result = size;
    } else {
        u32 length = uid == clipboard_uid ? clipboard_length : 0;
        if (!size) result = length;
        else if (size < length) result = -EFBIG;
        else { if (length) memcpy((void *)addr, clipboard, length); result = length; }
    }
    mutex_unlock(&clipboard_lock);
    return result;
}

static int sys_write(u32 fd, u32 buf, u32 len) {
    struct file *f = get_fd(fd);
    if (!f) return -EBADF;
    if (!user_buf_ok(buf, len, false)) return -EFAULT;
    return file_write(f, (const void *)buf, len, fd == 2);
}

static int sys_read(u32 fd, u32 buf, u32 len) {
    struct file *f = get_fd(fd);
    if (!f) return -EBADF;
    if (!user_buf_ok(buf, len, true)) return -EFAULT;
    return file_read(f, (void *)buf, len);
}

/* flags — как O_* в Linux i386: младшие 2 бита это доступ, дальше O_CREAT/EXCL/TRUNC/APPEND. */
static int sys_open_impl(u32 upath, u32 flags) {
    struct task *t = task_current();
    char path[PATH_MAX];
    if (!user_str(upath, path, sizeof(path))) return -EFAULT;
    u32 acc = flags & 3;
    if (acc > 2) return -EINVAL;
    bool want_write = acc != 0;
    bool want_read = acc != 1;
    bool creat = (flags & 0100) != 0;
    bool excl = (flags & 0200) != 0;
    bool trunc = (flags & 01000) != 0;
    bool append = (flags & 02000) != 0;

    u32 ino;
    struct fs_stat st;
    int r = fs_resolve(t->cwd, path, &ino);
    bool existed = r == 0;
    if (r == FS_ENOENT && creat && want_write) r = fs_create(t->cwd, path, FS_TYPE_FILE, &ino);
    if (r) return fs_errno(r);
    if (account_private(ino)) return -EPERM;
    if ((r = fs_stat(ino, &st))) return fs_errno(r);
    if (st.type == FS_TYPE_BLOCK && (!t->authenticated || t->uid != 0)) return -EPERM;
    if (st.type == FS_TYPE_DIR) return -EISDIR;
    if (st.type == FS_TYPE_BLOCK && want_write) return -EROFS;
    if (excl && creat && existed) return -EEXIST;
    if (trunc && want_write && (r = fs_truncate(ino))) return fs_errno(r);

    u8 mode = (want_read ? FMODE_READ : 0) | (want_write ? FMODE_WRITE : 0) | (append ? FMODE_APPEND : 0);
    struct file *f = file_open_disk(ino, mode, 0);
    if (!f) return -ENOMEM;
    int fd = alloc_fd(f);
    if (fd < 0) file_close(f);
    return fd;
}

static int sys_open(u32 upath,u32 mode) {
    fs_namespace_lock();
    int r=sys_open_impl(upath,mode);
    fs_namespace_unlock();
    return r;
}

static int sys_close(u32 fd) {
    struct file *f = get_fd(fd);
    if (!f) return -EBADF;
    task_current()->fds[fd] = NULL;
    file_close(f);
    return 0;
}

static int sys_lseek(u32 fd, u32 off, u32 whence) {
    struct file *f = get_fd(fd);
    if (!f) return -EBADF;
    return file_lseek(f, (int)off, (int)whence);
}

static int sys_fstat(u32 fd, u32 ust) {
    struct file *f = get_fd(fd);
    if (!f) return -EBADF;
    if (!user_buf_ok(ust, 8, true)) return -EFAULT;
    u32 mode = 0, size = 0;
    if (f->type == FILE_DISK) {
        struct fs_stat st;
        if (fs_stat(f->ino, &st)) return -EIO;
        size = st.size;
        if (st.type == FS_TYPE_DIR) mode = 0040000;
        else if (st.type == FS_TYPE_FILE) mode = 0100000;
    } else if (f->type == FILE_CONSOLE || f->type == FILE_PTY_SLAVE) {
        mode = 0020000;                                 /* S_IFCHR: isatty */
    }
    ((u32 *)ust)[0] = mode;
    ((u32 *)ust)[1] = size;
    return 0;
}

static int sys_pipe(u32 ufds) {
    if (!user_buf_ok(ufds, 8, true)) return -EFAULT;
    struct file *rd, *wr;
    if (pipe_create(&rd, &wr)) return -ENOMEM;
    int a = alloc_fd(rd), b = a >= 0 ? alloc_fd(wr) : -1;
    if (a < 0 || b < 0) {
        if (a >= 0) task_current()->fds[a] = NULL;
        file_close(rd);
        file_close(wr);
        return -EMFILE;
    }
    ((int *)ufds)[0] = a;
    ((int *)ufds)[1] = b;
    return 0;
}

static int sys_pty_open(u32 ufds) {
    if (!user_buf_ok(ufds, 8, true)) return -EFAULT;
    struct file *m, *sl;
    int r = pty_create(&m, &sl);
    if (r) return r;
    int a = alloc_fd(m), b = a >= 0 ? alloc_fd(sl) : -1;
    if (a < 0 || b < 0) {
        if (a >= 0) task_current()->fds[a] = NULL;
        file_close(m);
        file_close(sl);
        return -EMFILE;
    }
    ((int *)ufds)[0] = a;
    ((int *)ufds)[1] = b;
    return 0;
}

/* dup2(old, new): new начинает указывать на тот же открытый файл, что и old.
 * Так оболочка подключает stdout программы к каналу или файлу. */
static int sys_dup2(u32 oldfd, u32 newfd) {
    struct task *t = task_current();
    struct file *f = get_fd(oldfd);
    if (!f || newfd >= MAX_FDS) return -EBADF;
    if (oldfd == newfd) return newfd;
    file_close(t->fds[newfd]);
    t->fds[newfd] = file_dup(f);
    return newfd;
}

/* ---------------------------------------------------------------- каталоги */

static int sys_chdir(u32 upath) {
    struct task *t = task_current();
    char path[PATH_MAX];
    u32 ino;
    struct fs_stat st;
    if (!user_str(upath, path, sizeof(path))) return -EFAULT;
    int r = fs_resolve(t->cwd, path, &ino);
    if (!r) r = fs_stat(ino, &st);
    if (r) return fs_errno(r);
    if (st.type != FS_TYPE_DIR) return -ENOTDIR;
    t->cwd = ino;
    return 0;
}

static int sys_getcwd(u32 buf, u32 len) {
    char path[PATH_MAX];
    if (!user_buf_ok(buf, len, true)) return -EFAULT;
    int r = fs_getpath(task_current()->cwd, path, sizeof(path));
    if (r) return fs_errno(r);
    u32 n = strlen(path) + 1;
    if (n > len) return -ENAMETOOLONG;
    memcpy((void *)buf, path, n);
    return 0;
}

/* index-я запись каталога (без . и ..) */
static int sys_readdir(u32 upath, u32 index, u32 uout) {
    char path[PATH_MAX];
    if (!user_str(upath, path, sizeof(path))) return -EFAULT;
    if (!user_buf_ok(uout, sizeof(struct dirent_info), true)) return -EFAULT;
    u32 dir, pos = 0;
    int r = fs_resolve(task_current()->cwd, path, &dir);
    if (r) return fs_errno(r);
    struct fs_dirent de;
    for (u32 i = 0; ; i++) {
        r = fs_readdir(dir, &pos, &de);
        if (r <= 0) return fs_errno(r);
        if (i == index) break;
    }
    struct fs_stat st;
    if ((r = fs_stat(de.ino, &st))) return fs_errno(r);
    struct dirent_info *out = (struct dirent_info *)uout;
    memcpy(out->name, de.name, sizeof(out->name));
    out->type = st.type;
    out->size = st.size;
    return 1;
}

static int sys_stat(u32 upath, u32 uout) {
    char path[PATH_MAX];
    if (!user_str(upath, path, sizeof(path))) return -EFAULT;
    if (!user_buf_ok(uout, sizeof(struct dirent_info), true)) return -EFAULT;
    u32 ino;
    struct fs_stat st;
    int r = fs_resolve(task_current()->cwd, path, &ino);
    if (!r) r = fs_stat(ino, &st);
    if (r) return fs_errno(r);
    struct dirent_info *out = (struct dirent_info *)uout;
    const char *b = path + strlen(path);
    while (b > path && b[-1] != '/') b--;
    memset(out->name, 0, sizeof(out->name));
    memcpy(out->name, b, strlen(b) < sizeof(out->name) - 1 ? strlen(b) : sizeof(out->name) - 1);
    out->type = st.type;
    out->size = st.size;
    return 0;
}

static int sys_mkdir(u32 upath) {
    char path[PATH_MAX];
    if (!user_str(upath, path, sizeof(path))) return -EFAULT;
    return fs_errno(fs_create(task_current()->cwd, path, FS_TYPE_DIR, NULL));
}

static int sys_unlink(u32 upath) {
    char path[PATH_MAX];
    if (!user_str(upath, path, sizeof(path))) return -EFAULT;
    u32 ino;
    if(!fs_resolve(task_current()->cwd,path,&ino) && account_private(ino)) return -EPERM;
    return fs_errno(fs_remove(task_current()->cwd, path));
}

static int sys_rename(u32 uold, u32 unew) {
    char oldp[PATH_MAX], newp[PATH_MAX];
    if (!user_str(uold, oldp, sizeof(oldp)) || !user_str(unew, newp, sizeof(newp))) return -EFAULT;
    /* Resolve and rename under one namespace lock: checking a path before
     * acquiring the VFS lock would let another task move it in between. */
    fs_namespace_lock();
    u32 source, target, etc;
    u32 cwd = task_current()->cwd;
    int result;
    if (!fs_resolve(cwd, oldp, &source) &&
        (account_private(source) ||
         (!fs_resolve(FS_ROOT, "/etc", &etc) && source == etc))) result = -EPERM;
    else if (!fs_resolve(cwd, newp, &target) && account_private(target)) result = -EPERM;
    else result = fs_errno(fs_rename(cwd, oldp, newp));
    fs_namespace_unlock();
    return result;
}

/* ---------------------------------------------------------------- процессы */

static int sys_exec(struct regs *r, u32 upath, u32 uargv) {
    /* всё, что пришло от программы, копируем в ядро: её память сейчас исчезнет */
    char *buf = kmalloc(PATH_MAX + (PROC_MAX_ARGS) * 256);
    if (!buf) return -ENOMEM;
    char *argv[PROC_MAX_ARGS + 1];
    int argc = 0, ret;
    if (!user_str(upath, buf, PATH_MAX)) { ret = -EFAULT; goto out; }
    for (; argc < PROC_MAX_ARGS; argc++) {
        u32 slot = uargv + argc * 4;
        if (!user_buf_ok(slot, 4, false)) { ret = -EFAULT; goto out; }
        u32 s = *(u32 *)slot;
        if (!s) break;
        argv[argc] = buf + PATH_MAX + argc * 256;
        if (!user_str(s, argv[argc], 256)) { ret = -EFAULT; goto out; }
    }
    if (argc == 0) argv[argc++] = buf;      /* argv[0] по умолчанию — путь */
    ret = proc_errno(proc_exec(r, buf, argc, argv));
out:
    kfree(buf);
    return ret;
}

static int sys_waitpid(i32 pid, u32 ustatus, u32 opts) {
    if (ustatus && !user_buf_ok(ustatus, 4, true)) return -EFAULT;
    int code = 0;
    int r = task_wait_child(pid, &code, opts & WNOHANG);
    if (r == -1) return -ECHILD;
    if (r == -2) return -EINTR;
    if (r > 0) task_reap();                 /* сразу вернуть память ребёнка */
    if (r > 0 && ustatus) *(int *)ustatus = code;
    return r;
}

static int sys_kill(u32 pid) {
    struct task *v = task_by_id(pid);
    if (!v) return -ESRCH;
    if (!v->user) return -EPERM;            /* потоки ядра программе не по зубам */
    return task_kill(pid) == 0 ? 0 : -ESRCH;
}

static int sys_setpgid(u32 pid, u32 pgid) {
    struct task *t = task_current();
    struct task *v = pid ? task_by_id(pid) : t;
    if (!v) return -ESRCH;
    if (v != t && v->parent != t->id) return -EPERM;   /* только себе и своим детям */
    v->pgid = pgid ? pgid : v->id;
    return 0;
}

static int sys_ps(u32 ubuf, u32 max) {
    if (max > MAX_TASKS) max = MAX_TASKS;
    if (!user_buf_ok(ubuf, max * sizeof(struct proc_info), true)) return -EFAULT;
    static struct task_info list[MAX_TASKS];
    int n = task_list(list, max);
    struct proc_info *out = (struct proc_info *)ubuf;
    for (int i = 0; i < n; i++) {
        struct task *t = task_by_id(list[i].id);
        memset(&out[i], 0, sizeof(out[i]));
        out[i].pid = list[i].id;
        out[i].ppid = list[i].parent;
        out[i].pgid = t ? t->pgid : 0;
        memcpy(out[i].name, list[i].name, 16);
        out[i].state = list[i].state;
        out[i].ring = list[i].user ? 3 : 0;
        out[i].cpu_percent = list[i].cpu_percent;
        out[i].cpu_ms = list[i].cpu_ticks * (1000 / TIMER_HZ);
        out[i].mem_kb = list[i].mem_kb;
    }
    return n;
}

/* ---------------------------------------------------------------- графика */

static int sys_gfx_info(u32 uout) {
    if (!fb_ready()) return -ENODEV;
    if (!user_buf_ok(uout, sizeof(struct gfx_info), true)) return -EFAULT;
    struct fb_info fi;
    fb_get_info(&fi);
    struct gfx_info *o = (struct gfx_info *)uout;
    o->width = fi.width; o->height = fi.height; o->bpp = fi.bpp; o->pitch = fi.pitch;
    return 0;
}

static int sys_gfx_acquire(u32 on) {
    int r = fb_acquire(task_current()->id, on != 0);
    return r == -1 ? -ENODEV : r == -2 ? -EAGAIN : 0;
}

/* Программа рисует в своём буфере ширина x высота пикселей и отдаёт ядру
 * прямоугольник, который надо перенести на экран. Буфер проверяем целиком —
 * только ту часть, из которой будем читать. */
static int sys_gfx_blit(u32 ubuf, u32 urect) {
    struct task *t = task_current();
    if (!fb_ready()) return -ENODEV;
    if (!fb_is_owner(t->id)) return -EPERM;
    if (!user_buf_ok(urect, sizeof(struct gfx_rect), false)) return -EFAULT;
    struct gfx_rect r = *(struct gfx_rect *)urect;
    struct fb_info fi;
    fb_get_info(&fi);
    if (r.x < 0) { r.w += r.x; r.x = 0; }
    if (r.y < 0) { r.h += r.y; r.y = 0; }
    if (r.x + r.w > (int)fi.width)  r.w = fi.width - r.x;
    if (r.y + r.h > (int)fi.height) r.h = fi.height - r.y;
    if (r.w <= 0 || r.h <= 0) return 0;
    u32 first = ubuf + ((u32)r.y * fi.width + r.x) * 4;
    u32 last  = ubuf + ((u32)(r.y + r.h - 1) * fi.width + r.x + r.w) * 4;
    if (!user_buf_ok(first, last - first, false)) return -EFAULT;
    fb_blit((const u32 *)ubuf, fi.width, r.x, r.y, r.w, r.h);
    return 0;
}

static int sys_gfx_mode(u32 w, u32 h) {
    if (!fb_ready()) return -ENODEV;
    return display_set_mode(w, h) < 0 ? -EINVAL : 0;
}

static int sys_gfx_info2(u32 uout) {
    if (!fb_ready()) return -ENODEV;
    if (!user_buf_ok(uout, sizeof(struct gfx_info2), true)) return -EFAULT;
    struct fb_info fi;
    fb_get_info(&fi);
    struct gfx_info2 *o = (struct gfx_info2 *)uout;
    memset(o, 0, sizeof(*o));
    o->width = fi.width; o->height = fi.height; o->bpp = fi.bpp; o->pitch = fi.pitch;
    o->vram_kb = fi.vram >> 10; o->max_w = fi.max_w; o->max_h = fi.max_h;
    if (o->max_w > FB_MAX_W) o->max_w = FB_MAX_W;
    if (o->max_h > FB_MAX_H) o->max_h = FB_MAX_H;
    o->caps = fi.caps; o->gen = fi.gen;
    o->hw_cursor = fb_cursor_enabled();
    for (int i = 0; i < 23 && fi.driver[i]; i++) o->driver[i] = fi.driver[i];
    return 0;
}

/* копирование и заливка прямоугольника прямо на экране — делает видеокарта */
static int sys_gfx_accel(u32 op, u32 uargs) {
    if (!fb_ready()) return -ENODEV;
    if (!fb_is_owner(task_current()->id)) return -EPERM;
    if (!user_buf_ok(uargs, 6 * 4, false)) return -EFAULT;
    const i32 *a = (const i32 *)uargs;
    if (op == GA_COPY) { fb_copy(a[0], a[1], a[2], a[3], a[4], a[5]); return 0; }
    if (op == GA_FILL) { fb_fill(a[0], a[1], a[2], a[3], (u32)a[4]); return 0; }
    return -EINVAL;
}

static int sys_gfx_cursor(u32 on, u32 ushape) {
    if (!fb_ready()) return -ENODEV;
    if (!fb_is_owner(task_current()->id)) return -EPERM;
    if (ushape) {
        if (!user_buf_ok(ushape, sizeof(struct gfx_cursor), false)) return -EFAULT;
        const struct gfx_cursor *c = (const struct gfx_cursor *)ushape;
        if (c->w <= 0 || c->h <= 0 || c->w > 32 || c->h > 32) return -EINVAL;
        if (fb_cursor_define(c->px, c->w, c->h, c->hot_x, c->hot_y) < 0) return -ENODEV;
    }
    if (fb_cursor_enable(on != 0) < 0) return -ENODEV;
    if (on) { struct mouse_state m; mouse_get(&m); fb_cursor_move(m.x, m.y); }
    return 0;
}

static int sys_mouse(u32 uout) {
    if (!user_buf_ok(uout, sizeof(struct mouse_state), true)) return -EFAULT;
    mouse_get((struct mouse_state *)uout);
    return 0;
}

static int sys_gfx_font(u32 ubuf) {
    if (!fb_ready()) return -ENODEV;
    if (!user_buf_ok(ubuf, 256 * FONT_H, true)) return -EFAULT;
    memcpy((void *)ubuf, fb_font(), 256 * FONT_H);
    return 0;
}

/* Сдвинуть границу кучи программы. Новые страницы выделяются сразу. */
static u32 sys_sbrk(i32 inc) {
    struct task *t = task_current();
    u32 old = t->brk, new = old + inc;
    if (inc < 0 ? new < t->brk_start || new > old : new > SBRK_LIMIT || new < old)
        return (u32)-1;
    for (u32 va = (old + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1); va < new; va += PAGE_SIZE) {
        u32 frame = pmm_alloc();
        if (!frame) return (u32)-1;
        memset((void *)frame, 0, PAGE_SIZE);
        if (vmm_map_in(t->page_dir, va, frame, PG_USER | PG_WRITE)) { pmm_free(frame); return (u32)-1; }
        t->user_pages++;
        t->brk = va + PAGE_SIZE < new ? va + PAGE_SIZE : new;   /* на случай нехватки памяти */
    }
    t->brk = new;
    return old;
}


/* ---------------------------------------------------------------- сеть */

struct user_sockaddr { u32 ip; u32 port; };

struct net_info {
    u8  mac[6]; u8 up, dhcp;
    u32 ip, mask, gw, dns, lease;
    u32 rx_packets, tx_packets, rx_bytes, tx_bytes;
};

static struct sock *get_sock(u32 fd) {
    struct file *f = get_fd(fd);
    return f && f->type == FILE_SOCKET ? f->sock : NULL;
}

static int sys_socket(u32 type) {
    if (!netcfg.up) return -ENETDOWN;
    if (type != SOCK_TCP && type != SOCK_UDP) return -EINVAL;
    struct sock *s = sock_alloc(type);
    if (!s) return -ENOMEM;
    struct file *f = file_open_socket(s);
    if (!f) { sock_free(s); return -ENOMEM; }
    int fd = alloc_fd(f);
    if (fd < 0) file_close(f);
    return fd;
}

static int sys_connect(u32 fd, u32 ip, u32 port) {
    struct sock *s = get_sock(fd);
    if (!s) return -EBADF;
    if (s->type == SOCK_TCP && s->state != TCP_CLOSED) return -EISCONN;
    return sock_connect(s, ip, (u16)port);
}

static int sys_bind(u32 fd, u32 port) {
    struct sock *s = get_sock(fd);
    if (!s) return -EBADF;
    if (!port || port > 65535) return -EINVAL;
    return sock_bind(s, (u16)port);
}

static int sys_listen(u32 fd, u32 backlog) {
    struct sock *s = get_sock(fd);
    if (!s) return -EBADF;
    return sock_listen(s, (int)backlog);
}

static int sys_accept(u32 fd, u32 uaddr) {
    struct sock *s = get_sock(fd);
    if (!s) return -EBADF;
    if (uaddr && !user_buf_ok(uaddr, sizeof(struct user_sockaddr), true)) return -EFAULT;
    int err = 0;
    struct sock *c = sock_accept(s, &err);
    if (!c) return err ? err : -EINVAL;
    struct file *f = file_open_socket(c);
    if (!f) { sock_close(c); return -ENOMEM; }
    int nfd = alloc_fd(f);
    if (nfd < 0) { file_close(f); return nfd; }
    if (uaddr) {
        struct user_sockaddr *a = (void *)uaddr;
        a->ip = c->rip; a->port = c->rport;
    }
    return nfd;
}

static int sys_sendto(u32 fd, u32 buf, u32 len, u32 ip, u32 port) {
    struct sock *s = get_sock(fd);
    if (!s) return -EBADF;
    if (len > 1472) return -EMSGSIZE;
    if (!user_buf_ok(buf, len, false)) return -EFAULT;
    return sock_sendto(s, (const void *)buf, len, ip, (u16)port);
}

static int sys_recvfrom(u32 fd, u32 buf, u32 len, u32 uaddr, u32 timeout) {
    struct sock *s = get_sock(fd);
    if (!s) return -EBADF;
    if (!user_buf_ok(buf, len, true)) return -EFAULT;
    if (uaddr && !user_buf_ok(uaddr, sizeof(struct user_sockaddr), true)) return -EFAULT;
    u32 ip = 0; u16 port = 0;
    int r = sock_recvfrom(s, (void *)buf, len, &ip, &port, timeout);
    if (r >= 0 && uaddr) { struct user_sockaddr *a = (void *)uaddr; a->ip = ip; a->port = port; }
    return r;
}

static int sys_netinfo(u32 uinfo) {
    if (!user_buf_ok(uinfo, sizeof(struct net_info), true)) return -EFAULT;
    struct net_info *n = (void *)uinfo;
    memcpy(n->mac, netcfg.mac, 6);
    n->up = netcfg.up; n->dhcp = netcfg.dhcp;
    n->ip = netcfg.ip; n->mask = netcfg.mask; n->gw = netcfg.gw; n->dns = netcfg.dns;
    n->lease = netcfg.lease;
    e1000_stats(&n->rx_packets, &n->tx_packets, &n->rx_bytes, &n->tx_bytes);
    return 0;
}

static int sys_netconfig(u32 what, u32 value) {
    if (what == 1) { netcfg.dns = value; return 0; }
    return -EINVAL;
}

/* ---------------------------------------------------------------- общение процессов */

static int sys_shm_map(u32 id) {
    u32 addr;
    int r = shm_map((int)id, &addr);
    return r < 0 ? r : (int)addr;                   /* адрес >= 0xA0000000: в libc отличаем от -errno */
}

static int sys_msg_send(u32 pid, u32 umsg) {
    if (!user_buf_ok(umsg, MSG_SIZE, false)) return -EFAULT;
    u8 m[MSG_SIZE];
    memcpy(m, (void *)umsg, MSG_SIZE);
    return msg_send(pid, m);
}

static int sys_msg_recv(u32 umsg, u32 timeout) {
    if (!user_buf_ok(umsg, MSG_SIZE, true)) return -EFAULT;
    u8 m[MSG_SIZE];
    int r = msg_recv(m, (int)timeout);
    if (r == 0) memcpy((void *)umsg, m, MSG_SIZE);
    return r;
}

static int sys_service(u32 op, u32 uname) {
    char name[16];
    if (!user_str(uname, name, sizeof(name))) return -EFAULT;
    if (op == 1) return svc_register(name);
    if (op == 2) return svc_find(name);
    return -EINVAL;
}

static int sys_time(u32 uout) {
    if (!user_buf_ok(uout, sizeof(struct rtc_time), true)) return -EFAULT;
    rtc_read((struct rtc_time *)uout);
    return 0;
}

void syscall_dispatch(struct regs *r) {
    struct task *t = task_current();
    t->in_syscall = true;
    __asm__ volatile ("sti");   /* шлюз прерывания запретил IRQ; пока работаем в ядре — разрешим */

    u32 a = r->ebx, b = r->ecx, c = r->edx, d = r->esi, e = r->edi;
    int ret = -ENOSYS;
    switch (r->eax) {
    case SYS_EXIT:    t->in_syscall = false; task_exit_code((int)a);
    case SYS_WRITE:
        ret = sys_write(a, b, c);
        if (ret == -EPIPE) {                /* как SIGPIPE: читателей нет — писатель умирает */
            t->kill_pending = true;
            t->pending_code = 141;          /* 128 + 13 */
        }
        break;
    case SYS_READ:    ret = sys_read(a, b, c); break;
    case SYS_OPEN:    ret = sys_open(a, b); break;
    case SYS_MOUNT: {
        if(!t->authenticated || t->uid!=0) {ret=-EPERM;break;}
        char device[128],target[128];
        ret = !user_str(a,device,sizeof(device)) || !user_str(b,target,sizeof(target)) ? -EFAULT : fs_errno(fs_mount(t->cwd,device,target));
        break;
    }
    case SYS_MKFS: {
        if(!t->authenticated || t->uid!=0) {ret=-EPERM;break;}
        char device[128];
        ret=!user_str(a,device,sizeof(device)) ? -EFAULT : fs_errno(fs_mkfs(device));
        break;
    }
    case SYS_PARTCTL: {
        if(!t->authenticated || t->uid!=0) {ret=-EPERM;break;}
        char device[128]; u32 args[3];
        if(!user_str(a,device,sizeof(device)) || !user_buf_ok(c,sizeof(args),false)) { ret=-EFAULT; break; }
        memcpy(args,(const void*)c,sizeof(args));
        ret=fs_errno(fs_partition(device,b,args[0],args[1],args[2]));
        break;
    }
    case SYS_MOUNTINFO: {
        char device[128],target[128];
        if(!user_buf_ok(b,128,true) || !user_buf_ok(c,128,true)) { ret=-EFAULT; break; }
        ret=fs_mount_info(a,device,target);
        if(ret>0) { memcpy((void*)b,device,strlen(device)+1); memcpy((void*)c,target,strlen(target)+1); }
        else ret=fs_errno(ret);
        break;
    }
    case SYS_BLOCKINFO: {
        struct { char name[12]; u32 start,sectors,partition; } info;
        if(!user_buf_ok(b,sizeof(info),true)) { ret=-EFAULT; break; }
        const struct block_device *dev=block_get(block_at(a));
        ret=0;
        if(dev) { memset(&info,0,sizeof(info)); memcpy(info.name,dev->name,12); info.start=dev->start; info.sectors=dev->sectors; info.partition=dev->partition; memcpy((void*)b,&info,sizeof(info)); ret=1; }
        break;
    }
    case SYS_ACCOUNT: {
        char name[24]={0}, password[128]={0};
        if ((b && a!=5 && a!=7 && !user_str(b,name,sizeof(name))) || (c && a!=7 && !user_str(c,password,sizeof(password)))) ret=-EFAULT;
        else if(a==5 || a==7) {
            if(!user_buf_ok(b,24,true)) ret=-EFAULT;
            else ret=a==7 ? account_list((char *)b,c) : account_name((char *)b);
        } else ret=account_ctl(a,name,password);
        volatile char *v=password;for(unsigned i=0;i<sizeof(password);i++) v[i]=0;
        break;
    }
    case SYS_SYSINFO: {
        if (b < sizeof(struct system_info)) { ret=-EINVAL; break; }
        if (!user_buf_ok(a,sizeof(struct system_info),true)) { ret=-EFAULT; break; }
        struct system_info info; sysinfo_get(&info);
        memcpy((void *)a,&info,sizeof(info)); ret=0; break;
    }
    case SYS_CLOSE:   ret = sys_close(a); break;
    case SYS_LSEEK:   ret = sys_lseek(a, b, c); break;
    case SYS_FSTAT:   ret = sys_fstat(a, b); break;
    case SYS_SBRK:    ret = (int)sys_sbrk((i32)a); break;
    case SYS_SLEEP:   sleep_ms(a); ret = 0; break;
    case SYS_GETPID:  ret = t->id; break;
    case SYS_TICKS:   ret = timer_ticks() * (1000 / TIMER_HZ); break;
    case SYS_YIELD:   task_yield(); ret = 0; break;
    case SYS_FORK:    ret = proc_errno(proc_fork(r)); break;
    case SYS_EXEC:    ret = sys_exec(r, a, b); break;
    case SYS_WAITPID: ret = sys_waitpid((i32)a, b, c); break;
    case SYS_PIPE:    ret = sys_pipe(a); break;
    case SYS_DUP2:    ret = sys_dup2(a, b); break;
    case SYS_CHDIR:   ret = sys_chdir(a); break;
    case SYS_GETCWD:  ret = sys_getcwd(a, b); break;
    case SYS_READDIR: ret = sys_readdir(a, b, c); break;
    case SYS_KILL:    ret = sys_kill(a); break;
    case SYS_SETPGID: ret = sys_setpgid(a, b); break;
    case SYS_SETFG: {
        struct tty *tt = tty_of(t);
        if (tt) tt->fg = a; else proc_set_foreground(a);
        ret = 0;
        break;
    }
    case SYS_MKDIR:   ret = sys_mkdir(a); break;
    case SYS_UNLINK:  ret = sys_unlink(a); break;
    case SYS_RENAME:  ret = sys_rename(a, b); break;
    case SYS_PS:      ret = sys_ps(a, b); break;
    case SYS_GETPPID: ret = t->parent; break;
    case SYS_TTYMODE: {
        struct tty *tt = tty_of(t);
        ret = tt ? (int)tty_set_mode(tt, a & 7) : (int)console_set_mode(a & 7);
        break;
    }
    case SYS_STAT:    ret = sys_stat(a, b); break;
    case SYS_GFX_INFO:    ret = sys_gfx_info(a); break;
    case SYS_GFX_ACQUIRE: ret = sys_gfx_acquire(a); break;
    case SYS_GFX_BLIT:    ret = sys_gfx_blit(a, b); break;
    case SYS_MOUSE:       ret = sys_mouse(a); break;
    case SYS_GFX_FONT:    ret = sys_gfx_font(a); break;
    case SYS_TERMSIZE: {
        struct tty *tt = tty_of(t);
        ret = tt ? (int)(tt->cols << 16 | tt->rows) : (vga_cols() << 16) | vga_rows();
        break;
    }
    case SYS_SOCKET:      ret = sys_socket(a); break;
    case SYS_CONNECT:     ret = sys_connect(a, b, c); break;
    case SYS_BIND:        ret = sys_bind(a, b); break;
    case SYS_LISTEN:      ret = sys_listen(a, b); break;
    case SYS_ACCEPT:      ret = sys_accept(a, b); break;
    case SYS_SENDTO:      ret = sys_sendto(a, b, c, d, e); break;
    case SYS_RECVFROM:    ret = sys_recvfrom(a, b, c, d, e); break;
    case SYS_NETINFO:     ret = sys_netinfo(a); break;
    case SYS_PING:        ret = netcfg.up ? net_ping(a, (u16)b, c) : -ENETDOWN; break;
    case SYS_NETCONFIG:   ret = sys_netconfig(a, b); break;
    case SYS_SHM_ALLOC:   ret = shm_alloc(a); break;
    case SYS_SHM_MAP:     ret = sys_shm_map(a); break;
    case SYS_SHM_UNMAP:   ret = shm_unmap(a); break;
    case SYS_MSG_SEND:    ret = sys_msg_send(a, b); break;
    case SYS_MSG_RECV:    ret = sys_msg_recv(a, b); break;
    case SYS_UNITCTL: {
        char name[28] = {0};
        if (a != UNIT_LIST && a != UNIT_RELOAD && !user_str(b, name, sizeof(name))) { ret = -EFAULT; break; }
        if (c && !user_buf_ok(c, sizeof(struct unit_info), true)) { ret = -EFAULT; break; }
        struct unit_info info = {0};
        ret = services_ctl(a, name, &info, b);
        if (!ret && c) memcpy((void *)c, &info, sizeof(info));
        break;
    }
    case SYS_SERVICE:     ret = sys_service(a, b); break;
    case SYS_TIME:        ret = sys_time(a); break;
    case SYS_GFX_MODE:    ret = sys_gfx_mode(a, b); break;
    case SYS_GFX_INFO2:   ret = sys_gfx_info2(a); break;
    case SYS_GFX_ACCEL:   ret = sys_gfx_accel(a, b); break;
    case SYS_GFX_CURSOR:  ret = sys_gfx_cursor(a, b); break;
    case SYS_PTY_OPEN:    ret = sys_pty_open(a); break;
    case SYS_TTYCTL:      ret = tty_ctl(get_fd(a), b, c); break;
    case SYS_SOCKCTL:     { struct sock *s = get_sock(a); ret = s ? sock_ctl(s, (int)b, c) : -EBADF; break; }
    case SYS_CLIPBOARD: ret = sys_clipboard(a, b, c); break;
    case SYS_KBLAYOUT:    if ((int)a >= 0) kb_set_layout(a); ret = kb_layout(); break;
    }
    r->eax = (u32)ret;

    __asm__ volatile ("cli");
    t->in_syscall = false;
    if (t->kill_pending) task_exit_code(t->pending_code ? t->pending_code : 137);  /* убит, пока был в ядре */
}

