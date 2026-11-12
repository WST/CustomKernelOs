/* Физический аллокатор страниц: битовая карта, 1 бит на фрейм 4 КБ.
 *
 *   бит = 1  — фрейм занят (или его нельзя трогать)
 *   бит = 0  — фрейм свободен
 *
 * Изначально всё помечено занятым; затем освобождаем то, что BIOS назвал
 * свободной RAM, и снова занимаем первый мегабайт (ядро, стек, BIOS, VGA). */
#include "pmm.h"
#include "string.h"
#include "bootinfo.h"
#include "idt.h"

#define LOW_RESERVED  0x100000        /* первый 1 МБ не раздаём никогда */
#define MAX_PHYS      0x40000000u     /* работаем с первым 1 ГБ (выше — память программ, см. paging.h) */
#define MAX_FRAMES    (MAX_PHYS / PAGE_SIZE)

static u32 bitmap[MAX_FRAMES / 32];   /* 32 КБ на 1 ГБ */
static u8 *refs;                      /* счётчики владельцев; лежат в расширенной памяти, не в BSS ядра */
static u32 limit_frame;               /* фреймы >= этого не существуют */
static u32 total_frames, used_frames;
static u32 next_hint;                 /* откуда начинать поиск */

static inline void set_used(u32 f) { bitmap[f / 32] |=  (1u << (f % 32)); }
static inline void set_free(u32 f) { bitmap[f / 32] &= ~(1u << (f % 32)); }
static inline bool is_used(u32 f)  { return bitmap[f / 32] & (1u << (f % 32)); }

u32 e820_count(void) {
    u32 n = *(volatile u32 *)E820_ADDR;
    return n > 128 ? 128 : n;
}

const struct e820_entry *e820_get(u32 i) {
    return (const struct e820_entry *)(E820_ADDR + 4) + i;
}

const char *e820_type_name(u32 type) {
    switch (type) {
    case E820_USABLE:   return "usable";
    case E820_RESERVED: return "reserved";
    case E820_ACPI:     return "ACPI reclaimable";
    case E820_NVS:      return "ACPI NVS";
    case E820_BAD:      return "bad memory";
    default:            return "unknown";
    }
}

/* обрезать регион [base, base+len) до 0..MAX_PHYS */
static bool clip(const struct e820_entry *e, u32 *start, u32 *end) {
    u64 b = e->base, t = e->base + e->length;
    if (b >= MAX_PHYS || e->length == 0) return false;
    if (t > MAX_PHYS) t = MAX_PHYS;
    *start = (u32)b;
    *end   = (u32)t;
    return true;
}

void pmm_init(void) {
    memset(bitmap, 0xFF, sizeof(bitmap));
    u32 n = e820_count();
    u32 s, e;

    if (n == 0) {
        /* BIOS без E820 — бывает только на очень старых машинах; считаем, что есть 32 МБ */
        static struct e820_entry fake = { 0, 32 * 1024 * 1024, E820_USABLE, 1 };
        clip(&fake, &s, &e);
        limit_frame = e / PAGE_SIZE;
        for (u32 f = 0; f < limit_frame; f++) set_free(f);
    } else {
        /* 1) освобождаем полностью свободные фреймы внутри регионов «usable» */
        for (u32 i = 0; i < n; i++) {
            const struct e820_entry *en = e820_get(i);
            if (en->type != E820_USABLE || !clip(en, &s, &e)) continue;
            u32 first = (s + PAGE_SIZE - 1) / PAGE_SIZE;   /* округлить вверх */
            u32 last  = e / PAGE_SIZE;                      /* округлить вниз */
            for (u32 f = first; f < last; f++) set_free(f);
            if (last > limit_frame) limit_frame = last;
        }
        /* 2) регионы разных типов могут перекрываться — зарезервированное важнее */
        for (u32 i = 0; i < n; i++) {
            const struct e820_entry *en = e820_get(i);
            if (en->type == E820_USABLE || !clip(en, &s, &e)) continue;
            for (u32 f = s / PAGE_SIZE; f < (e + PAGE_SIZE - 1) / PAGE_SIZE && f < MAX_FRAMES; f++)
                set_used(f);
        }
    }

    /* 3) первый мегабайт — наш (ядро, стек, карта E820) и BIOS */
    for (u32 f = 0; f < LOW_RESERVED / PAGE_SIZE; f++) set_used(f);

#ifdef MYOS_HARDWARE
    /* The live kernel is above 1 MiB. Reserve it and the RAM root before
     * allocating refcounts, otherwise clearing that table overwrites code. */
    extern u8 __kernel_start[], __bss_end[];
    for (u32 f=(u32)__kernel_start/PAGE_SIZE; f<((u32)__bss_end+PAGE_SIZE-1)/PAGE_SIZE; f++)
        set_used(f);
    const struct boot_info *boot=boot_info_get();
    if (boot_root_valid(boot))
        for (u32 f=boot->root_phys/PAGE_SIZE; f<(boot->root_phys+boot->root_bytes+PAGE_SIZE-1)/PAGE_SIZE; f++)
            set_used(f);
#endif

    /* Счётчики владельцев — по байту на кадр. В BSS ядра им нет места: ядро
     * собрано ниже 640 КБ, а 256 КБ таблицы залезли бы в окно VGA (0xA0000)
     * и затёрли экран. Кладём таблицу в свободную расширенную память. */
    u32 need = (limit_frame + PAGE_SIZE - 1) / PAGE_SIZE;
    u32 run = 0, start = 0;
    for (u32 f = LOW_RESERVED / PAGE_SIZE; f < limit_frame; f++) {
        if (is_used(f)) { run = 0; continue; }
        if (run == 0) start = f;
        if (++run == need) {
            refs = (u8 *)(start * PAGE_SIZE);
            memset(refs, 0, limit_frame);
            for (u32 i = 0; i < need; i++) set_used(start + i);
            break;
        }
    }
    if (!refs) kpanic("no memory for page refcounts");

    total_frames = used_frames = 0;
    for (u32 f = LOW_RESERVED / PAGE_SIZE; f < limit_frame; f++) {
        total_frames++;
        if (is_used(f)) used_frames++;
    }
    /* «занятые» дыры (reserved внутри диапазона) не считаем памятью */
    total_frames -= used_frames;
    used_frames = 0;
    next_hint = LOW_RESERVED / PAGE_SIZE;
}

u32 pmm_alloc(void) {
    if (!refs) return 0;
    for (u32 pass = 0; pass < 2; pass++) {
        u32 from = pass ? LOW_RESERVED / PAGE_SIZE : next_hint;
        u32 to   = pass ? next_hint : limit_frame;
        for (u32 f = from; f < to; f++) {
            if (bitmap[f / 32] == 0xFFFFFFFF) { f |= 31; continue; }   /* пропуск 32 занятых */
            if (!is_used(f)) {
                set_used(f);
                refs[f] = 1;
                used_frames++;
                next_hint = f + 1;
                return f * PAGE_SIZE;
            }
        }
    }
    return 0;  /* память закончилась */
}

void pmm_free(u32 addr) {
    if (!refs) return;
    u32 f = addr / PAGE_SIZE;
    if (addr % PAGE_SIZE || addr < LOW_RESERVED || f >= limit_frame || !is_used(f)) return;
    /* Ноль у занятого кадра — зарезервированная память (таблица счётчиков,
     * ядро), а не последняя ссылка. Отдавать такой кадр нельзя: следующий
     * pmm_alloc перезапишет его. Повторное освобождение уже свободного кадра
     * отсекается проверкой is_used выше. */
    if (refs[f] == 0) kpanic("free of a reserved frame");
    if (refs[f] > 1) { refs[f]--; return; }          /* кадр ещё отображён в другом процессе */
    refs[f] = 0;
    set_free(f);
    used_frames--;
    if (f < next_hint) next_hint = f;
}

bool pmm_share(u32 addr) {
    if (!refs) return false;
    u32 f = addr / PAGE_SIZE;
    if (addr % PAGE_SIZE || f >= limit_frame || !is_used(f) || refs[f] == 0 || refs[f] == 255) return false;
    refs[f]++;
    return true;
}

u32 pmm_refs(u32 addr) {
    if (!refs) return 0;
    u32 f = addr / PAGE_SIZE;
    if (addr % PAGE_SIZE || f >= limit_frame || !is_used(f)) return 0;
    return refs[f];
}

void pmm_stats(struct pmm_stats *st) {
    st->total_frames  = total_frames;
    st->used_frames   = used_frames;
    st->managed_limit = limit_frame * PAGE_SIZE;
}
