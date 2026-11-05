#ifndef FS_H
#define FS_H
#include "types.h"

/* MyFS — простая файловая система в духе классической Unix FS.
 *
 * Раскладка диска (блок = 512 байт = 1 сектор):
 *   [0]            суперблок
 *   [1..B]         битовая карта занятых блоков (1 бит на блок)
 *   [B+1..B+I]     таблица inode (по 8 штук на блок)
 *   [...]          блоки данных
 *
 * inode 0 не используется (0 = «нет inode»), корневой каталог — inode 1. */

#define FS_ROOT       1
#define FS_NAME_MAX   27

enum { FS_TYPE_FREE = 0, FS_TYPE_FILE = 1, FS_TYPE_DIR = 2 };

enum {
    FS_OK        =  0,
    FS_ENOENT    = -1,
    FS_EEXIST    = -2,
    FS_ENOTDIR   = -3,
    FS_EISDIR    = -4,
    FS_ENOSPC    = -5,
    FS_ENOTEMPTY = -6,
    FS_EINVAL    = -7,
    FS_EIO       = -8,
    FS_ENODEV    = -9,
    FS_EFBIG     = -10,
    FS_ENAMETOOLONG = -11,
};

struct fs_stat {
    u32 type;
    u32 size;
    u32 blocks;
};

struct fs_dirent {
    u32  ino;
    char name[FS_NAME_MAX + 1];
};

struct fs_info {
    u32 total_blocks, free_blocks;
    u32 total_inodes, free_inodes;
};

int  fs_init(bool *formatted);   /* смонтировать, при отсутствии ФС — отформатировать */
int  fs_format(void);
bool fs_ready(void);

int  fs_resolve(u32 cwd, const char *path, u32 *ino);
int  fs_create(u32 cwd, const char *path, int type, u32 *ino);
int  fs_remove(u32 cwd, const char *path);
int  fs_rename(u32 cwd, const char *src, const char *dst);

int  fs_read(u32 ino, u32 off, void *buf, u32 n);         /* -> прочитано байт */
int  fs_write(u32 ino, u32 off, const void *buf, u32 n);  /* -> записано байт */
int  fs_truncate(u32 ino);
int  fs_stat(u32 ino, struct fs_stat *st);

int  fs_readdir(u32 dir, u32 *pos, struct fs_dirent *out); /* 1 = есть запись, 0 = конец */
int  fs_getpath(u32 ino, char *buf, u32 len);
void fs_info(struct fs_info *info);

const char *fs_strerror(int err);

#endif
