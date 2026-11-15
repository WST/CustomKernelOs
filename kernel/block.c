#include "block.h"
#include "ata.h"
#ifndef HOST_TEST
#include "ahci.h"
#include "pmm.h"
#include "paging.h"
#include "heap.h"
#include "vga.h"
#endif
#include "string.h"
#include "task.h"
#ifdef HOST_TEST
static unsigned io_reads, io_writes;
void block_io_reset(void) { io_reads = io_writes = 0; }
void block_io_stats(unsigned *reads, unsigned *writes) {
    if (reads) *reads = io_reads;
    if (writes) *writes = io_writes;
}
int atexit(void (*fn)(void));
static void host_exit_flush(void) { block_flush(); }
void *calloc(unsigned long, unsigned long);
#endif
/* Five stable IDs per disk: whole device, then four primary MBR slots.
 * Rescanning one disk never invalidates handles on the other disk.
 * A journal commit dirties the cache and then calls block_flush_device.
 * That flush writes consecutive sectors in one command and only then issues
 * the hardware barrier. File bytes stay cached until that barrier, idle,
 * pressure, or an explicit flush. Power loss drops whatever is still dirty. */
static struct block_device devices[BLOCK_MAX];
static bool tables[2];
static struct mutex block_lock;
#define CACHE_RUN 32
/* Пачка сброса и опережающего чтения. ATA PIO не берёт больше CACHE_RUN,
 * AHCI умеет AHCI_MAX_SECTORS и буфер рассчитан на этот потолок. */
#ifdef HOST_TEST
#define RUN_MAX CACHE_RUN
#else
#define RUN_MAX AHCI_MAX_SECTORS
#endif
#define SECTORS_PER_PAGE 8
#define F_VALID 1u
#define F_DIRTY 2u
#define F_REF   4u
#define F_LIST  8u
/* The boot stack occupies 0x9b000..0x9f000. The run buffer used to sit in BSS,
 * which the linker rejects. The heap is ready before the first disk command. */
struct cache_ent {
    u32 lba, next, dirty_next, dirty_prev;
    i16 unit;
    u16 flags;
    u32 stamp;
};
static u32 epoch;
static struct cache_ent *cache;
static u32 cache_slots, hash_mask, clock_hand, dirty_head;
static u32 *hash;
static u8 *run_buf, *flush_buf;
#ifndef HOST_TEST
static u32 *pages;
static bool cache_announced;
#else
static u8 *data_blob;
#endif
static int seq_unit;
static u32 seq_lba;
static bool seq_valid;
static bool device_dirty[2];

static u32 hash_of(int unit, u32 lba) {
    return (lba * 2654435761u ^ ((u32)unit * 0x9e3779b9u)) & hash_mask;
}
static u8 *slot_data(u32 i) {
#ifdef HOST_TEST
    return data_blob ? data_blob + i * 512 : NULL;
#else
    u32 phys = pages ? pages[i / SECTORS_PER_PAGE] : 0;
    return phys ? (u8 *)phys + (i % SECTORS_PER_PAGE) * 512 : NULL;
#endif
}
static void hash_unlink(u32 i) {
    if (!hash || !(cache[i].flags & F_VALID)) { cache[i].next = 0; return; }
    u32 *link = &hash[hash_of(cache[i].unit, cache[i].lba)];
    while (*link) {
        u32 idx = *link - 1;
        if (idx == i) { *link = cache[i].next; cache[i].next = 0; return; }
        link = &cache[idx].next;
    }
}
static void hash_link(u32 i) {
    u32 h = hash_of(cache[i].unit, cache[i].lba);
    cache[i].next = hash[h];
    hash[h] = i + 1;
}
/* Список грязных слотов. Сброс идёт по нему, а не по всей таблице:
 * таблица рассчитана на сотни мегабайт, и полный просмотр на каждом
 * барьере журнала съедал больше, чем сама запись. */
static void dirty_unlink(u32 i) {
    if (!(cache[i].flags & F_LIST)) return;
    u32 next = cache[i].dirty_next, prev = cache[i].dirty_prev;
    if (prev) cache[prev - 1].dirty_next = next;
    else dirty_head = next;
    if (next) cache[next - 1].dirty_prev = prev;
    cache[i].dirty_next = cache[i].dirty_prev = 0;
    cache[i].flags = (u16)(cache[i].flags & ~F_LIST);
}
static void dirty_add(u32 i) {
    if (cache[i].flags & F_LIST) return;
    cache[i].dirty_next = dirty_head;
    cache[i].dirty_prev = 0;
    if (dirty_head) cache[dirty_head - 1].dirty_prev = i + 1;
    dirty_head = i + 1;
    cache[i].flags = (u16)(cache[i].flags | F_LIST);
}

#ifndef HOST_TEST
/* Половина ОЗУ остаётся программам. Кэш берёт не больше четверти и только
 * то, что свободно сверх этого резерва. На маленькой машине — 2 МиБ:
 * хватает на заголовки и не теснит гостя на 256 МиБ. Чистые страницы
 * возвращаются через block_reclaim, когда выделение кадра не удалось. */
static u32 choose_slots(void) {
    struct pmm_stats st;
    pmm_stats(&st);
    u32 total = st.total_frames, used = st.used_frames;
    u32 free = total > used ? total - used : 0;
    u32 slots = 512;
    if (total >= (384u * 1024u * 1024u) / PAGE_SIZE) {
        u32 reserve = total / 2;
        u32 budget = free > reserve ? free - reserve : 0;
        u32 cap = total / 4;
        if (budget > cap) budget = cap;
        /* Slot table and hash take roughly a quarter of the data bytes. */
        u32 data_frames = budget - budget / 4;
        u32 n = data_frames * SECTORS_PER_PAGE;
        if (n > slots) slots = n;
    } else if (free > 2048) slots = 4096;
    if (slots > 524288) slots = 524288;
    slots &= ~(SECTORS_PER_PAGE - 1);
    return slots ? slots : SECTORS_PER_PAGE;
}
/* Caller holds block_lock. Drops clean groups only: dirty file data is not
 * discarded to satisfy an allocation. */
static u32 reclaim_clean_locked(u32 frames) {
    if (!pages || !frames) return 0;
    u32 groups = cache_slots / SECTORS_PER_PAGE, live = 0, freed = 0;
    for (u32 g = 0; g < groups; g++) if (pages[g]) live++;
    for (u32 g = 0; g < groups && freed < frames && live > 16; g++) {
        if (!pages[g]) continue;
        bool dirty = false;
        for (u32 s = 0; s < SECTORS_PER_PAGE; s++) {
            u32 i = g * SECTORS_PER_PAGE + s;
            if ((cache[i].flags & (F_VALID | F_DIRTY)) == (F_VALID | F_DIRTY)) dirty = true;
        }
        if (dirty) continue;
        for (u32 s = 0; s < SECTORS_PER_PAGE; s++) {
            u32 i = g * SECTORS_PER_PAGE + s;
            if (cache[i].flags & F_VALID) hash_unlink(i);
            cache[i].flags = 0;
        }
        pmm_free(pages[g]);
        pages[g] = 0;
        freed++;
        live--;
    }
    return freed;
}
static int ensure_page(u32 i) {
    u32 g = i / SECTORS_PER_PAGE;
    if (pages[g]) return 0;
    u32 frame = pmm_alloc();
    if (!frame || frame >= IDENTITY_MAX) {
        if (frame) pmm_free(frame);
        if (!reclaim_clean_locked(1)) return -1;
        frame = pmm_alloc();
        if (!frame || frame >= IDENTITY_MAX) { if (frame) pmm_free(frame); return -1; }
    }
    pages[g] = frame;
    return 0;
}
u32 block_reclaim(u32 frames) {
    if (!cache || !frames || !mutex_trylock(&block_lock)) return 0;
    u32 freed = reclaim_clean_locked(frames);
    mutex_unlock(&block_lock);
    return freed;
}
#else
u32 block_reclaim(u32 frames) { (void)frames; return 0; }
static int ensure_page(u32 i) { (void)i; return data_blob ? 0 : -1; }
#endif

static u32 sectors(int u) {
#ifdef HOST_TEST
    return u==0 ? ata_sectors() : 0;
#else
    if (ahci_present(u)) return ahci_sectors(u);
    return ata_unit_sectors(u);
#endif
}
static u32 run_limit(int unit) {
#ifdef HOST_TEST
    (void)unit;
    return CACHE_RUN;
#else
    return ahci_present(unit) ? AHCI_MAX_SECTORS : CACHE_RUN;
#endif
}
static int device_read_run(int u, u32 lba, void *buf, u32 n) {
    if (!n || n > RUN_MAX) return -1;
#ifndef HOST_TEST
    if (!ahci_present(u) && n > CACHE_RUN) return -1;
#endif
#ifdef HOST_TEST
    if (u) return -1;
    u8 *p = buf;
    for (u32 i = 0; i < n; i++) if (ata_read(lba + i, p + i * 512)) return -1;
    return 0;
#else
    if (ahci_present(u)) return ahci_read_sectors(u, lba, buf, n);
    return ata_unit_read_sectors(u, lba, buf, n);
#endif
}
static int device_write_run(int u, u32 lba, const void *buf, u32 n) {
    if (u >= 0 && u < 2) device_dirty[u] = true;
    if (!n || n > RUN_MAX) return -1;
#ifndef HOST_TEST
    if (!ahci_present(u) && n > CACHE_RUN) return -1;
#endif
#ifdef HOST_TEST
    if (u) return -1;
    const u8 *p = buf;
    for (u32 i = 0; i < n; i++) if (ata_write(lba + i, p + i * 512)) return -1;
    return 0;
#else
    if (ahci_present(u)) return ahci_write(u, lba, buf, n);
    return ata_unit_write_sectors(u, lba, (void *)buf, n);
#endif
}
static int cache_find(int unit, u32 lba) {
    if (!hash) return -1;
    for (u32 link = hash[hash_of(unit, lba)]; link; link = cache[link - 1].next) {
        u32 i = link - 1;
        if ((cache[i].flags & F_VALID) && cache[i].unit == unit && cache[i].lba == lba) return (int)i;
    }
    return -1;
}
/* Caller holds block_lock. Writes a consecutive dirty run and clears dirty.
 * Список грязных — стек, поэтому старт может оказаться в конце пачки:
 * сначала сдвигаемся к её началу, иначе каждый сектор уходит отдельно. */
static int flush_from(int unit, u32 lba) {
    u32 limit = run_limit(unit);
    u32 back = 0;
    while (back + 1 < limit && lba) {
        int prev = cache_find(unit, lba - 1);
        if (prev < 0 || !(cache[prev].flags & F_DIRTY)) break;
        lba--;
        back++;
    }
    u32 n = 0;
    while (n < limit) {
        int i = cache_find(unit, lba + n);
        if (i < 0 || !(cache[i].flags & F_DIRTY)) break;
        u8 *src = slot_data((u32)i);
        if (!src || !flush_buf) return -1;
        memcpy(flush_buf + n * 512, src, 512);
        n++;
    }
    if (!n) return 0;
    if (device_write_run(unit, lba, flush_buf, n)) return -1;
    for (u32 k = 0; k < n; k++) {
        int i = cache_find(unit, lba + k);
        if (i >= 0) {
            cache[i].flags = (u16)(cache[i].flags & ~F_DIRTY);
            dirty_unlink((u32)i);
        }
    }
    return 0;
}
static int claim_slot(void) {
    for (u32 n = 0; n < cache_slots * 2; n++) {
        u32 i = clock_hand;
        if (++clock_hand == cache_slots) clock_hand = 0;
        if (!(cache[i].flags & F_VALID)) {
            if (ensure_page(i)) continue;
            return (int)i;
        }
        if (cache[i].flags & F_REF) { cache[i].flags = (u16)(cache[i].flags & ~F_REF); continue; }
        if (cache[i].flags & F_DIRTY) {
            if (flush_from(cache[i].unit, cache[i].lba)) return -1;
            if (cache[i].flags & F_DIRTY) return -1;
        }
        hash_unlink(i);
        cache[i].flags = 0;
        if (ensure_page(i)) continue;
        return (int)i;
    }
    return -1;
}
static void *cache_alloc(unsigned long n, unsigned long size) {
#ifdef HOST_TEST
    return calloc(n, size);
#else
    return kcalloc(n, size);
#endif
}
static int ensure_cache(void) {
    if (cache && run_buf && flush_buf && hash) return 0;
    mutex_lock(&block_lock);
    if (!cache || !run_buf || !flush_buf || !hash) {
#ifdef HOST_TEST
        cache_slots = 128;
#else
        if (!cache_slots) cache_slots = choose_slots();
#endif
        if (!cache) cache = cache_alloc(cache_slots, sizeof *cache);
        if (!run_buf) run_buf = cache_alloc(RUN_MAX, 512);
        /* Отдельный буфер: сброс грязного слота во время опережающего чтения
         * не затирает только что прочитанную пачку. */
        if (!flush_buf) flush_buf = cache_alloc(RUN_MAX, 512);
#ifdef HOST_TEST
        if (!data_blob) data_blob = cache_alloc(cache_slots, 512);
#else
        /* Страницы данных берём по мере обращения, а не все сразу:
         * предел — это потолок, свободная память остаётся программам. */
        u32 groups = cache_slots / SECTORS_PER_PAGE;
        if (!pages) pages = cache_alloc(groups, sizeof *pages);
#endif
        if (!hash && cache_slots) {
            u32 size = 1;
            while (size < cache_slots * 2) size <<= 1;
            hash = cache_alloc(size, sizeof *hash);
            if (hash) hash_mask = size - 1;
        }
    }
    int rc = (cache && run_buf && flush_buf && hash && cache_slots) ? 0 : -1;
#ifdef HOST_TEST
    if (!data_blob) rc = -1;
#else
    if (!pages || !cache_slots) rc = -1;
#endif
    bool announce = false;
#ifndef HOST_TEST
    announce = !cache_announced && !rc;
    if (announce) cache_announced = true;
#endif
    mutex_unlock(&block_lock);
#ifndef HOST_TEST
    if (announce) kprintf("block cache: up to %u KiB\n", cache_slots / 2);
#else
    (void)announce;
#endif
    return rc;
}
static int cache_store(int unit, u32 lba, const void *buf, bool through) {
    if (ensure_cache()) return -1;
    mutex_lock(&block_lock);
    int i = cache_find(unit, lba);
    if (i < 0) i = claim_slot();
    int rc = -1;
    if (i >= 0) {
        u8 *dst = slot_data((u32)i);
        if (!dst) rc = -1;
        else {
            if (cache[i].flags & F_VALID) hash_unlink((u32)i);
            memcpy(dst, buf, 512);
            cache[i].unit = (i16)unit;
            cache[i].lba = lba;
            if (!through) dirty_add((u32)i);
            cache[i].stamp = epoch;
            cache[i].flags = (u16)(F_VALID | F_REF | (cache[i].flags & F_LIST) | (through ? 0 : F_DIRTY));
            hash_link((u32)i);
            rc = 0;
            if (through && device_write_run(unit, lba, buf, 1)) {
                dirty_add((u32)i);
                cache[i].flags = (u16)(cache[i].flags | F_DIRTY);
                rc = -1;
            } else if (through) dirty_unlink((u32)i);
        }
    }
    mutex_unlock(&block_lock);
    return rc;
}
static int install_slot(int unit, u32 lba, const u8 *src) {
    int i = cache_find(unit, lba);
    /* A sector already dirty in memory is newer than this read-ahead. */
    if (i >= 0) {
        if (cache[i].flags & F_DIRTY) return 0;
        u8 *dst = slot_data((u32)i);
        if (!dst) return -1;
        memcpy(dst, src, 512);
        cache[i].flags = (u16)(F_VALID | F_REF);
        return 0;
    }
    i = claim_slot();
    if (i < 0) return -1;
    u8 *dst = slot_data((u32)i);
    if (!dst) return -1;
    memcpy(dst, src, 512);
    cache[i].unit = (i16)unit;
    cache[i].lba = lba;
    cache[i].flags = (u16)(F_VALID | F_REF);
    hash_link((u32)i);
    return 0;
}
static int cache_load(int unit, u32 lba, void *buf) {
    if (ensure_cache()) return -1;
    mutex_lock(&block_lock);
    int i = cache_find(unit, lba);
    int rc = 0;
    if (i >= 0) {
        u8 *src = slot_data((u32)i);
        if (!src) rc = -1;
        else {
            cache[i].flags = (u16)(cache[i].flags | F_REF);
            memcpy(buf, src, 512);
        }
    } else {
        u32 max = sectors(unit), n = 1;
        if (!max || lba >= max) rc = -1;
        else {
            /* Второй промах подряд читает пачку. Случайный промах — один сектор,
             * чтобы обход каталога не вытеснил файл. */
            if (seq_valid && seq_unit == unit && seq_lba == lba) n = run_limit(unit);
            if (n > max - lba) n = max - lba;
            if (device_read_run(unit, lba, run_buf, n)) rc = -1;
            else {
                memcpy(buf, run_buf, 512);
                for (u32 k = 0; k < n; k++) if (install_slot(unit, lba + k, run_buf + k * 512)) break;
                seq_unit = unit;
                seq_lba = lba + n;
                seq_valid = true;
            }
        }
    }
    if (!rc) { seq_unit = unit; seq_lba = lba + 1; seq_valid = true; }
    mutex_unlock(&block_lock);
    return rc;
}
static int flush_locked(void) {
    int rc = 0;
    if (!cache || !flush_buf) return 0;
    while (dirty_head) {
        u32 i = dirty_head - 1;
        if (!(cache[i].flags & F_DIRTY)) { dirty_unlink(i); continue; }
        if (flush_from(cache[i].unit, cache[i].lba)) { rc = -1; break; }
    }
    return rc;
}
static int barrier_locked(int unit) {
    if (!device_dirty[unit]) return 0;
#ifdef HOST_TEST
    /* Host fixtures may override this to model a volatile drive cache. */
    extern int ata_unit_flush(int) __attribute__((weak));
    int r = ata_unit_flush ? ata_unit_flush(unit) : 0;
#else
    int r = ahci_present(unit) ? ahci_flush(unit) : ata_unit_flush(unit);
#endif
    if (!r) device_dirty[unit] = false;
    return r;
}
int block_flush_device(int id) {
    if (!block_get(id)) return -1;
    mutex_lock(&block_lock);
    int r = flush_locked();
    if (!r) r = barrier_locked(id / 5);
    mutex_unlock(&block_lock);
    return r;
}
int block_flush(void) {
    mutex_lock(&block_lock);
    int rc = flush_locked();
    for (int u = 0; u < 2; u++) if (barrier_locked(u)) rc = -1;
    mutex_unlock(&block_lock);
    return rc;
}
void block_flush_idle(void) {
    /* The idle task must not sleep on this lock. The owner may itself be
     * asleep inside an ATA command, waiting for the timer, and a waiter
     * here would run with interrupts cleared. */
#ifndef HOST_TEST
    if (!mutex_trylock(&block_lock)) return;
    flush_locked();
    mutex_unlock(&block_lock);
#else
    block_flush();
#endif
}
void block_epoch_begin(void) {
    if (++epoch == 0) epoch = 1;
}
void block_epoch_rollback(void) {
    if (!cache || !epoch) return;
    mutex_lock(&block_lock);
    for (u32 i = 0; i < cache_slots; i++) {
        if (!(cache[i].flags & F_DIRTY) || cache[i].stamp != epoch) continue;
        dirty_unlink(i);
        if (cache[i].flags & F_VALID) hash_unlink(i);
        cache[i].flags = 0;
    }
    mutex_unlock(&block_lock);
}
void block_cache_invalidate(void) {
    mutex_lock(&block_lock);
    if (cache)
        for (u32 i = 0; i < cache_slots; i++) cache[i].flags = 0;
    dirty_head = 0;
    if (hash)
        for (u32 i = 0; i <= hash_mask; i++) hash[i] = 0;
    seq_valid = false;
    mutex_unlock(&block_lock);
}
static u32 le32(const u8 *p) { return p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }
const struct block_device *block_get(int id) { return id >= 0 && id < BLOCK_MAX && devices[id].sectors ? &devices[id] : NULL; }
int block_count(void) { int n = 0; for (int i = 0; i < BLOCK_MAX; i++) if (block_get(i)) n++; return n; }
int block_at(int index) { for (int i = 0; i < BLOCK_MAX; i++) if (block_get(i) && index-- == 0) return i; return -1; }
bool block_disk_partitioned(int disk) { return disk >= 0 && disk < 2 && tables[disk]; }
bool block_partitioned(void) { return tables[0]; }
int block_find(const char *name) {
    if (!strncmp(name, "/dev/", 5)) name += 5;
    for (int i = 0; i < BLOCK_MAX; i++) if (block_get(i) && !strcmp(name, devices[i].name)) return i;
    return -1;
}
int block_rescan(int disk) {
    if (disk < 0 || disk >= 2) return -1;
    int base = disk * 5; memset(devices + base, 0, 5 * sizeof(*devices)); tables[disk] = false;
    u32 total = sectors(disk); if (!total) return -1;
    if (total > 0x10000000u) total = 0x10000000u;
    struct block_device *whole = &devices[base]; strcpy(whole->name, disk ? "sdc" : "sdb"); whole->sectors = total;
    u8 buf[512]; if (cache_load(disk, 0, buf)) return -1;
    if (buf[510] != 0x55 || buf[511] != 0xaa) return 0;
    tables[disk] = true;
    for (int i = 0; i < 4; i++) {
        const u8 *p = buf + 446 + i * 16; u32 start = le32(p + 8), size = le32(p + 12);
        if (!p[4] || p[4] == 0xee || p[4] == 5 || p[4] == 0x0f || p[4] == 0x85) continue;
        if (!start || !size || start >= total || size > total - start) continue;
        bool overlap = false;
        for (int j = 1; j <= i; j++) { const struct block_device *d = &devices[base + j];
            if (d->sectors && start < d->start + d->sectors && d->start < start + size) overlap = true;
        }
        if (overlap) continue;
        struct block_device *d = &devices[base + i + 1]; strcpy(d->name, whole->name); d->name[3] = '1' + i; d->name[4] = 0;
        d->start = start; d->sectors = size; d->partition = true;
    }
    return 0;
}
int block_init(void) {
#ifdef HOST_TEST
    static bool hooked;
    if (!hooked) { atexit(host_exit_flush); hooked = true; }
#endif
    int r = block_rescan(0); block_rescan(1); return r;
}
static int access_sector(int id, u32 sector, void *buf, bool write, bool through) {
    const struct block_device *d = block_get(id);
    if (!d || sector >= d->sectors) return -1;
#ifdef HOST_TEST
    if (write) io_writes++; else io_reads++;
#endif
    return write ? cache_store(id / 5, d->start + sector, buf, through) : cache_load(id / 5, d->start + sector, buf);
}
int block_read(int id, u32 sector, void *buf) { return access_sector(id, sector, buf, false, false); }
int block_write(int id, u32 sector, const void *buf) { return access_sector(id, sector, (void *)buf, true, false); }
int block_write_through(int id, u32 sector, const void *buf) { return access_sector(id, sector, (void *)buf, true, true); }
