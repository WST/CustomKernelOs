/* MyFS: реализация. Описание формата — в fs.h. */
#include "myfs.h"
#include "block.h"
#include "string.h"
#include "task.h"
#ifdef HOST_TEST
void *malloc(unsigned long); void free(void *);
#define bitmap_alloc(n) malloc(n)
#define bitmap_free(p) free(p)
#else
#include "heap.h"
#define bitmap_alloc(n) kmalloc(n)
#define bitmap_free(p) kfree(p)
#endif

#define BS            512                       /* размер блока */
#define MAGIC         0x5346594D                /* "MYFS" в little-endian */
#define VERSION       1
#define NDIRECT       12
#define NINDIRECT     (BS / 4)                  /* 128 ссылок в косвенном блоке */
#define NDINDIRECT    (NINDIRECT * NINDIRECT)  /* 16384 блока через двойной косвенный */
#define MAX_FILE_BLKS (NDIRECT + NINDIRECT + NDINDIRECT)
#define INODES_PER_BLK (BS / sizeof(struct inode))
#define BITS_PER_BLK  (BS * 8)
#define MAX_BLOCKS    2097152u                  /* 1 GiB, 512-byte sectors */
#define MIN_INODES    256u
#define MAX_INODES    16384u

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

struct fs_state { struct superblock super; u8 *bits; u32 bitmap_bytes, hint; bool active; u32 nblocks, ninodes; int device; };
static struct fs_state states[5];
static int selected;
void myfs_select(int slot, int device) { selected = slot; states[slot].device = device; }
#define sb (states[selected].super)
#define bitmap (states[selected].bits)
static int prepare_bitmap(u32 bytes) {
    if (states[selected].bitmap_bytes >= bytes) return 0;
    u8 *p=bitmap_alloc(bytes); if(!p) return FS_ENOSPC;
    bitmap_free(bitmap); bitmap=p; states[selected].bitmap_bytes=bytes; return 0;
}
#define mounted (states[selected].active)
#define available_blocks (states[selected].nblocks)
#define available_inodes (states[selected].ninodes)

/* ---------------------------------------------------------------- блоки */

static int bread(u32 b, void *buf)        { return block_read(states[selected].device, b, buf)  ? FS_EIO : FS_OK; }
static int bwrite(u32 b, const void *buf) { return block_write(states[selected].device, b, buf) ? FS_EIO : FS_OK; }

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
    u32 start=states[selected].hint;
    if(start<sb.data_start || start>=sb.total_blocks) start=sb.data_start;
    for (u32 n=0; n<sb.total_blocks-sb.data_start; n++) {
        u32 b=start+n; if(b>=sb.total_blocks) b=sb.data_start+(b-sb.total_blocks);
        if (!bit_get(b)) {
            bit_set(b, true);
            if (bitmap_sync(b) || bwrite(b, zero)) { bit_set(b, false); return 0; }
            states[selected].hint=b+1;
            available_blocks--;
            return b;
        }
    }
    return 0;  /* места нет */
}

static void bfree(u32 b) {
    if (b < sb.data_start || b >= sb.total_blocks || !bit_get(b)) return;
    bit_set(b, false);
    if(b<states[selected].hint) states[selected].hint=b;
    bitmap_sync(b);
    available_blocks++;
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
            available_inodes--;
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

static int myfs_read_impl(u32 ino, u32 off, void *buf, u32 n) {
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

static int myfs_write_impl(u32 ino, u32 off, const void *buf, u32 n) {
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

static int myfs_truncate_impl(u32 ino) {
    struct inode in;
    int r = iread(ino, &in);
    if (r) return r;
    if (in.type != FS_TYPE_FILE) return FS_EISDIR;
    free_all_blocks(&in);
    return iwrite(ino, &in);
}

static int myfs_stat_impl(u32 ino, struct fs_stat *st) {
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
    for (u32 off = 0; myfs_read(dir, off, &de, sizeof(de)) == sizeof(de); off += sizeof(de)) {
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
    while (myfs_read(dir, off, &de, sizeof(de)) == sizeof(de) && de.ino) off += sizeof(de);
    memset(&de, 0, sizeof(de));
    de.ino = ino;
    memcpy(de.name, name, strlen(name) + 1);
    int r = myfs_write(dir, off, &de, sizeof(de));
    return r < 0 ? r : FS_OK;
}

static int dir_set(u32 dir, u32 off, u32 ino, const char *name) {
    struct dirent de;
    memset(&de, 0, sizeof(de));
    de.ino = ino;
    if (name) memcpy(de.name, name, strlen(name) + 1);
    int r = myfs_write(dir, off, &de, sizeof(de));
    return r < 0 ? r : FS_OK;
}

static bool dir_empty(u32 dir) {
    struct dirent de;
    for (u32 off = 0; myfs_read(dir, off, &de, sizeof(de)) == sizeof(de); off += sizeof(de))
        if (de.ino && strcmp(de.name, ".") && strcmp(de.name, "..")) return false;
    return true;
}

static int myfs_readdir_impl(u32 dir, u32 *pos, struct fs_dirent *out) {
    struct dirent de;
    int r = dir_check(dir);
    if (r) return r;
    while (myfs_read(dir, *pos, &de, sizeof(de)) == sizeof(de)) {
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
static int myfs_resolve_impl(u32 cwd, const char *path, u32 *ino) {
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
    else                 { buf[slash] = 0; r = myfs_resolve(cwd, buf, parent); }
    if (r) return r;
    return dir_check(*parent);
}

static int myfs_create_impl(u32 cwd, const char *path, int type, u32 *out) {
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

static int myfs_remove_impl(u32 cwd, const char *path) {
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
    available_inodes++;
    return dir_set(parent, off, 0, NULL);
}

static int myfs_rename_impl(u32 cwd, const char *src, const char *dst) {
    if (!mounted) return FS_ENODEV;
    u32 sparent, soff, dparent, existing;
    char sname[FS_NAME_MAX + 1], dname[FS_NAME_MAX + 1];
    int r = resolve_parent(cwd, src, &sparent, sname);
    if (r) return r;
    u32 ino = dir_lookup(sparent, sname, &soff);
    if (!ino) return FS_ENOENT;

    /* mv file dir  ->  dir/file */
    if (myfs_resolve(cwd, dst, &existing) == FS_OK && dir_check(existing) == FS_OK) {
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

static int myfs_getpath_impl(u32 ino, char *buf, u32 len) {
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
        while (myfs_readdir(parent, &p, &de) == 1) if (de.ino == chain[i]) { found = true; break; }
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

static int myfs_format_impl(void) {
    static u8 zero[BS];
    u32 total = block_get(states[selected].device)->sectors;
    if (!total) return FS_ENODEV;
    if (total > MAX_BLOCKS) return FS_EFBIG;
    int allocation=prepare_bitmap(((total + BITS_PER_BLK - 1) / BITS_PER_BLK)*BS);
    if(allocation) return allocation;

    memset(&sb, 0, sizeof(sb));
    sb.magic         = MAGIC;
    sb.version       = VERSION;
    sb.total_blocks  = total;
    sb.bitmap_start  = 1;
    sb.bitmap_blocks = (total + BITS_PER_BLK - 1) / BITS_PER_BLK;
    sb.inode_start   = sb.bitmap_start + sb.bitmap_blocks;
    sb.inode_count   = total / 128;
    if(sb.inode_count < MIN_INODES) sb.inode_count=MIN_INODES;
    if(sb.inode_count > MAX_INODES) sb.inode_count=MAX_INODES;
    sb.inode_count = (sb.inode_count + INODES_PER_BLK - 1) / INODES_PER_BLK * INODES_PER_BLK;
    sb.inode_blocks  = sb.inode_count / INODES_PER_BLK;
    sb.data_start    = sb.inode_start + sb.inode_blocks;
    if (sb.data_start + 16 > total) return FS_ENOSPC;

    /* битмап: служебные блоки заняты */
    memset(bitmap, 0, sb.bitmap_blocks * BS);
    for (u32 b = 0; b < sb.data_start; b++) bit_set(b, true);
    for (u32 i = 0; i < sb.bitmap_blocks; i++)
        if (bwrite(sb.bitmap_start + i, bitmap + i * BS)) return FS_EIO;
    for (u32 i = 0; i < sb.inode_blocks; i++)
        if (bwrite(sb.inode_start + i, zero)) return FS_EIO;
    if (bwrite(0, &sb)) return FS_EIO;

    states[selected].hint=sb.data_start;
    available_blocks = total - sb.data_start;
    available_inodes = sb.inode_count - 1;
    mounted = true;

    /* корневой каталог: inode 1, «..» указывает на себя */
    struct inode root;
    memset(&root, 0, sizeof(root));
    root.type = FS_TYPE_DIR;
    root.links = 1;
    if (iwrite(FS_ROOT, &root)) return FS_EIO;
    available_inodes--;
    int r;
    if ((r = dir_add(FS_ROOT, ".", FS_ROOT)) || (r = dir_add(FS_ROOT, "..", FS_ROOT))) return r;
    return FS_OK;
}

static int myfs_init_impl(bool *formatted, bool allow_format) {
    *formatted = false;
    mounted = false;
    if (!block_get(states[selected].device)->sectors) return FS_ENODEV;
    if (bread(0, &sb)) return FS_EIO;

    if (sb.magic != MAGIC) {
        if (!allow_format) return FS_EINVAL;
        const u8 *raw = (const u8 *)&sb;
        for (u32 i=0; i<BS; i++) if (raw[i]) return FS_EINVAL;
        *formatted = true;
        return myfs_format();
    }

    if (sb.version != VERSION || sb.total_blocks > MAX_BLOCKS || sb.total_blocks > block_get(states[selected].device)->sectors ||
        sb.bitmap_start != 1 || sb.bitmap_blocks != (sb.total_blocks + BITS_PER_BLK - 1) / BITS_PER_BLK ||
        sb.inode_start != 1 + sb.bitmap_blocks || (sb.inode_count < MIN_INODES || sb.inode_count > MAX_INODES || sb.inode_count % INODES_PER_BLK) ||
        sb.inode_blocks != sb.inode_count / INODES_PER_BLK || sb.data_start != sb.inode_start + sb.inode_blocks ||
        sb.data_start >= sb.total_blocks) return FS_EINVAL;
    int allocation=prepare_bitmap(sb.bitmap_blocks * BS);
    if(allocation) return allocation;
    states[selected].hint=sb.data_start;
    for (u32 i = 0; i < sb.bitmap_blocks; i++)
        if (bread(sb.bitmap_start + i, bitmap + i * BS)) return FS_EIO;
    mounted = true;

    available_blocks = 0;
    for (u32 b = sb.data_start; b < sb.total_blocks; b++) if (!bit_get(b)) available_blocks++;
    available_inodes = 0;
    struct inode in;
    for (u32 i = FS_ROOT; i < sb.inode_count; i++)
        if (!iread(i, &in) && in.type == FS_TYPE_FREE) available_inodes++;
    return FS_OK;
}

bool myfs_ready(void) { return mounted; }

static void myfs_info_impl(struct fs_info *info) {
    info->total_blocks = sb.total_blocks - sb.data_start;
    info->free_blocks  = available_blocks;
    info->total_inodes = sb.inode_count - 1;
    info->free_inodes  = available_inodes;
}

const char *myfs_strerror(int err) {
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
    case FS_EFBIG:        return "file or filesystem too large";
    case FS_ENAMETOOLONG: return "name too long (max 27 chars)";
    default:              return "unknown error";
    }
}

/* ---------------------------------------------------------------- блокировка
 * Файловая система общая для всех задач, а операции с ней долгие (диск!),
 * поэтому вместо запрета прерываний — мьютекс: ждущая задача спит, а не крутится.
 * Мьютекс рекурсивный: myfs_create внутри себя вызывает myfs_write и т.п. */
/* Private backend: all kernel access is serialized by the VFS lock. */
int myfs_init(bool *formatted, bool allow_format) { int r = myfs_init_impl(formatted, allow_format); return r; }
int myfs_format(void) { int r = myfs_format_impl(); return r; }
int myfs_resolve(u32 cwd, const char *path, u32 *ino) { int r = myfs_resolve_impl(cwd, path, ino); return r; }
int myfs_create(u32 cwd, const char *path, int type, u32 *out) { int r = myfs_create_impl(cwd, path, type, out); return r; }
int myfs_remove(u32 cwd, const char *path) { int r = myfs_remove_impl(cwd, path); return r; }
int myfs_rename(u32 cwd, const char *src, const char *dst) { int r = myfs_rename_impl(cwd, src, dst); return r; }
int myfs_read(u32 ino, u32 off, void *buf, u32 n) { int r = myfs_read_impl(ino, off, buf, n); return r; }
int myfs_write(u32 ino, u32 off, const void *buf, u32 n) { int r = myfs_write_impl(ino, off, buf, n); return r; }
int myfs_truncate(u32 ino) { int r = myfs_truncate_impl(ino); return r; }
int myfs_stat(u32 ino, struct fs_stat *st) { int r = myfs_stat_impl(ino, st); return r; }
int myfs_readdir(u32 dir, u32 *pos, struct fs_dirent *out) { int r = myfs_readdir_impl(dir, pos, out); return r; }
int myfs_getpath(u32 ino, char *buf, u32 len) { int r = myfs_getpath_impl(ino, buf, len); return r; }
void myfs_info(struct fs_info *info) { myfs_info_impl(info); }






