#ifndef BLOCK_H
#define BLOCK_H
#include "types.h"
#define BLOCK_MAX 10
struct block_device { char name[12]; u32 start, sectors; bool partition; };
int block_init(void);
int block_count(void);
int block_at(int index); /* enumeration -> stable device ID */
bool block_disk_partitioned(int disk);
int block_rescan(int disk);
const struct block_device *block_get(int id);
int block_find(const char *name);
int block_read(int id, u32 sector, void *buf);
/* Raw device writes are deliberately not exposed through /dev. */
int block_write(int id, u32 sector, const void *buf);
bool block_partitioned(void);
#endif
