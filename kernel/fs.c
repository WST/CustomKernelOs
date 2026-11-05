/* MyFS: реализация. Описание формата — в fs.h. */
#include "fs.h"
#include "ata.h"
#include "string.h"
#include "task.h"

#define BS            512                       /* размер блока */
#define MAGIC         0x5346594D                /* "MYFS" в little-endian */
#define VERSION       1
#define NDIRECT       12
#define NINDIRECT     (BS / 4)                  /* 128 ссылок в косвенном блоке */
#define NDINDIRECT    (NINDIRECT * NINDIRECT)  /* 16384 блока через двойной косвенный */
#define MAX_FILE_BLKS (NDIRECT + NINDIRECT + NDINDIRECT)
#define INODES_PER_BLK (BS / sizeof(struct inode))
#define BITS_PER_BLK  (BS * 8)
#define MAX_BLOCKS    65536                     /* 32 МБ — ограничение битмапа в памяти */
#define INODE_COUNT   256

struct superblock {
    u32 magic;
    u32 version;
    u32 total_blocks;
    u32 bitmap_start, bitmap_blocks;
    u32 inode_start, inode_blocks, inode_count;
    u32 data_start;
    u8  pad[BS - 9 * 4];
} __attribute__((packed));

struct inode {
    u16 type;
    u16 links;
    u32 size;
    u32 direct[NDIRECT];
    u32 indirect;
    u32 dindirect;      /* двойной косвенный: блок ссылок на блоки ссылок (было reserved = 0) */
} __attribute__((packed));                      /* ровно 64 байта */

struct dirent {
    u32  ino;
    char name[FS_NAME_MAX + 1];
} __attribute__((packed));                      /* 32 байта, 16 в блоке */

_Static_assert(sizeof(struct inode) == 64, "inode must be 64 bytes");
_Static_assert(sizeof(struct dirent) == 32, "dirent must be 32 bytes");
_Static_assert(sizeof(struct superblock) == BS, "superblock must be one block");

static struct superblock sb;
static u8   bitmap[MAX_BLOCKS / 8];
static bool mounted;
static u32  free_blocks, free_inodes;

/* ---------------------------------------------------------------- блоки */

static int bread(u32 b, void *buf)        { return ata_read(b, buf)  ? FS_EIO : FS_OK; }
static int bwrite(u32 b, const void *buf) { return ata_write(b, buf) ? FS_EIO : FS_OK; }

static bool bit_get(u32 b) { return bitmap[b / 8] & (1 << (b % 8)); }
static void bit_set(u32 b, bool v) {
    if (v) bitmap[b / 8] |=  (1 << (b % 8));
    else   bitmap[b / 8] &= ~(1 << (b % 8));
}

/* записать на диск тот сектор битмапа, где лежит бит блока b */
static int bitmap_sync(u32 b) {
    u32 sec = b / BITS_PER_BLK;
    return bwrite(sb.bitmap_start + sec, bitmap + sec * BS);
}

static u32 balloc(void) {
    static u8 zero[BS];
    for (u32 b = sb.data_start; b < sb.total_blocks; b++) {
        if (!bit_get(b)) {
            bit_set(b, true);
            if (bitmap_sync(b) || bwrite(b, zero)) { bit_set(b, false); return 0; }
            free_blocks--;
            return b;
        }
    }
    return 0;  /* места нет */
}

static void bfree(u32 b) {
    if (b < sb.data_start || b >= sb.total_blocks || !bit_get(b)) return;
    bit_set(b, false);
    bitmap_sync(b);
    free_blocks++;
}

/* ---------------------------------------------------------------- inode */

static int iread(u32 ino, struct inode *out) {
    static struct inode blk[INODES_PER_BLK];
    if (ino == 0 || ino >= sb.inode_count) return FS_EINVAL;
    if (bread(sb.inode_start + ino / INODES_PER_BLK, blk)) return FS_EIO;
    *out = blk[ino % INODES_PER_BLK];
    return FS_OK;
}

static int iwrite(u32 ino, const struct inode *in) {
    static struct inode blk[INODES_PER_BLK];
    u32 b = sb.inode_start + ino / INODES_PER_BLK;
    if (ino == 0 || ino >= sb.inode_count) return FS_EINVAL;
    if (bread(b, blk)) return FS_EIO;
    blk[ino % INODES_PER_BLK] = *in;
    return bwrite(b, blk);
}

static u32 ialloc(int type) {
    struct inode in;
    for (u32 i = FS_ROOT; i < sb.inode_count; i++) {
        if (iread(i, &in)) return 0;
        if (in.type == FS_TYPE_FREE) {
            memset(&in, 0, sizeof(in));
            in.type = type;
            in.links = 1;
            if (iwrite(i, &in)) return 0;
            free_inodes--;
            return i;
        }
    }
    return 0;
}

/* Номер дискового блока для idx-го блока файла. alloc=true — выделить, если нет.
 * Возвращает 0, если блока нет (или не удалось выделить). */
static u32 bmap(struct inode *in, u32 idx, bool alloc) {
    static u32 ind[NINDIRECT];
    if (idx < NDIRECT) {
        if (!in->direct[idx] && alloc) in->direct[idx] = balloc();
        return in->direct[idx];
    }
    idx -= NDIRECT;
    if (idx >= NINDIRECT) {                  /* двойной косвенный: idx / 128 — какой блок ссылок, idx % 128 — где в нём */
        static u32 top[NINDIRECT];
        idx -= NINDIRECT;
        if (idx >= NDINDIRECT) return 0;
        if (!in->dindirect && (!alloc || !(in->dindirect = balloc()))) return 0;
        if (bread(in->dindirect, top)) return 0;
        u32 *mid = &top[idx / NINDIRECT];
        if (!*mid) {
            if (!alloc || !(*mid = balloc())) return 0;
            memset(ind, 0, sizeof(ind));
            if (bwrite(*mid, ind) || bwrite(in->dindirect, top)) return 0;
        }
        u32 m = *mid;
        if (bread(m, ind)) return 0;
        if (!ind[idx % NINDIRECT] && alloc) {
            if (!(ind[idx % NINDIRECT] = balloc())) return 0;
            if (bwrite(m, ind)) return 0;
        }
        return ind[idx % NINDIRECT];
    }
    if (!in->indirect) {
        if (!alloc || !(in->indirect = balloc())) return 0;
    }
    if (bread(in->indirect, ind)) return 0;
    if (!ind[idx] && alloc) {
        if (!(ind[idx] = balloc())) return 0;
        if (bwrite(in->indirect, ind)) return 0;
    }
    return ind[idx];
}

static void free_all_blocks(struct inode *in) {
    static u32 ind[NINDIRECT];
    for (int i = 0; i < NDIRECT; i++) { bfree(in->direct[i]); in->direct[i] = 0; }
    if (in->indirect) {
        if (!bread(in->indirect, ind))
            for (u32 i = 0; i < NINDIRECT; i++) bfree(ind[i]);
        bfree(in->indirect);
        in->indirect = 0;
    }
    if (in->dindirect) {
        static u32 top[NINDIRECT];
        if (!bread(in->dindirect, top))
            for (u32 j = 0; j < NINDIRECT; j++) {
                if (!top[j]) continue;
                if (!bread(top[j], ind))
                    for (u32 i = 0; i < NINDIRECT; i++) bfree(ind[i]);
                bfree(top[j]);
            }
        bfree(in->dindirect);
        in->dindirect = 0;
    }
    in->size = 0;
}

/* ---------------------------------------------------------------- чтение/запись */

static int fs_read_impl(u32 ino, u32 off, void *buf, u32 n) {
    static u8 blk[BS];
    struct inode in;
    int r = iread(ino, &in);
    if (r) return r;
    if (off >= in.size) return 0;
    if (n > in.size - off) n = in.size - off;

    u8 *dst = buf;
    u32 done = 0;
    while (done < n) {
        u32 pos = off + done, bo = pos % BS;
        u32 chunk = BS - bo;
        if (chunk > n - done) chunk = n - done;
        u32 b = bmap(&in, pos / BS, false);
        if (b) { if (bread(b, blk)) return FS_EIO; memcpy(dst + done, blk + bo, chunk); }
        else   memset(dst + done, 0, chunk);    /* «дырка» в файле */
        done += chunk;
    }
    return done;
}

static int fs_write_impl(u32 ino, u32 off, const void *buf, u32 n) {
    static u8 blk[BS];
    struct inode in;
    int r = iread(ino, &in);
    if (r) return r;
    if (off + n > MAX_FILE_BLKS * BS) return FS_EFBIG;

    const u8 *src = buf;
    u32 done = 0;
    while (done < n) {
        u32 pos = off + done, bo = pos % BS;
        u32 chunk = BS - bo;
        if (chunk > n - done) chunk = n - done;
        u32 b = bmap(&in, pos / BS, true);
        if (!b) { r = FS_ENOSPC; break; }
        if (chunk < BS && bread(b, blk)) { r = FS_EIO; break; }
        memcpy(blk + bo, src + done, chunk);
        if (bwrite(b, blk)) { r = FS_EIO; break; }
        done += chunk;
    }
    if (off + done > in.size) in.size = off + done;
    int w = iwrite(ino, &in);
    if (r) return r;
    return w ? w : (int)done;
}

static int fs_truncate_impl(u32 ino) {
    struct inode in;
    int r = iread(ino, &in);
    if (r) return r;
    if (in.type != FS_TYPE_FILE) return FS_EISDIR;
    free_all_blocks(&in);
    return iwrite(ino, &in);
}

static int fs_stat_impl(u32 ino, struct fs_stat *st) {
    struct inode in;
    int r = iread(ino, &in);
    if (r) return r;
    if (in.type == FS_TYPE_FREE) return FS_ENOENT;
    st->type = in.type;
    st->size = in.size;
    u32 data = (in.size + BS - 1) / BS;
    st->blocks = data + (in.indirect ? 1 : 0);
    if (data > NDIRECT + NINDIRECT)          /* блоки ссылок двойного косвенного */
        st->blocks += 1 + (data - NDIRECT - NINDIRECT + NINDIRECT - 1) / NINDIRECT;
    return FS_OK;
}

/* ---------------------------------------------------------------- каталоги */

static int dir_check(u32 dir) {
    struct inode in;
    int r = iread(dir, &in);
    if (r) return r;
    if (in.type == FS_TYPE_FREE) return FS_ENOENT;
    return in.type == FS_TYPE_DIR ? FS_OK : FS_ENOTDIR;
}

/* найти запись name; вернуть inode (0 — нет) и смещение записи */
static u32 dir_lookup(u32 dir, const char *name, u32 *offset) {
    struct dirent de;
    for (u32 off = 0; fs_read(dir, off, &de, sizeof(de)) == sizeof(de); off += sizeof(de)) {
        if (de.ino && !strcmp(de.name, name)) {
            if (offset) *offset = off;
            return de.ino;
        }
    }
    return 0;
}

static int dir_add(u32 dir, const char *name, u32 ino) {
    struct dirent de;
    u32 off = 0;
    while (fs_read(dir, off, &de, sizeof(de)) == sizeof(de) && de.ino) off += sizeof(de);
    memset(&de, 0, sizeof(de));
    de.ino = ino;
    memcpy(de.name, name, strlen(name) + 1);
    int r = fs_write(dir, off, &de, sizeof(de));
    return r < 0 ? r : FS_OK;
}

static int dir_set(u32 dir, u32 off, u32 ino, const char *name) {
    struct dirent de;
    memset(&de, 0, sizeof(de));
    de.ino = ino;
    if (name) memcpy(de.name, name, strlen(name) + 1);
    int r = fs_write(dir, off, &de, sizeof(de));
    return r < 0 ? r : FS_OK;
}

static bool dir_empty(u32 dir) {
    struct dirent de;
    for (u32 off = 0; fs_read(dir, off, &de, sizeof(de)) == sizeof(de); off += sizeof(de))
        if (de.ino && strcmp(de.name, ".") && strcmp(de.name, "..")) return false;
    return true;
}

static int fs_readdir_impl(u32 dir, u32 *pos, struct fs_dirent *out) {
    struct dirent de;
    int r = dir_check(dir);
    if (r) return r;
    while (fs_read(dir, *pos, &de, sizeof(de)) == sizeof(de)) {
        *pos += sizeof(de);
        if (de.ino && strcmp(de.name, ".") && strcmp(de.name, "..")) {
            out->ino = de.ino;
            memcpy(out->name, de.name, FS_NAME_MAX + 1);
            return 1;
        }
    }
    return 0;
}

/* ---------------------------------------------------------------- пути */

/* Разбор пути: "/a/b", "a/b", "../x", "." — относительно cwd */
static int fs_resolve_impl(u32 cwd, const char *path, u32 *ino) {
    if (!mounted) return FS_ENODEV;
    u32 cur = (path[0] == '/') ? FS_ROOT : cwd;
    char name[FS_NAME_MAX + 1];

    while (*path) {
        while (*path == '/') path++;
        if (!*path) break;
        u32 len = 0;
        while (path[len] && path[len] != '/') len++;
        if (len > FS_NAME_MAX) return FS_ENAMETOOLONG;
        memcpy(name, path, len);
        name[len] = 0;
        path += len;

        int r = dir_check(cur);
        if (r) return r;
        u32 next = dir_lookup(cur, name, NULL);
        if (!next) return FS_ENOENT;
        cur = next;
    }
    *ino = cur;
    return FS_OK;
}

/* Разделить путь на родительский каталог и последнее имя */
static int resolve_parent(u32 cwd, const char *path, u32 *parent, char *name) {
    char buf[128];
    u32 len = strlen(path);
    if (len == 0 || len >= sizeof(buf)) return FS_EINVAL;
    memcpy(buf, path, len + 1);
    while (len > 1 && buf[len - 1] == '/') buf[--len] = 0;   /* хвостовые '/' */

    int slash = -1;
    for (int i = len - 1; i >= 0; i--) if (buf[i] == '/') { slash = i; break; }

    const char *base = buf + slash + 1;
    if (!*base || !strcmp(base, ".") || !strcmp(base, "..")) return FS_EINVAL;
    if (strlen(base) > FS_NAME_MAX) return FS_ENAMETOOLONG;
    memcpy(name, base, strlen(base) + 1);

    int r;
    if (slash < 0)       { *parent = cwd; r = FS_OK; }
    else if (slash == 0) { *parent = FS_ROOT; r = FS_OK; }
    else                 { buf[slash] = 0; r = fs_resolve(cwd, buf, parent); }
    if (r) return r;
    return dir_check(*parent);
}

static int fs_create_impl(u32 cwd, const char *path, int type, u32 *out) {
    if (!mounted) return FS_ENODEV;
    u32 parent;
    char name[FS_NAME_MAX + 1];
    int r = resolve_parent(cwd, path, &parent, name);
    if (r) return r;
    if (dir_lookup(parent, name, NULL)) return FS_EEXIST;

    u32 ino = ialloc(type);
    if (!ino) return FS_ENOSPC;
    if (type == FS_TYPE_DIR) {
        if ((r = dir_add(ino, ".", ino)) || (r = dir_add(ino, "..", parent))) return r;
    }
    if ((r = dir_add(parent, name, ino))) return r;
    if (out) *out = ino;
    return FS_OK;
}

static int fs_remove_impl(u32 cwd, const char *path) {
    if (!mounted) return FS_ENODEV;
    u32 parent, off;
    char name[FS_NAME_MAX + 1];
    int r = resolve_parent(cwd, path, &parent, name);
    if (r) return r;
    u32 ino = dir_lookup(parent, name, &off);
    if (!ino) return FS_ENOENT;

    struct inode in;
    if ((r = iread(ino, &in))) return r;
    if (in.type == FS_TYPE_DIR && !dir_empty(ino)) return FS_ENOTEMPTY;

    free_all_blocks(&in);
    in.type = FS_TYPE_FREE;
    in.links = 0;
    if ((r = iwrite(ino, &in))) return r;
    free_inodes++;
    return dir_set(parent, off, 0, NULL);
}

static int fs_rename_impl(u32 cwd, const char *src, const char *dst) {
    if (!mounted) return FS_ENODEV;
    u32 sparent, soff, dparent, existing;
    char sname[FS_NAME_MAX + 1], dname[FS_NAME_MAX + 1];
    int r = resolve_parent(cwd, src, &sparent, sname);
    if (r) return r;
    u32 ino = dir_lookup(sparent, sname, &soff);
    if (!ino) return FS_ENOENT;

    /* mv file dir  ->  dir/file */
    if (fs_resolve(cwd, dst, &existing) == FS_OK && dir_check(existing) == FS_OK) {
        dparent = existing;
        memcpy(dname, sname, sizeof(sname));
    } else if ((r = resolve_parent(cwd, dst, &dparent, dname))) {
        return r;
    }
    if (dir_lookup(dparent, dname, NULL)) return FS_EEXIST;

    /* нельзя переместить каталог внутрь самого себя */
    for (u32 p = dparent; ; p = dir_lookup(p, "..", NULL)) {
        if (p == ino) return FS_EINVAL;
        if (p == FS_ROOT || p == 0) break;
    }

    if ((r = dir_add(dparent, dname, ino))) return r;
    if ((r = dir_set(sparent, soff, 0, NULL))) return r;

    u32 dotdot;
    if (dir_check(ino) == FS_OK && dparent != sparent && dir_lookup(ino, "..", &dotdot))
        return dir_set(ino, dotdot, dparent, "..");
    return FS_OK;
}

static int fs_getpath_impl(u32 ino, char *buf, u32 len) {
    u32 chain[32];
    int depth = 0;
    while (ino != FS_ROOT) {
        if (depth == 32) return FS_EINVAL;
        chain[depth++] = ino;
        ino = dir_lookup(ino, "..", NULL);
        if (!ino) return FS_ENOENT;
    }
    u32 pos = 0;
    buf[pos++] = '/';
    for (int i = depth - 1; i >= 0; i--) {
        u32 parent = (i == depth - 1) ? FS_ROOT : chain[i + 1];
        struct fs_dirent de;
        u32 p = 0;
        bool found = false;
        while (fs_readdir(parent, &p, &de) == 1) if (de.ino == chain[i]) { found = true; break; }
        if (!found) return FS_ENOENT;
        u32 n = strlen(de.name);
        if (pos + n + 2 > len) return FS_ENAMETOOLONG;
        memcpy(buf + pos, de.name, n);
        pos += n;
        if (i) buf[pos++] = '/';
    }
    buf[pos] = 0;
    return FS_OK;
}

/* ---------------------------------------------------------------- монтирование */

static int fs_format_impl(void) {
    static u8 zero[BS];
    u32 total = ata_sectors();
    if (!total) return FS_ENODEV;
    if (total > MAX_BLOCKS) total = MAX_BLOCKS;

    memset(&sb, 0, sizeof(sb));
    sb.magic         = MAGIC;
    sb.version       = VERSION;
    sb.total_blocks  = total;
    sb.bitmap_start  = 1;
    sb.bitmap_blocks = (total + BITS_PER_BLK - 1) / BITS_PER_BLK;
    sb.inode_start   = sb.bitmap_start + sb.bitmap_blocks;
    sb.inode_count   = INODE_COUNT;
    sb.inode_blocks  = INODE_COUNT / INODES_PER_BLK;
    sb.data_start    = sb.inode_start + sb.inode_blocks;
    if (sb.data_start + 16 > total) return FS_ENOSPC;

    /* битмап: служебные блоки заняты */
    memset(bitmap, 0, sizeof(bitmap));
    for (u32 b = 0; b < sb.data_start; b++) bit_set(b, true);
    for (u32 i = 0; i < sb.bitmap_blocks; i++)
        if (bwrite(sb.bitmap_start + i, bitmap + i * BS)) return FS_EIO;
    for (u32 i = 0; i < sb.inode_blocks; i++)
        if (bwrite(sb.inode_start + i, zero)) return FS_EIO;
    if (bwrite(0, &sb)) return FS_EIO;

    free_blocks = total - sb.data_start;
    free_inodes = INODE_COUNT - 1;
    mounted = true;

    /* корневой каталог: inode 1, «..» указывает на себя */
    struct inode root;
    memset(&root, 0, sizeof(root));
    root.type = FS_TYPE_DIR;
    root.links = 1;
    if (iwrite(FS_ROOT, &root)) return FS_EIO;
    free_inodes--;
    int r;
    if ((r = dir_add(FS_ROOT, ".", FS_ROOT)) || (r = dir_add(FS_ROOT, "..", FS_ROOT))) return r;
    return FS_OK;
}

static int fs_init_impl(bool *formatted) {
    *formatted = false;
    mounted = false;
    if (!ata_sectors()) return FS_ENODEV;
    if (bread(0, &sb)) return FS_EIO;

    if (sb.magic != MAGIC || sb.version != VERSION || sb.total_blocks > MAX_BLOCKS) {
        *formatted = true;
        return fs_format();
    }

    for (u32 i = 0; i < sb.bitmap_blocks; i++)
        if (bread(sb.bitmap_start + i, bitmap + i * BS)) return FS_EIO;
    mounted = true;

    free_blocks = 0;
    for (u32 b = sb.data_start; b < sb.total_blocks; b++) if (!bit_get(b)) free_blocks++;
    free_inodes = 0;
    struct inode in;
    for (u32 i = FS_ROOT; i < sb.inode_count; i++)
        if (!iread(i, &in) && in.type == FS_TYPE_FREE) free_inodes++;
    return FS_OK;
}

bool fs_ready(void) { return mounted; }

static void fs_info_impl(struct fs_info *info) {
    info->total_blocks = sb.total_blocks - sb.data_start;
    info->free_blocks  = free_blocks;
    info->total_inodes = sb.inode_count - 1;
    info->free_inodes  = free_inodes;
}

const char *fs_strerror(int err) {
    switch (err) {
    case FS_OK:           return "success";
    case FS_ENOENT:       return "no such file or directory";
    case FS_EEXIST:       return "already exists";
    case FS_ENOTDIR:      return "not a directory";
    case FS_EISDIR:       return "is a directory";
    case FS_ENOSPC:       return "no space left on disk";
    case FS_ENOTEMPTY:    return "directory not empty";
    case FS_EINVAL:       return "invalid argument";
    case FS_EIO:          return "disk I/O error";
    case FS_ENODEV:       return "no disk / file system not mounted";
    case FS_EFBIG:        return "file too large (max 8 MB)";
    case FS_ENAMETOOLONG: return "name too long (max 27 chars)";
    default:              return "unknown error";
    }
}

/* ---------------------------------------------------------------- блокировка
 * Файловая система общая для всех задач, а операции с ней долгие (диск!),
 * поэтому вместо запрета прерываний — мьютекс: ждущая задача спит, а не крутится.
 * Мьютекс рекурсивный: fs_create внутри себя вызывает fs_write и т.п. */
static struct mutex fs_lock;
int fs_init(bool *formatted) { mutex_lock(&fs_lock); int r = fs_init_impl(formatted); mutex_unlock(&fs_lock); return r; }
int fs_format(void) { mutex_lock(&fs_lock); int r = fs_format_impl(); mutex_unlock(&fs_lock); return r; }
int fs_resolve(u32 cwd, const char *path, u32 *ino) { mutex_lock(&fs_lock); int r = fs_resolve_impl(cwd, path, ino); mutex_unlock(&fs_lock); return r; }
int fs_create(u32 cwd, const char *path, int type, u32 *out) { mutex_lock(&fs_lock); int r = fs_create_impl(cwd, path, type, out); mutex_unlock(&fs_lock); return r; }
int fs_remove(u32 cwd, const char *path) { mutex_lock(&fs_lock); int r = fs_remove_impl(cwd, path); mutex_unlock(&fs_lock); return r; }
int fs_rename(u32 cwd, const char *src, const char *dst) { mutex_lock(&fs_lock); int r = fs_rename_impl(cwd, src, dst); mutex_unlock(&fs_lock); return r; }
int fs_read(u32 ino, u32 off, void *buf, u32 n) { mutex_lock(&fs_lock); int r = fs_read_impl(ino, off, buf, n); mutex_unlock(&fs_lock); return r; }
int fs_write(u32 ino, u32 off, const void *buf, u32 n) { mutex_lock(&fs_lock); int r = fs_write_impl(ino, off, buf, n); mutex_unlock(&fs_lock); return r; }
int fs_truncate(u32 ino) { mutex_lock(&fs_lock); int r = fs_truncate_impl(ino); mutex_unlock(&fs_lock); return r; }
int fs_stat(u32 ino, struct fs_stat *st) { mutex_lock(&fs_lock); int r = fs_stat_impl(ino, st); mutex_unlock(&fs_lock); return r; }
int fs_readdir(u32 dir, u32 *pos, struct fs_dirent *out) { mutex_lock(&fs_lock); int r = fs_readdir_impl(dir, pos, out); mutex_unlock(&fs_lock); return r; }
int fs_getpath(u32 ino, char *buf, u32 len) { mutex_lock(&fs_lock); int r = fs_getpath_impl(ino, buf, len); mutex_unlock(&fs_lock); return r; }
void fs_info(struct fs_info *info) { mutex_lock(&fs_lock); fs_info_impl(info); mutex_unlock(&fs_lock); }
