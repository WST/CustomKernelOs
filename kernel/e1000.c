/* Драйвер сетевой карты Intel 82540EM (e1000) — её QEMU даёт по умолчанию.
 *
 * Карта работает с памятью сама (DMA). Мы даём ей два кольца дескрипторов:
 *   RX — 32 пустых буфера по 2 КБ; карта кладёт туда пришедшие кадры и ставит
 *        в дескрипторе бит DD («готово»). Мы забираем кадр и возвращаем буфер.
 *   TX — кадры на отправку; мы пишем буфер, длину и двигаем «хвост» (TDT),
 *        карта отправляет и тоже ставит DD.
 * Регистры карты — это память (MMIO) по адресу из PCI BAR0. Она выше 1 ГБ,
 * поэтому отображаем её в ядро отдельно, как видеопамять. Прерывания не
 * используем: сетевой поток ядра сам опрашивает кольцо раз в 10 мс. */
#include "e1000.h"
#include "pci.h"
#include "paging.h"
#include "pmm.h"
#include "string.h"

#define MMIO_VIRT 0xE1000000u          /* сюда отображаем регистры (128 КБ) */

#define REG_CTRL   0x0000
#define REG_STATUS 0x0008
#define REG_IMC    0x00D8
#define REG_RCTL   0x0100
#define REG_TCTL   0x0400
#define REG_TIPG   0x0410
#define REG_RDBAL  0x2800
#define REG_RDBAH  0x2804
#define REG_RDLEN  0x2808
#define REG_RDH    0x2810
#define REG_RDT    0x2818
#define REG_TDBAL  0x3800
#define REG_TDBAH  0x3804
#define REG_TDLEN  0x3808
#define REG_TDH    0x3810
#define REG_TDT    0x3818
#define REG_MTA    0x5200
#define REG_RAL    0x5400
#define REG_RAH    0x5404

#define CTRL_SLU   (1u << 6)            /* «link up» */
#define RCTL_EN    (1u << 1)
#define RCTL_UPE   (1u << 3)            /* принимать любые unicast (QEMU так проще) */
#define RCTL_BAM   (1u << 15)           /* принимать широковещательные */
#define RCTL_SECRC (1u << 26)           /* отрезать CRC */
#define TCTL_EN    (1u << 1)
#define TCTL_PSP   (1u << 3)
#define CMD_EOP    0x01
#define CMD_IFCS   0x02
#define CMD_RS     0x08
#define DD         0x01

#define NRX 32
#define NTX 32
#define BUF 2048

struct rx_desc { u64 addr; u16 len; u16 csum; u8 status; u8 errors; u16 special; } __attribute__((packed));
struct tx_desc { u64 addr; u16 len; u8 cso; u8 cmd; u8 status; u8 css; u16 special; } __attribute__((packed));

static volatile u8 *mmio;
static volatile struct rx_desc *rx;
static volatile struct tx_desc *tx;
static u8  *rx_buf[NRX], *tx_buf[NTX];
static u32 rx_cur, tx_cur;
static u8  mac[6];
static u32 n_rx, n_tx, b_rx, b_tx;

static inline u32  rd(u32 reg)        { return *(volatile u32 *)(mmio + reg); }
static inline void wr(u32 reg, u32 v) { *(volatile u32 *)(mmio + reg) = v; }

/* память для DMA: физический адрес = виртуальному (RAM < 1 ГБ отображена 1:1) */
static void *dma_page(void) {
    u32 p = pmm_alloc();
    if (p) memset((void *)p, 0, PAGE_SIZE);
    return (void *)p;
}

bool e1000_init(void) {
    struct pci_dev d;
    if (!pci_find_device(0x8086, 0x100E, &d)) return false;      /* 82540EM */
    u32 cmd = pci_read(d.bus, d.dev, d.func, 0x04);
    pci_write(d.bus, d.dev, d.func, 0x04, cmd | 0x6);           /* память + bus master (для DMA) */

    u32 phys = d.bar0 & ~0xFu;
    for (u32 off = 0; off < 0x20000; off += PAGE_SIZE)
        if (vmm_map(MMIO_VIRT + off, phys + off, PG_WRITE | 0x18)) return false;   /* 0x18 = без кэша: это регистры, а не память */
    mmio = (volatile u8 *)MMIO_VIRT;

    wr(REG_IMC, 0xFFFFFFFF);                                    /* прерывания не нужны */
    wr(REG_CTRL, rd(REG_CTRL) | CTRL_SLU);
    for (int i = 0; i < 128; i++) wr(REG_MTA + i * 4, 0);

    /* MAC QEMU уже записал в RAL/RAH */
    u32 lo = rd(REG_RAL), hi = rd(REG_RAH);
    for (int i = 0; i < 4; i++) mac[i] = lo >> (8 * i);
    mac[4] = hi; mac[5] = hi >> 8;

    /* приём: кольцо из 32 дескрипторов, у каждого свой буфер 2 КБ (по 2 на страницу) */
    rx = dma_page();
    for (int i = 0; i < NRX; i += 2) {
        u8 *page = dma_page();
        if (!page || !rx) return false;
        rx_buf[i] = page;
        rx_buf[i + 1] = page + BUF;
    }
    for (int i = 0; i < NRX; i++) { rx[i].addr = (u32)rx_buf[i]; rx[i].status = 0; }
    wr(REG_RDBAL, (u32)rx);
    wr(REG_RDBAH, 0);
    wr(REG_RDLEN, NRX * sizeof(struct rx_desc));
    wr(REG_RDH, 0);
    wr(REG_RDT, NRX - 1);
    wr(REG_RCTL, RCTL_EN | RCTL_UPE | RCTL_BAM | RCTL_SECRC);  /* размер буфера 2048 — это 00 */

    /* передача */
    tx = dma_page();
    for (int i = 0; i < NTX; i += 2) {
        u8 *page = dma_page();
        if (!page || !tx) return false;
        tx_buf[i] = page;
        tx_buf[i + 1] = page + BUF;
    }
    for (int i = 0; i < NTX; i++) { tx[i].addr = (u32)tx_buf[i]; tx[i].status = DD; tx[i].cmd = 0; }
    wr(REG_TDBAL, (u32)tx);
    wr(REG_TDBAH, 0);
    wr(REG_TDLEN, NTX * sizeof(struct tx_desc));
    wr(REG_TDH, 0);
    wr(REG_TDT, 0);
    wr(REG_TCTL, TCTL_EN | TCTL_PSP | (0x10 << 4) | (0x40 << 12));
    wr(REG_TIPG, 0x0060200A);
    return true;
}

void e1000_mac(u8 out[6]) { memcpy(out, mac, 6); }

int e1000_send(const void *frame, u32 len) {
    if (!mmio || len > BUF) return -1;
    volatile struct tx_desc *t = &tx[tx_cur];
    if (!(t->status & DD)) return -1;                   /* карта ещё не отправила прошлый кадр */
    memcpy(tx_buf[tx_cur], frame, len);
    if (len < 60) { memset(tx_buf[tx_cur] + len, 0, 60 - len); len = 60; }   /* минимум Ethernet */
    t->len = len;
    t->cmd = CMD_EOP | CMD_IFCS | CMD_RS;
    t->status = 0;
    tx_cur = (tx_cur + 1) % NTX;
    wr(REG_TDT, tx_cur);                                /* «отправляй всё до сюда» */
    n_tx++; b_tx += len;
    return 0;
}

int e1000_receive(void *buf, u32 max) {
    if (!mmio) return 0;
    volatile struct rx_desc *r = &rx[rx_cur];
    if (!(r->status & DD)) return 0;
    u32 len = r->len;
    if (len > max) len = max;
    memcpy(buf, rx_buf[rx_cur], len);
    r->status = 0;
    wr(REG_RDT, rx_cur);                                /* буфер снова свободен */
    rx_cur = (rx_cur + 1) % NRX;
    n_rx++; b_rx += len;
    return len;
}

void e1000_stats(u32 *a, u32 *b, u32 *c, u32 *d) { *a = n_rx; *b = n_tx; *c = b_rx; *d = b_tx; }
