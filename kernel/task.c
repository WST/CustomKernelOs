/* Многозадачность: потоки ядра, пользовательские процессы, планировщик.
 *
 * КАК ПЕРЕКЛЮЧАЕТСЯ КОНТЕКСТ
 * Любое прерывание попадает в isr_common (isr.asm), который кладёт все регистры
 * задачи на стек — получается struct regs — и вызывает isr_handler(regs*).
 * isr_handler возвращает указатель на struct regs, из которого надо «вернуться».
 * Если вернуть regs другой задачи, isr_common сделает mov esp, eax, снимет её
 * регистры и выполнит iret — процессор продолжит работу уже в другой задаче.
 *
 * ПРОЦЕССЫ (ring 3)
 * У процесса свой каталог страниц и свой стек ядра. При переключении на него
 * мы загружаем его CR3 и пишем вершину его стека ядра в TSS (esp0): когда в
 * программе случится прерывание, процессор перейдёт именно на этот стек. */
#include "task.h"
#include "tty.h"
#include "ipc.h"
#include "heap.h"
#include "string.h"
#include "timer.h"
#include "vga.h"
#include "gdt.h"
#include "paging.h"
#include "file.h"
#include "fb.h"

#define CANARY       0xDEADC0DE
#define CANARY_WORDS 4
#define STACK_RED    2048         /* последние 2 КБ стека — запретная зона */

static struct task *tasks[MAX_TASKS];
static struct task *current;
static struct task *idle_task;
static struct task  task0;          /* kmain / оболочка: живёт на стеке загрузчика */
static u32 next_id;
static u32 slice;
static bool need_resched;
u32 irq_nesting;

/* ---------------------------------------------------------------- служебное */

struct task *task_current(void) { return current; }
bool tasking_ready(void) { return current != NULL; }

const char *task_state_name(enum task_state s) {
    switch (s) {
    case TASK_READY:    return "ready";
    case TASK_RUNNING:  return "running";
    case TASK_SLEEPING: return "sleeping";
    case TASK_BLOCKED:  return "blocked";
    case TASK_ZOMBIE:   return "zombie";
    }
    return "?";
}

static int slot_of(struct task *t) {
    for (int i = 0; i < MAX_TASKS; i++) if (tasks[i] == t) return i;
    return -1;
}

struct task *task_by_id(u32 id) {
    for (int i = 0; i < MAX_TASKS; i++) if (tasks[i] && tasks[i]->id == id) return tasks[i];
    return NULL;
}

static bool canary_ok(struct task *t) {
    if (!t->stack) return true;
    u32 *c = (u32 *)t->stack;
    for (int i = 0; i < CANARY_WORDS; i++) if (c[i] != CANARY) return false;
    return true;
}

static void release_mutex(struct task *t) {
    if (t->holding && t->holding->owner == t) {
        t->holding->owner = NULL;
        t->holding->depth = 0;
        task_wakeup(t->holding);
    }
    t->holding = NULL;
}

static void close_all_files(struct task *t) {
    for (int i = 0; i < MAX_FDS; i++) {
        file_close(t->fds[i]);
        t->fds[i] = NULL;
    }
}

/* Превратить задачу в зомби: стек и память освободит reap().
 * Файлы закрываем сразу — чтобы, например, читатель канала увидел конец. */
static void make_zombie(struct task *t, int code) {
    if (t->state == TASK_ZOMBIE) return;
    release_mutex(t);
    close_all_files(t);
    console_release(t->id);         /* если программа перевела терминал в сырой режим */
    tty_release(t->id);
    fb_release(t->id);              /* если программа захватила экран — вернуть консоль */
    t->exit_code = code;
    t->state = TASK_ZOMBIE;

    /* дети умершего становятся сиротами: их код выхода никто не заберёт */
    for (int i = 0; i < MAX_TASKS; i++) {
        struct task *c = tasks[i];
        if (c && c->parent == t->id && c->user) {
            c->parent = 0;
            c->keep_zombie = false;     /* ждать его больше некому — пусть reap() уберёт */
        }
    }
    task_wakeup(t);                 /* того, кто ждёт именно эту задачу (task_waitpid) */
    struct task *p = t->parent ? task_by_id(t->parent) : NULL;
    if (p) task_wakeup(p);          /* родителя, ждущего любого ребёнка (waitpid -1) */
}

static void free_task(struct task *t) {
    close_all_files(t);
    if (t->user && t->page_dir) { shm_release_all(t, t->page_dir); vmm_destroy_dir(t->page_dir); }
    msg_free(t);
    kfree(t->stack);
    kfree(t);
}

/* Освободить завершившиеся задачи. Свой стек (и свой каталог страниц)
 * освобождать нельзя — поэтому это делает кто-то другой. */
void task_reap(void) {
    for (int i = 0; i < MAX_TASKS; i++) {
        u32 f = irq_save();
        struct task *t = tasks[i];
        /* каталог процесса нельзя освобождать, пока он загружен в CR3
         * (у потоков ядра общий каталог ядра — его не освобождаем вовсе) */
        if (t && t->state == TASK_ZOMBIE && t != current && !t->keep_zombie &&
            !(t->user && t->page_dir == vmm_current_dir())) {
            tasks[i] = NULL;
            irq_restore(f);
            free_task(t);
            continue;
        }
        irq_restore(f);
    }
}

/* ---------------------------------------------------------------- FPU
 * Сопроцессор (x87) — общий для всех. Если программа A считала с double, а
 * планировщик переключился на B, которая тоже считает, регистры FPU надо
 * сохранить и потом вернуть. fnsave кладёт всё состояние (108 байт) в память,
 * frstor загружает обратно. Ядро само FPU не использует (-mno-80387). */
static u8 fpu_clean[108];

void fpu_init(void) {
    u32 cr0;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~(1u << 2);                  /* EM=0: FPU есть, команды не эмулировать */
    cr0 &= ~(1u << 3);                  /* TS=0 */
    cr0 |= (1u << 1) | (1u << 5);       /* MP=1, NE=1: ошибки FPU — обычным исключением */
    __asm__ volatile ("mov %0, %%cr0" : : "r"(cr0));
    __asm__ volatile ("fninit; fnsave %0" : "=m"(fpu_clean));
}

static inline void fpu_save(struct task *t)    { __asm__ volatile ("fnsave %0" : "=m"(t->fpu)); }
static inline void fpu_restore(struct task *t) { __asm__ volatile ("frstor %0" : : "m"(t->fpu)); }

void fpu_reset_current(void) {
    memcpy(current->fpu, fpu_clean, sizeof(fpu_clean));
    fpu_restore(current);
}

void fpu_copy_current(struct task *to) {
    fpu_save(current);                  /* fnsave сбрасывает FPU — сразу вернём */
    fpu_restore(current);
    memcpy(to->fpu, current->fpu, sizeof(current->fpu));
}

/* ---------------------------------------------------------------- планировщик */

static struct task *pick_next(void) {
    int cur = slot_of(current);
    for (int i = 1; i <= MAX_TASKS; i++) {
        struct task *t = tasks[(cur + i) % MAX_TASKS];
        if (t && t != idle_task && t != current && t->state == TASK_READY) return t;
    }
    if (current != idle_task && current->state == TASK_READY) return current;
    return idle_task;
}

struct regs *schedule(struct regs *r) {
    if (!current) return r;

    current->esp = (u32)r;
    /* переполнение стека ядра: esp в «красной зоне» или затёрта канарейка */
    if (current->state != TASK_ZOMBIE && current->stack &&
        ((u32)r < (u32)current->stack + STACK_RED || !canary_ok(current))) {
        kprintf("\n[task %u '%s' killed: kernel stack overflow]\n", current->id, current->name);
        make_zombie(current, 134);
    }
    if (current->state == TASK_RUNNING) current->state = TASK_READY;

    struct task *next = pick_next();
    if (next->state == TASK_READY) next->state = TASK_RUNNING;
    if (next != current) {
        if (current->user && current->state != TASK_ZOMBIE) fpu_save(current);
        if (next->user) fpu_restore(next);
    }
    current = next;
    slice = TIME_SLICE;
    need_resched = false;

    vmm_switch(next->page_dir);                     /* адресное пространство задачи */
    if (next->stack) tss_set_kernel_stack((u32)next->stack + TASK_STACK);
    return (struct regs *)next->esp;
}

struct regs *sched_tick(struct regs *r) {
    if (!current) return r;
    current->cpu_ticks++;

    u32 now = timer_ticks();
    for (int i = 0; i < MAX_TASKS; i++) {
        struct task *t = tasks[i];
        if (t && t->state == TASK_SLEEPING && (i32)(now - t->wake_tick) >= 0) {
            t->state = TASK_READY;
            need_resched = true;
        }
        if (t && t->state == TASK_BLOCKED && t->wait_deadline &&
            (i32)(now - t->wait_deadline) >= 0) {       /* ждал с тайм-аутом — время вышло */
            t->state = TASK_READY;
            need_resched = true;
        }
    }
    if (now % TIMER_HZ == 0) {                      /* CPU% за последнюю секунду */
        for (int i = 0; i < MAX_TASKS; i++) {
            struct task *t = tasks[i];
            if (!t) continue;
            t->cpu_recent = t->cpu_ticks - t->cpu_snapshot;
            t->cpu_snapshot = t->cpu_ticks;
        }
    }

    if (current->state == TASK_ZOMBIE) return schedule(r);
    if (current == idle_task && need_resched) return schedule(r);
    if (slice) slice--;
    if (slice == 0) return schedule(r);             /* квант исчерпан — вытесняем */
    return r;
}

struct regs *sched_maybe(struct regs *r) {
    if (!current) return r;
    if (current->state == TASK_ZOMBIE) return schedule(r);   /* например, убит по Ctrl+C */
    if (need_resched && current == idle_task) return schedule(r);
    return r;
}

/* ---------------------------------------------------------------- создание */

static void task_entry(void) {
    current->entry(current->arg);
    task_exit();
}

static void idle_loop(void *arg) {
    (void)arg;
    for (;;) {
        task_reap();
        __asm__ volatile ("sti; hlt");
    }
}

void tasking_init(void) {
    memset(&task0, 0, sizeof(task0));
    task0.id = next_id++;
    memcpy(task0.name, "shell", 6);
    task0.state = TASK_RUNNING;
    task0.page_dir = vmm_kernel_dir();
    tasks[0] = &task0;
    current = &task0;
    slice = TIME_SLICE;

    task_create("idle", idle_loop, NULL);   /* займёт слот 1 */
    idle_task = tasks[1];
}

static struct task *alloc_task(const char *name) {
    task_reap();
    struct task *t = kcalloc(1, sizeof(*t));
    u8 *stack = kcalloc(1, TASK_STACK);
    if (!t || !stack) { kfree(t); kfree(stack); return NULL; }
    for (int i = 0; i < CANARY_WORDS; i++) ((u32 *)stack)[i] = CANARY;
    u32 n = strlen(name);
    if (n >= TASK_NAME) n = TASK_NAME - 1;
    memcpy(t->name, name, n);
    t->stack = stack;
    t->state = TASK_READY;
    t->created = timer_ticks();
    t->page_dir = vmm_kernel_dir();
    return t;
}

/* кадр «прерванной» задачи на вершине её стека ядра */
static struct regs *initial_frame(struct task *t) {
    struct regs *r = (struct regs *)(t->stack + TASK_STACK - sizeof(struct regs));
    memset(r, 0, sizeof(*r));
    r->eflags = 0x202;                      /* IF=1: прерывания включены */
    t->esp = (u32)r;
    return r;
}

/* поставить задачу в таблицу -> её id; при неудаче задача освобождается и -> 0 */
u32 task_start(struct task *t) {
    u32 f = irq_save();
    int slot = -1;
    for (int i = 0; i < MAX_TASKS; i++) if (!tasks[i]) { slot = i; break; }
    if (slot >= 0) {
        t->id = next_id++;
        tasks[slot] = t;
    }
    if (slot >= 0 && !t->pgid) t->pgid = t->id;   /* новая группа (fork задаёт свою) */
    u32 id = slot >= 0 ? t->id : 0;
    irq_restore(f);
    if (!id) free_task(t);
    return id;
}

int task_create(const char *name, void (*entry)(void *), void *arg) {
    struct task *t = alloc_task(name);
    if (!t) return -1;
    t->entry = entry;
    t->arg = arg;
    struct regs *r = initial_frame(t);
    r->gs = r->fs = r->es = r->ds = KERNEL_DS;
    r->cs = KERNEL_CS;
    r->eip = (u32)task_entry;
    u32 id = task_start(t);
    return id ? (int)id : -2;
}

/* Процесс: первый же iret из этого кадра уйдёт в кольцо 3 (cs = 0x1B),
 * процессор заодно загрузит пользовательские ss:esp из кадра. */
struct task *task_alloc_user(const char *name, u32 page_dir, u32 entry, u32 user_esp) {
    struct task *t = alloc_task(name);
    if (!t) return NULL;
    t->user = true;
    t->page_dir = page_dir;
    struct regs *r = initial_frame(t);
    r->gs = r->fs = r->es = r->ds = USER_DS;
    r->cs = USER_CS;
    r->ss = USER_DS;
    r->eip = entry;
    r->useresp = user_esp;
    for (int i = 0; i < 3; i++) t->fds[i] = &console_file;   /* stdin, stdout, stderr */
    memcpy(t->fpu, fpu_clean, sizeof(fpu_clean));
    return t;
}

/* ---------------------------------------------------------------- завершение */

void task_yield(void) {
    __asm__ volatile ("int $0x81");
}

void task_exit_code(int code) {
    __asm__ volatile ("cli");
    make_zombie(current, code);
    task_yield();
    for (;;) ;                               /* сюда управление не вернётся */
}

void task_exit(void) { task_exit_code(0); }

int task_kill(u32 id) {
    u32 f = irq_save();
    struct task *t = task_by_id(id);
    if (!t || t->state == TASK_ZOMBIE) { irq_restore(f); return -1; }
    if (t == &task0 || t == idle_task) { irq_restore(f); return -2; }
    if (t->user && t->in_syscall) {
        /* Процесс сейчас внутри ядра (например, пишет на диск). Убить его прямо
         * сейчас — значит бросить структуры ядра на полпути. Поэтому только
         * ставим флаг и будим; умрёт он сам, на выходе из системного вызова. */
        t->kill_pending = true;
        if (t->state == TASK_SLEEPING || t->state == TASK_BLOCKED) t->state = TASK_READY;
    } else {
        make_zombie(t, 137);                 /* как SIGKILL в Unix: 128 + 9 */
    }
    need_resched = true;
    irq_restore(f);
    if (t == current && !irq_nesting) task_yield();
    return 0;
}

int task_kill_group(u32 pgid) {
    if (!pgid) return 0;
    int n = 0;
    for (int i = 0; i < MAX_TASKS; i++) {
        struct task *t = tasks[i];
        if (t && t->pgid == pgid && t->state != TASK_ZOMBIE && task_kill(t->id) == 0) n++;
    }
    return n;
}

/* waitpid() для процессов: ждать своего ребёнка pid (или любого, если pid = -1).
 * -> id завершившегося, 0 (nohang и все ещё живы), -1 (детей нет), -2 (прервано) */
int task_wait_child(int pid, int *code, bool nohang) {
    u32 f = irq_save();
    for (;;) {
        bool have = false;
        for (int i = 0; i < MAX_TASKS; i++) {
            struct task *c = tasks[i];
            if (!c || c->parent != current->id || (pid > 0 && c->id != (u32)pid)) continue;
            have = true;
            if (c->state == TASK_ZOMBIE) {
                u32 id = c->id;
                if (code) *code = c->exit_code;
                c->keep_zombie = false;
                c->parent = 0;
                irq_restore(f);
                return id;
            }
        }
        if (!have) { irq_restore(f); return -1; }
        if (nohang) { irq_restore(f); return 0; }
        task_wait(current);                  /* make_zombie ребёнка разбудит нас */
        if (current->kill_pending) { irq_restore(f); return -2; }
    }
}

/* дождаться завершения задачи и забрать код выхода */
int task_waitpid(u32 id, int *code) {
    u32 f = irq_save();
    for (;;) {
        struct task *t = task_by_id(id);
        if (!t) { irq_restore(f); return -1; }
        if (t->state == TASK_ZOMBIE) {
            if (code) *code = t->exit_code;
            t->keep_zombie = false;          /* теперь reap() может её убрать */
            irq_restore(f);
            return 0;
        }
        task_wait(t);
    }
}

/* ---------------------------------------------------------------- сон и ожидание */

void task_sleep_ticks(u32 ticks) {
    if (!current) {
        u32 end = timer_ticks() + ticks;
        while ((i32)(timer_ticks() - end) < 0) __asm__ volatile ("hlt");
        return;
    }
    if (!ticks) { task_yield(); return; }
    u32 f = irq_save();
    current->wake_tick = timer_ticks() + ticks;
    current->state = TASK_SLEEPING;
    task_yield();
    irq_restore(f);
}

/* вызывается с запрещёнными прерываниями; после пробуждения они тоже запрещены */
void task_wait(void *chan) {
    current->wait_chan = chan;
    current->wait_deadline = 0;
    current->state = TASK_BLOCKED;
    task_yield();
    current->wait_chan = NULL;
}

/* то же, но не дольше ticks тиков; вызывать с cli */
bool task_wait_timeout(void *chan, u32 ticks) {
    u32 deadline = timer_ticks() + (ticks ? ticks : 1);
    current->wait_chan = chan;
    current->wait_deadline = deadline;
    current->state = TASK_BLOCKED;
    task_yield();
    current->wait_chan = NULL;
    current->wait_deadline = 0;
    return (i32)(timer_ticks() - deadline) < 0;
}

void task_wakeup(void *chan) {
    u32 f = irq_save();
    for (int i = 0; i < MAX_TASKS; i++) {
        struct task *t = tasks[i];
        if (t && t->state == TASK_BLOCKED && t->wait_chan == chan) {
            t->state = TASK_READY;
            need_resched = true;
        }
    }
    irq_restore(f);
}

/* ---------------------------------------------------------------- мьютекс */

void mutex_lock(struct mutex *m) {
    if (!current) return;
    u32 f = irq_save();
    while (m->owner && m->owner != current) task_wait(m);
    m->owner = current;
    m->depth++;
    current->holding = m;
    irq_restore(f);
}

void mutex_unlock(struct mutex *m) {
    if (!current) return;
    u32 f = irq_save();
    if (m->owner == current && --m->depth == 0) {
        m->owner = NULL;
        current->holding = NULL;
        task_wakeup(m);
    }
    irq_restore(f);
}

/* ---------------------------------------------------------------- падения */

/* Исключение процессора. Если упал процесс или поток — убиваем только его. */
bool task_fault(struct regs *r, const char *what) {
    if (!current || current == &task0 || current == idle_task) return false;
    vga_set_color(LIGHT_RED, BLACK);
    if (current->user) {
        kprintf("\n%s: Segmentation fault (%s at EIP=%p", current->name, what, r->eip);
        if (r->int_no == 14) {
            u32 cr2;
            __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
            kprintf(", address %p", cr2);
        }
        kprintf(")\n");
    } else {
        kprintf("\n[task %u '%s' killed: %s at EIP=%p]\n", current->id, current->name, what, r->eip);
    }
    vga_set_color(LIGHT_GREY, BLACK);
    make_zombie(current, 139);               /* как SIGSEGV: 128 + 11 */
    return true;
}

/* ---------------------------------------------------------------- ps */

int task_list(struct task_info *out, int max) {
    task_reap();
    int n = 0;
    u32 now = timer_ticks();
    u32 f = irq_save();
    for (int i = 0; i < MAX_TASKS && n < max; i++) {
        struct task *t = tasks[i];
        if (!t) continue;
        struct task_info *o = &out[n++];
        o->id = t->id;
        memcpy(o->name, t->name, TASK_NAME);
        o->state = t->state;
        o->user = t->user;
        o->parent = t->parent;
        o->cpu_ticks = t->cpu_ticks;
        o->cpu_percent = t->cpu_recent * 100 / TIMER_HZ;
        o->age_ticks = now - t->created;
        o->mem_kb = (t->stack ? TASK_STACK / 1024 : 0) + t->user_pages * 4;
    }
    irq_restore(f);
    return n;
}
