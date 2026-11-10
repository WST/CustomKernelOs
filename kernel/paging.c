/* Страничная адресация (paging), 32 бит, без PAE.
 *
 * Виртуальный адрес делится так:
 *   [31..22] индекс в каталоге страниц (1024 записи, каждая — 4 МБ)
 *   [21..12] индекс в таблице страниц  (1024 записи, каждая — 4 КБ)
 *   [11..0]  смещение внутри страницы
 *
 * Карта виртуальной памяти MyOS (одинакова в каждом адресном пространстве,
 * кроме средней части):
 *   0x00000000 - 0x00000FFF   НЕ отображена: разыменование NULL -> page fault
 *   0x00001000 - 0x003FFFFF   1:1, страницы по 4 КБ  (ядро, стек, VGA, BIOS)   ┐ только
 *   0x00400000 - 0x3FFFFFFF   1:1, страницы по 4 МБ  (вся RAM до 1 ГБ)         │ ядро
 *   0x40000000 - 0xBFFFFFFF   программа: у каждого процесса СВОЯ              ← PG_USER
 *   0xD0000000 - 0xD3FFFFFF   куча ядра                                        ┘
 *
 * Каталог процесса = копия «ядерных» записей каталога ядра + свои записи для
 * 1..3 ГБ. Таблицы страниц ядра общие, поэтому ядро видно одинаково везде —
 * но без бита PG_USER программа к нему прикоснуться не может. */
#include "paging.h"
#include "pmm.h"
#include "heap.h"
#include "string.h"
#include "smp.h"

#define PD_INDEX(v) ((v) >> 22)
#define PT_INDEX(v) (((v) >> 12) & 0x3FF)
#define FRAME(e)    ((e) & 0xFFFFF000)
#define HUGE_SIZE   0x400000
#define USER_PDE_FIRST PD_INDEX(USER_BASE)   /* 256 */
#define USER_PDE_END   PD_INDEX(USER_TOP)    /* 768 */

static u32 page_dir[1024]    __attribute__((aligned(4096)));   /* каталог ядра */
static u32 first_table[1024] __attribute__((aligned(4096)));   /* первые 4 МБ */
static u32 identity_limit;
static u32 tables;


static inline void invlpg(u32 addr) { __asm__ volatile ("invlpg (%0)" : : "r"(addr) : "memory"); }
static inline bool is_user_pde(u32 i) { return i >= USER_PDE_FIRST && i < USER_PDE_END; }

/* найти (и при create=true создать) запись таблицы страниц для virt в каталоге dir.
 * Все физические кадры ниже 1 ГБ, а он отображён 1:1 — значит, к таблицам
 * можно обращаться прямо по физическому адресу. */
static u32 *get_pte(u32 *dir, u32 virt, bool create, bool user) {
    u32 *pde = &dir[PD_INDEX(virt)];
    if (*pde & PG_HUGE) return NULL;
    if (!(*pde & PG_PRESENT)) {
        if (!create) return NULL;
        u32 frame = pmm_alloc();
        if (!frame) return NULL;
        memset((void *)frame, 0, PAGE_SIZE);
        *pde = frame | PG_PRESENT | PG_WRITE | (user ? PG_USER : 0);
        tables++;
    }
    return &((u32 *)FRAME(*pde))[PT_INDEX(virt)];
}

void paging_init(void) {
    struct pmm_stats st;
    pmm_stats(&st);

    identity_limit = (st.managed_limit + HUGE_SIZE - 1) & ~(HUGE_SIZE - 1);
    if (identity_limit < HUGE_SIZE) identity_limit = HUGE_SIZE;
    if (identity_limit > IDENTITY_MAX) identity_limit = IDENTITY_MAX;

    memset(page_dir, 0, sizeof(page_dir));
    for (u32 i = 0; i < 1024; i++)
        first_table[i] = (i * PAGE_SIZE) | PG_PRESENT | PG_WRITE;
    first_table[0] = 0;                                    /* ловушка для NULL */
    page_dir[0] = (u32)first_table | PG_PRESENT | PG_WRITE;
    tables = 1;
    for (u32 a = HUGE_SIZE; a < identity_limit; a += HUGE_SIZE)
        page_dir[PD_INDEX(a)] = a | PG_PRESENT | PG_WRITE | PG_HUGE;

    /* Таблицы страниц для всей кучи создаём сразу. Иначе таблица, появившаяся
     * позже, попала бы только в каталог ядра, а каталоги уже запущенных
     * процессов о ней бы не узнали — и ядро упало бы внутри системного вызова. */
    for (u32 a = HEAP_START; a < HEAP_MAX; a += HUGE_SIZE)
        get_pte(page_dir, a, true, false);


    __asm__ volatile (
        "mov %0, %%cr3\n\t"
        "mov %%cr4, %%eax\n\t"
        "or  $0x10, %%eax\n\t"          /* CR4.PSE: разрешить страницы 4 МБ */
        "mov %%eax, %%cr4\n\t"
        "mov %%cr0, %%eax\n\t"
        "or  $0x80000000, %%eax\n\t"    /* CR0.PG: включить paging */
        "mov %%eax, %%cr0\n\t"
        : : "r"(page_dir) : "eax", "memory");
}

/* ---------------------------------------------------------------- каталог ядра */

int vmm_map(u32 virt, u32 phys, u32 flags) {
    u32 *pte = get_pte(page_dir, virt, true, false);
    if (!pte) return -1;
    *pte = FRAME(phys) | (flags & 0xFFF) | PG_PRESENT;
    invlpg(virt);
    smp_kernel_mapping_changed();
    return 0;
}

u32 vmm_unmap(u32 virt) {
    u32 *pte = get_pte(page_dir, virt, false, false);
    if (!pte || !(*pte & PG_PRESENT)) return 0;
    u32 phys = FRAME(*pte);
    *pte = 0;
    invlpg(virt);
    smp_kernel_mapping_changed();
    return phys;
}

bool vmm_lookup(u32 dir_phys, u32 virt, u32 *phys, u32 *flags) {
    u32 *dir = (u32 *)dir_phys;
    u32 pde = dir[PD_INDEX(virt)];
    if (!(pde & PG_PRESENT)) return false;
    if (pde & PG_HUGE) {
        if (phys) *phys = (pde & 0xFFC00000) | (virt & 0x3FFFFF);
        if (flags) *flags = pde & 0xFFF;
        return true;
    }
    u32 pte = ((u32 *)FRAME(pde))[PT_INDEX(virt)];
    if (!(pte & PG_PRESENT)) return false;
    if (phys) *phys = FRAME(pte) | (virt & 0xFFF);
    if (flags) *flags = (pte & pde & PG_USER) | (pte & pde & PG_WRITE) | PG_PRESENT;
    return true;
}

bool vmm_translate(u32 virt, u32 *phys) { return vmm_lookup(vmm_current_dir(), virt, phys, NULL); }

/* ---------------------------------------------------------------- процессы */

u32 vmm_kernel_dir(void)  { return (u32)page_dir; }
u32 vmm_current_dir(void) { u32 dir; __asm__ volatile("mov %%cr3,%0":"=r"(dir)); return dir; }

u32 vmm_create_dir(void) {
    u32 frame = pmm_alloc();
    if (!frame) return 0;
    u32 *dir = (u32 *)frame;
    for (u32 i = 0; i < 1024; i++) dir[i] = is_user_pde(i) ? 0 : page_dir[i];
    return frame;
}

int vmm_map_in(u32 dir, u32 virt, u32 phys, u32 flags) {
    bool user = flags & PG_USER;
    if (user && (virt < USER_BASE || virt >= USER_TOP)) return -1;
    u32 *pte = get_pte((u32 *)dir, virt, true, user);
    if (!pte) return -1;
    bool replace=(*pte & PG_PRESENT)!=0;
    *pte = FRAME(phys) | (flags & 0xFFF) | PG_PRESENT;
    if (dir == vmm_current_dir()) invlpg(virt);
    if (replace) smp_user_tlb_flush(dir);
    return 0;
}

/* убрать страницу из каталога процесса -> физ. адрес (0, если её не было). Кадр НЕ освобождается. */
u32 vmm_unmap_in(u32 dir, u32 virt) {
    u32 *pte = get_pte((u32 *)dir, virt, false, false);
    if (!pte || !(*pte & PG_PRESENT)) return 0;
    u32 phys = FRAME(*pte);
    *pte = 0;
    if (dir == vmm_current_dir()) invlpg(virt);
    smp_user_tlb_flush(dir);
    return phys;
}

/* Для fork(): новый каталог с КОПИЕЙ всей пользовательской памяти.
 * Каждая страница копируется сразу (без copy-on-write — проще, но медленнее).
 * -> физ. адрес каталога или 0, если не хватило памяти. */
u32 vmm_clone_user(u32 src_phys, u32 *pages) {
    u32 dst_phys = vmm_create_dir();
    if (!dst_phys) return 0;
    u32 *src = (u32 *)src_phys;
    *pages = 0;
    for (u32 i = USER_PDE_FIRST; i < USER_PDE_END; i++) {
        if (!(src[i] & PG_PRESENT)) continue;
        if (i >= PD_INDEX(SHM_BASE) && i < PD_INDEX(SHM_END)) continue;   /* общая память не наследуется */
        u32 *table = (u32 *)FRAME(src[i]);
        for (u32 j = 0; j < 1024; j++) {
            if (!(table[j] & PG_PRESENT)) continue;
            u32 frame = pmm_alloc();
            if (!frame) { vmm_destroy_dir(dst_phys); return 0; }
            memcpy((void *)frame, (void *)FRAME(table[j]), PAGE_SIZE);
            u32 va = (i << 22) | (j << 12);
            if (vmm_map_in(dst_phys, va, frame, table[j] & 0xFFF)) {
                pmm_free(frame);
                vmm_destroy_dir(dst_phys);
                return 0;
            }
            (*pages)++;
        }
    }
    return dst_phys;
}

/* Освободить всю пользовательскую часть каталога и сам каталог.
 * Ядерные таблицы общие — их не трогаем. Вызывать НЕ из этого же каталога. */
u32 vmm_destroy_dir(u32 dir_phys) {
    u32 *dir = (u32 *)dir_phys, pages = 0;
    if (dir_phys == (u32)page_dir || dir_phys == vmm_current_dir()) return 0;
    for (u32 i = USER_PDE_FIRST; i < USER_PDE_END; i++) {
        if (!(dir[i] & PG_PRESENT)) continue;
        u32 *table = (u32 *)FRAME(dir[i]);
        for (u32 j = 0; j < 1024; j++)
            if (table[j] & PG_PRESENT) { pmm_free(FRAME(table[j])); pages++; }
        pmm_free((u32)table);
        tables--;
    }
    pmm_free(dir_phys);
    return pages;
}

void vmm_switch(u32 dir) {
    if (dir == vmm_current_dir()) return;
    __asm__ volatile ("mov %0, %%cr3" : : "r"(dir) : "memory");   /* заодно сбрасывает TLB */
}

u32 paging_identity_limit(void) { return identity_limit; }
u32 paging_table_count(void)    { return tables; }
