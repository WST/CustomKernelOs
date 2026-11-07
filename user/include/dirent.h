#ifndef DIRENT_H
#define DIRENT_H

#define DT_UNKNOWN 0
#define DT_CHR     2
#define DT_DIR     4
#define DT_REG     8
#define DT_BLK     6

struct dirent {
    unsigned d_ino;
    unsigned char d_type;
    char d_name[256];
};

typedef struct DIR DIR;

DIR *opendir(const char *name);
struct dirent *readdir(DIR *dir);
int closedir(DIR *dir);
#endif
