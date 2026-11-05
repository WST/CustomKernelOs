/* Общая память, сообщения между процессами, имена служб и часы реального времени. */
#include "ipc.h"
#include "task.h"
#include "paging.h"
#include "pmm.h"
#include "heap.h"
#include "string.h"
#include "timer.h"
#include "syscall.h"

/* ------------------------------------------------------------ общая память
 * Объект — список физических страниц и счётчик отображений. Когда последний
 * процесс его отпускает (или умирает), страницы возвращаются системе. */

struct shm {
    bool used;
    u32  npages;
    u32 *frames;
    int  refs;
};
static struct shm shms[SHM_MAX];

int shm_alloc(u32 size) {
    if (size == 0 || size > 16 * 1024 * 1024) return -EINVAL;
    u32 n = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    u32 *frames = kcalloc(n, sizeof(u32));
    if (!frames) return -ENOMEM;
    for (u32 i = 0; i < n; i++) {
        frames[i] = pmm_alloc();
        if (!frames[i]) {
            for (u32 j = 0; j < i; j++) pmm_free(frames[j]);
            kfree(frames);
            return -ENOMEM;
        }
        memset((void *)frames[i], 0, PAGE_SIZE);
    }
    u32 f = irq_save();
    for (int id = 0; id < SHM_MAX; id++) {
        if (shms[id].used) continue;
        shms[id].used = true;
        shms[id].npages = n;
        shms[id].frames = frames;
        shms[id].refs = 0;
        irq_restore(f);
        return id + 1;                              /* id с единицы: 0 — «нет» */
    }
    irq_restore(f);
    for (u32 i = 0; i < n; i++) pmm_free(frames[i]);
    kfree(frames);
    return -ENOMEM;
}

static void shm_put(struct shm *s) {
    if (--s->refs > 0) return;
    for (u32 i = 0; i < s->npages; i++) pmm_free(s->frames[i]);
    kfree(s->frames);
    s->frames = NULL;
    s->used = false;
}

int shm_map(int id, u32 *addr) {
    struct task *t = task_current();
    if (id < 1 || id > SHM_MAX || !shms[id - 1].used) return -EINVAL;
    struct shm *s = &shms[id - 1];
    int slot = -1;
    for (int i = 0; i < SHM_PER_TASK; i++) {
        if (t->shm[i].id == id) { *addr = t->shm[i].addr; return 0; }   /* уже отображён */
        if (!t->shm[i].id && slot < 0) slot = i;
    }
    if (slot < 0) return -EMFILE;
    /* место в окне SHM_BASE..SHM_END: первая подходящая дырка (окна меняют размер —
     * старые буферы уходят, новые приходят, и адреса не должны кончаться) */
    u32 size = s->npages * PAGE_SIZE, va = 0;
    for (int c = -1; c < SHM_PER_TASK && !va; c++) {
        u32 cand = SHM_BASE;
        if (c >= 0) {
            if (!t->shm[c].id) continue;
            cand = t->shm[c].addr + shms[t->shm[c].id - 1].npages * PAGE_SIZE;
        }
        if (cand + size > SHM_END || cand + size < cand) continue;
        bool ok = true;
        for (int i = 0; i < SHM_PER_TASK && ok; i++) {
            if (!t->shm[i].id) continue;
            u32 a = t->shm[i].addr, e = a + shms[t->shm[i].id - 1].npages * PAGE_SIZE;
            if (cand < e && a < cand + size) ok = false;
        }
        if (ok) va = cand;
    }
    if (!va) return -ENOMEM;
    for (u32 i = 0; i < s->npages; i++)
        if (vmm_map_in(t->page_dir, va + i * PAGE_SIZE, s->frames[i], PG_USER | PG_WRITE)) {
            for (u32 j = 0; j < i; j++) vmm_unmap_in(t->page_dir, va + j * PAGE_SIZE);
            return -ENOMEM;
        }
    s->refs++;
    t->shm[slot].id = id;
    t->shm[slot].addr = va;
    *addr = va;
    return 0;
}

static void unmap_slot(struct task *t, u32 dir, int i) {
    struct shm *s = &shms[t->shm[i].id - 1];
    for (u32 p = 0; p < s->npages; p++) vmm_unmap_in(dir, t->shm[i].addr + p * PAGE_SIZE);
    t->shm[i].id = 0;
    t->shm[i].addr = 0;
    shm_put(s);
}

int shm_unmap(u32 addr) {
    struct task *t = task_current();
    for (int i = 0; i < SHM_PER_TASK; i++)
        if (t->shm[i].id && t->shm[i].addr == addr) { unmap_slot(t, t->page_dir, i); return 0; }
    return -EINVAL;
}

/* Перед тем как освободить память процесса: снять общие страницы, чтобы
 * vmm_destroy_dir не отдал их системе, пока ими пользуются другие. */
void shm_release_all(struct task *t, u32 dir) {
    u32 f = irq_save();
    for (int i = 0; i < SHM_PER_TASK; i++)
        if (t->shm[i].id) unmap_slot(t, dir, i);
    irq_restore(f);
}

/* ------------------------------------------------------------ сообщения */

struct mbox {
    u8  msgs[MBOX_LEN][MSG_SIZE];
    int head, count;
};

int msg_send(u32 pid, const void *msg) {
    struct task *to = task_by_id(pid);
    if (!to || !to->user || to->state == TASK_ZOMBIE) return -ESRCH;
    if (!to->mbox) {
        struct mbox *mb = kcalloc(1, sizeof(struct mbox));
        if (!mb) return -ENOMEM;
        u32 f = irq_save();
        if (!to->mbox) to->mbox = mb; else kfree(mb);
        irq_restore(f);
    }
    u32 f = irq_save();
    struct mbox *mb = to->mbox;
    if (mb->count == MBOX_LEN) { irq_restore(f); return -EAGAIN; }   /* ящик полон */
    u8 *slot = mb->msgs[(mb->head + mb->count) % MBOX_LEN];
    memcpy(slot, msg, MSG_SIZE);
    *(u32 *)slot = task_current()->id;              /* первое поле — от кого (подделать нельзя) */
    mb->count++;
    irq_restore(f);
    task_wakeup(mb);
    return 0;
}

int msg_recv(void *msg, int timeout_ms) {
    struct task *t = task_current();
    if (!t->mbox) {
        struct mbox *mb = kcalloc(1, sizeof(struct mbox));
        if (!mb) return -ENOMEM;
        u32 f = irq_save();
        if (!t->mbox) t->mbox = mb; else kfree(mb);
        irq_restore(f);
    }
    struct mbox *mb = t->mbox;
    u32 deadline = timer_ticks() + (timeout_ms > 0 ? (u32)(timeout_ms + 9) / 10 : 0);
    for (;;) {
        u32 f = irq_save();
        if (mb->count) {
            memcpy(msg, mb->msgs[mb->head], MSG_SIZE);
            mb->head = (mb->head + 1) % MBOX_LEN;
            mb->count--;
            irq_restore(f);
            return 0;
        }
        if (timeout_ms == 0) { irq_restore(f); return -EAGAIN; }
        if (timeout_ms > 0) {
            i32 left = (i32)(deadline - timer_ticks());
            if (left <= 0) { irq_restore(f); return -ETIMEDOUT; }
            task_wait_timeout(mb, left);
        } else {
            task_wait(mb);
        }
        irq_restore(f);
        if (t->kill_pending) return -EINTR;
    }
}

void msg_free(struct task *t) {
    kfree(t->mbox);
    t->mbox = NULL;
}

/* ------------------------------------------------------------ имена служб */

#define SVC_MAX 8
static struct { char name[16]; u32 pid; } svcs[SVC_MAX];

int svc_register(const char *name) {
    u32 pid = task_current()->id;
    int free_slot = -1;
    for (int i = 0; i < SVC_MAX; i++) {
        if (svcs[i].pid && !strcmp(svcs[i].name, name)) {
            struct task *o = task_by_id(svcs[i].pid);
            if (o && o->state != TASK_ZOMBIE && svcs[i].pid != pid) return -EEXIST;   /* занято живым */
            svcs[i].pid = pid;
            return 0;
        }
        if (!svcs[i].pid && free_slot < 0) free_slot = i;
    }
    if (free_slot < 0) return -ENOMEM;
    int n = 0;
    while (name[n] && n < 15) { svcs[free_slot].name[n] = name[n]; n++; }
    svcs[free_slot].name[n] = 0;
    svcs[free_slot].pid = pid;
    return 0;
}

int svc_find(const char *name) {
    for (int i = 0; i < SVC_MAX; i++) {
        if (!svcs[i].pid || strcmp(svcs[i].name, name)) continue;
        struct task *o = task_by_id(svcs[i].pid);
        if (!o || o->state == TASK_ZOMBIE) { svcs[i].pid = 0; return 0; }
        return svcs[i].pid;
    }
    return 0;
}

/* ------------------------------------------------------------ часы (CMOS RTC) */

static u8 cmos(u8 reg) { outb(0x70, reg); return inb(0x71); }

void rtc_read(struct rtc_time *t) {
    u8 s, m, h, d, mo, y, b;
    u32 f = irq_save();
    while (cmos(0x0A) & 0x80) {}                    /* не читать во время обновления */
    s = cmos(0x00); m = cmos(0x02); h = cmos(0x04);
    d = cmos(0x07); mo = cmos(0x08); y = cmos(0x09);
    b = cmos(0x0B);
    irq_restore(f);
    if (!(b & 0x04)) {                              /* числа в двоично-десятичном виде */
        #define BCD(x) (((x) & 0x0F) + ((x) >> 4) * 10)
        s = BCD(s); m = BCD(m); h = BCD(h & 0x7F) | (h & 0x80); d = BCD(d); mo = BCD(mo); y = BCD(y);
    }
    if (!(b & 0x02) && (h & 0x80)) h = ((h & 0x7F) + 12) % 24;   /* 12-часовой формат */
    t->year = 2000 + y; t->month = mo; t->day = d;
    t->hour = h & 0x7F; t->min = m; t->sec = s;
}
