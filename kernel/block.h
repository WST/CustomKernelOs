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
/* Raw device writes are deliberately not exposed through /dev.
 * block_write may return before the device has the sector: file data stays in
 * the block cache until close, idle, cache pressure, or block_flush. A power
 * loss drops whatever has not been flushed. Metadata uses block_write_through. */
int block_write(int id, u32 sector, const void *buf);
int block_write_through(int id, u32 sector, const void *buf);
int block_flush(void);
void block_flush_idle(void); /* не ждёт, если диск уже занят ожидающей задачей */
void block_cache_invalidate(void); /* drop cached sectors; dirty ones are discarded */
bool block_partitioned(void);
#ifdef HOST_TEST
void block_io_reset(void);
void block_io_stats(unsigned *reads, unsigned *writes);
#endif
#endif
