#include "module.h"
#include "pci.h"
#include "paging.h"
#include "pmm.h"
#include "heap.h"
#include "string.h"
#include "timer.h"
#include "task.h"
#include "klog.h"
#include "wlan.h"
#include "wlan_frame.h"
#include "iwl_fw.h"

/* Intel Wireless-N 1000: softmac, прошивка API 5, только 2.4 ГГц.
 * Прерывание не включается: линия INTx на Z570 сидит в IOAPIC, а ядро
 * программирует только 8259. Карта опрашивается по CSR_INT. */

MYOS_MODULE(iwl1000);
MYOS_PCI_ID(0x8086, 0x0083);
MYOS_PCI_ID(0x8086, 0x0084);

/* 0xE5000000 занят окном ACPI, которое smp_init снимает после загрузки модулей,
 * и регистрами NVIDIA. Иначе поток wlan и wifid ловят page fault на CSR. */
#define MMIO_VIRT 0xE6000000u
#define MMIO_BYTES 0x4000u
#define CSR_HW_IF 0x000
#define CSR_INT 0x008
#define CSR_INT_MASK 0x00c
#define CSR_RESET 0x020
#define CSR_GP 0x024
#define CSR_HW_REV 0x028
#define CSR_EEPROM 0x02c
#define CSR_EEPROM_GP 0x030
#define CSR_OTP_GP 0x034
#define CSR_INT_FH_RX 0x80000000u
#define CSR_RESET_SW 0x80u
#define CSR_RESET_MASTER_DIS 0x100u
#define CSR_RESET_STOP 0x200u
#define CSR_RESET_LINK_PM_DIS 0x80000000u
#define CSR_GP_CLOCK 1u
#define CSR_GP_INIT_DONE 4u
#define CSR_GP_ACCESS 8u
#define CSR_GP_SLEEP 0x10u
#define CSR_GP_RFKILL 0x08000000u
#define CSR_HW_IF_HAP_WAKE 0x00080000u
#define CSR_HW_IF_EEPROM_SEM 0x00200000u
#define CSR_HW_IF_NIC_READY 0x00400000u
#define CSR_HW_IF_PREPARE 0x08000000u
#define CSR_OTP_SELECT 0x00010000u
#define CSR_OTP_RELATIVE 0x00020000u
#define CSR_MBOX_SET 0x088
#define CSR_MBOX_OS_ALIVE 0x20u
#define CSR_UCODE_GP1 0x054
#define CSR_UCODE_GP1_CLR 0x05c
#define UCODE_GP1_RFKILL (1u << 1)
#define UCODE_GP1_CMD_BLOCKED (1u << 2)
#define CSR_GIO_CHICKEN 0x100
#define CSR_GIO_DIS_L0S_TIMER 0x20000000u
#define CSR_GIO_L1A_NO_L0S 0x00800000u
#define CSR_ANA_PLL 0x20c
#define CSR_ANA_PLL_1000 0x00880300u
#define CSR_HPET 0x240
#define CSR_HPET_VAL 0xFFFF0000u
#define CSR_DBG_LINK_PWR 0x250
#define FH_KW 0x197c
#define FH_CBBC 0x19d0
#define FH_SRVC_ADDR 0x19c8
#define FH_TFDIB0(ch) (0x1900u + 8u * (ch))
#define FH_TFDIB1(ch) (0x1904u + 8u * (ch))
#define CSR_FH_INT 0x010
#define FH_RX_STTS 0x1bc0
#define FH_RX_BASE 0x1bc4
#define FH_RX_WPTR 0x1bc8
#define FH_RX_CFG 0x1c00
#define FH_TX_CFG(ch) (0x1d00u + 0x20u * (ch))
#define FH_TX_STS(ch) (0x1d00u + 0x20u * (ch) + 8u)
#define FH_TSSR 0x1eb0
#define HBUS_MEM_RADDR 0x40c
#define HBUS_MEM_WADDR 0x410
#define HBUS_MEM_WDAT 0x418
#define HBUS_MEM_RDAT 0x41c
#define HBUS_PRPH_WADDR 0x444
#define HBUS_PRPH_RADDR 0x448
#define HBUS_PRPH_WDAT 0x44c
#define HBUS_PRPH_RDAT 0x450
#define APMG_CLK_EN 0x3004
#define APMG_PS 0x300c
#define APMG_PCI_STT 0x3010
#define APMG_CLK_DMA 0x200u
#define APMG_PS_RESET 0x04000000u
#define APMG_PS_SRC 0x03000000u
#define APMG_L1_OFF 0x800u
#define CSR_EEPROM_GP_OWNER 0x180u
#define CSR_OTP_ECC_FIX 0x00100000u
#define CSR_OTP_ECC_BAD 0x00200000u
#define HBUS_WRPTR 0x460
#define FH_TX_CHICKEN 0x1e98
#define APMG_DIGITAL_SVR 0x3058
#define SCD_BASE 0xa02c00u
#define SCD_CTX 0x600u
#define SCD_CTX_DWORDS (520u / 4u)
#define SCD_QCTX(q) (0x600u + (q) * 8u)
#define SCD_TRANS(q) (0x7e0u + (q) * 2u)
#define Q_CMD 4
#define Q_TX 0
#define Q_N 20
#define RING_N 256
#define SCHED_STRIDE 320
#define RX_N 16
#define STA_BCAST 15
#define STA_AP 0

struct rxon {
    u8 node[6]; u16 res1; u8 bssid[6]; u16 res2; u8 wlap[6]; u16 res3;
    u8 dev_type, air; u16 rx_chain; u8 ofdm, cck; u16 assoc;
    u32 flags, filter; u16 channel;
    u8 ht1, ht2, ht3, res5; u16 acq, res6;
} __attribute__((packed));

static volatile u8 *mmio;
static struct pci_dev dev;
static u8 mac[6];
static int max_ch = 11;
static int alive, rfkill_noted, running;
static u32 pages[96];
static int npages;
static u8 *fw_buf;
static struct iwl_fw fw;
static u32 *rx_bd;
static volatile u32 *rx_closed;
static u8 *rx_data[RX_N];
static u32 rx_read;
static u8 *cmd_ring, *data_ring, *sched;
static u32 cmd_w, data_w;
static u8 *cmd_body[16], *data_body[16];
static u32 cmd_body_i, data_body_i;
static u32 rx_wptr;
static int cur_ch = 1;
static u8 ap_mac[6];
static int ap_set;
static u8 join_bssid[6], join_tsf[8];
static volatile u16 join_aid, join_bint;
static int join_set;
static volatile u32 data_acked;
static u32 xtal;
static u16 temp_off;
static u8 tx_ant = 1, rx_ant = 3;
static u16 rfcfg;
static u8 calib_blob[4][1024];
static u16 calib_n[4];
static int calib_got, rx_frames, rx_notes;
static u8 rx_last_cmd, reply_cmd, err_cmd;
static u32 err_type, err_info, err_ptr;
static int fw_dumped;
static u8 rx_hist[4][12], rx_hist_slot[4];
static u32 rx_hist_n;
static volatile int want_calib, scan_done, rx_hold, reply_seen, reply_err;
static volatile u8 want_slot = 0xff;
static int have_phy, last_rssi;
static volatile int qlock;
static void *mod;

static u32 rd(u32 r) { return *(volatile u32 *)(mmio + r); }
static void wr(u32 r, u32 v) { *(volatile u32 *)(mmio + r) = v; }

static void *dma_page(void) {
    if (npages >= (int)(sizeof(pages) / sizeof(pages[0]))) return NULL;
    u32 p = pmm_alloc();
    if (!p) return NULL;
    pages[npages++] = p;
    memset((void *)p, 0, 4096);
    return (void *)p;
}

/* Указатель записи — 8 бит, слот считается линейно. Кольцо обязано быть одним куском. */
static void *dma_contig(u32 n) {
    if (!n || npages + (int)n > (int)(sizeof(pages) / sizeof(pages[0]))) return NULL;
    for (int attempt = 0; attempt < 4; attempt++) {
        int base = npages;
        u32 first = 0;
        int ok = 1;
        for (u32 i = 0; i < n; i++) {
            u32 p = pmm_alloc();
            if (!p) { ok = 0; break; }
            pages[npages++] = p;
            memset((void *)p, 0, 4096);
            if (!i) first = p;
            else if (p != first + i * 4096u) ok = 0;
        }
        if (ok && first) return (void *)first;
        while (npages > base) pmm_free(pages[--npages]);
    }
    return NULL;
}

static int poll_ms(u32 reg, u32 mask, u32 want, int ms) {
    for (int i = 0; i < ms; i++) {
        if ((rd(reg) & mask) == want) return 1;
        sleep_ms(1);
    }
    return (rd(reg) & mask) == want;
}

/* Владение у хоста. Иначе тактовый домен после сброса не поднимается. */
static void claim_nic(void) {
    for (int n = 0; n < 5; n++) {
        wr(CSR_HW_IF, rd(CSR_HW_IF) | CSR_HW_IF_NIC_READY);
        if (poll_ms(CSR_HW_IF, CSR_HW_IF_NIC_READY, CSR_HW_IF_NIC_READY, 25)) {
            wr(CSR_MBOX_SET, rd(CSR_MBOX_SET) | CSR_MBOX_OS_ALIVE);
            return;
        }
        wr(CSR_HW_IF, rd(CSR_HW_IF) | CSR_HW_IF_PREPARE);
    }
}

/* Сброс гасит INIT_DONE, и пока его не выставить снова, бит такта остаётся нулём.
 * У 1000 PLL пишется до перехода в рабочее питание, иначе такт тоже не встаёт. */
static int wake_clock(void) {
    wr(CSR_GIO_CHICKEN, rd(CSR_GIO_CHICKEN) | CSR_GIO_DIS_L0S_TIMER | CSR_GIO_L1A_NO_L0S);
    wr(CSR_HPET, rd(CSR_HPET) | CSR_HPET_VAL);
    wr(CSR_HW_IF, rd(CSR_HW_IF) | CSR_HW_IF_HAP_WAKE);
    wr(CSR_DBG_LINK_PWR, rd(CSR_DBG_LINK_PWR) | CSR_RESET_LINK_PM_DIS);
    wr(CSR_ANA_PLL, rd(CSR_ANA_PLL) | CSR_ANA_PLL_1000);
    wr(CSR_GP, rd(CSR_GP) | CSR_GP_INIT_DONE | CSR_GP_ACCESS);
    int clock = 0;
    for (int i = 0; i < 50; i++) {
        u32 gp = rd(CSR_GP);
        if (gp & CSR_GP_CLOCK) clock = 1;
        if (clock && !(gp & CSR_GP_SLEEP)) return 1;
        sleep_ms(1);
    }
    return clock;
}

static void prph_write(u32 addr, u32 val) {
    wr(HBUS_PRPH_WADDR, (addr & 0x000fffffu) | (3u << 24));
    wr(HBUS_PRPH_WDAT, val);
}

static u32 prph_read(u32 addr) {
    wr(HBUS_PRPH_RADDR, (addr & 0x000fffffu) | (3u << 24));
    return rd(HBUS_PRPH_RDAT);
}

static int power_d0(void) {
    if (!(pci_read(dev.bus, dev.dev, dev.func, 4) & (1u << 20))) return 1;
    unsigned cap = pci_read(dev.bus, dev.dev, dev.func, 0x34) & 0xfc;
    for (unsigned n = 0; cap && n < 48; n++) {
        if (cap < 0x40 || cap > 0xf8) return 0;
        u32 header = pci_read(dev.bus, dev.dev, dev.func, cap);
        if ((header & 255) == 1) {
            u32 pm = pci_read(dev.bus, dev.dev, dev.func, cap + 4);
            if (pm & 3) pci_write(dev.bus, dev.dev, dev.func, cap + 4, pm & ~0x8003u);
            sleep_ms(10);
            return !(pci_read(dev.bus, dev.dev, dev.func, cap + 4) & 3);
        }
        cap = (header >> 8) & 0xfc;
    }
    return !cap;
}

static int map_bar(void) {
    u32 bar = dev.bar0, phys = bar & ~15u;
    if ((bar & 1) || !phys) {
        klogf("IWL: BAR0 is empty (%x); skipping the card\n", bar);
        return 0;
    }
    for (u32 off = 0; off < MMIO_BYTES; off += 4096)
        if (vmm_map(MMIO_VIRT + off, phys + off, PG_WRITE | 0x18)) return 0;
    mmio = (volatile u8 *)MMIO_VIRT;
    u32 cmd = pci_read(dev.bus, dev.dev, dev.func, 4);
    /* INTx этой карты приходит в IOAPIC, а ядро слушает только 8259.
     * Непогашенный пин крутит прерывание без конца и загрузка замирает. */
    pci_write(dev.bus, dev.dev, dev.func, 4, cmd | 6u | (1u << 10));
    return 1;
}

/* Байт образа, с которого читаются MAC и флаги каналов. 1000 кладёт NVM в OTP. */
static u32 nvm_base;

/* Бит 1 регистра — команда, его ставить нельзя: чтение тогда не поднимает бит готовности.
 * В битах 15:2 лежит номер слова, поэтому байтовый адрес сдвигается на 1 и маскируется. */
static int nvm_word(u32 byte_addr, u16 *out) {
    for (int attempt = 0; attempt < 2; attempt++) {
        wr(CSR_EEPROM, (byte_addr << 1) & 0xfffcu);
        for (int i = 0; i < 50000; i++) {
            u32 v = rd(CSR_EEPROM);
            if (!(v & 1)) continue;
            u32 otp = rd(CSR_OTP_GP);
            if (otp & CSR_OTP_ECC_BAD) {
                wr(CSR_OTP_GP, otp | CSR_OTP_ECC_BAD);
                break;
            }
            if (otp & CSR_OTP_ECC_FIX) wr(CSR_OTP_GP, rd(CSR_OTP_GP) | CSR_OTP_ECC_FIX);
            *out = (u16)(v >> 16);
            return 1;
        }
    }
    return 0;
}

static int mac_at(u32 base) {
    u8 tmp[6];
    for (int i = 0; i < 3; i++) {
        u16 w;
        if (!nvm_word(base + (u32)(0x15 + i) * 2u, &w)) return 0;
        tmp[i * 2] = (u8)w;
        tmp[i * 2 + 1] = (u8)(w >> 8);
    }
    unsigned any = 0, all = 255;
    for (int i = 0; i < 6; i++) { any |= tmp[i]; all &= tmp[i]; }
    if (!any || all == 255 || (tmp[0] & 1)) return 0;
    memcpy(mac, tmp, 6);
    nvm_base = base;
    return 1;
}

static int read_mac(void) {
    if (mac_at(nvm_base)) return 1;
    if (nvm_base && mac_at(0)) return 1;
    u16 w0 = 0xffff, at = 0xffff, img = 0xffff;
    nvm_word(0, &w0);
    nvm_word(0x2a, &at);
    nvm_word(nvm_base + 0x2a, &img);
    klogf("IWL: OTP has no MAC, base=%x w0=%x at2a=%x atimg=%x reg=%x\n",
          nvm_base, w0, at, img, rd(CSR_EEPROM));
    return 0;
}

static void read_channels(void) {
    int allow = 0;
    for (int i = 0; i < 13; i++) {
        u16 f = 0;
        if (!nvm_word(nvm_base + (u32)(0x35 + i) * 2u, &f)) continue;
        if (f && f != 0xffff && (f & 1)) allow |= 1 << (i + 1);
    }
    int n = 0;
    for (int i = 1; i <= 11; i++) if (allow & (1 << i)) n++;
    max_ch = 11;
    if (n >= 11 && (allow & (1 << 12)) && (allow & (1 << 13))) max_ch = 13;
}

/* Бит 27 равен 1, когда тумблер разрешает радио. Ноль — аппаратная блокировка. */
static int hw_blocked(void) { return (rd(CSR_GP) & CSR_GP_RFKILL) == 0; }

static void prph_write(u32 addr, u32 val);
static u32 prph_read(u32 addr);

static int nic_reset(void) {
    claim_nic();
    wr(CSR_RESET, CSR_RESET_STOP);
    if (!poll_ms(CSR_RESET, CSR_RESET_MASTER_DIS, CSR_RESET_MASTER_DIS, 100)) {
        klogf("IWL: master did not stop, csr=%x gp=%x\n", rd(CSR_RESET), rd(CSR_GP));
        return 0;
    }
    wr(CSR_RESET, CSR_RESET_SW);
    sleep_ms(6);
    wr(CSR_INT, 0xffffffffu);
    wr(CSR_INT_MASK, 0);
    claim_nic();
    if (wake_clock()) {
        /* 1.32 В. На 1000 иначе шумит DC-DC и прошивка молчит после калибровки. */
        u32 svr = prph_read(APMG_DIGITAL_SVR);
        prph_write(APMG_DIGITAL_SVR, (svr & ~0x1e0u) | (3u << 5));
        return 1;
    }
    klogf("IWL: reset did not finish, csr=%x gp=%x hwif=%x pll=%x\n",
          rd(CSR_RESET), rd(CSR_GP), rd(CSR_HW_IF), rd(CSR_ANA_PLL));
    return 0;
}

/* Цепочка OTP: слово блока указывает на следующий. Образ — предыдущий блок,
 * со следующего слова. Нулевой указатель в первом слове значит «образ с нуля». */
static void nvm_walk(void) {
    u16 link = 0;
    u32 next = 0;
    int first = 1;
    for (int used = 0; used <= 3;) {
        u32 valid = next;
        next = (u32)link * 2u;
        if (!nvm_word(next, &link)) {
            klogf("IWL: OTP link read failed at %x, reg=%x\n", next, rd(CSR_EEPROM));
            nvm_base = 0;
            return;
        }
        if (!link) {
            nvm_base = first ? 0 : valid + 2;
            return;
        }
        first = 0;
        used++;
    }
    klogf("IWL: OTP link list has no end\n");
    nvm_base = 0;
}

static int nvm_semaphore(void) {
    for (int n = 0; n < 40; n++) {
        wr(CSR_HW_IF, rd(CSR_HW_IF) | CSR_HW_IF_EEPROM_SEM);
        if (poll_ms(CSR_HW_IF, CSR_HW_IF_EEPROM_SEM, CSR_HW_IF_EEPROM_SEM, 5)) return 1;
    }
    return 0;
}

/* Питание OTP поднимается импульсом сброса в блоке APMG, иначе порт чтения молчит. */
static void nvm_open(void) {
    wr(CSR_GP, rd(CSR_GP) | CSR_GP_INIT_DONE | CSR_GP_ACCESS);
    prph_write(APMG_CLK_EN, prph_read(APMG_CLK_EN) | APMG_CLK_DMA);
    prph_write(APMG_PCI_STT, prph_read(APMG_PCI_STT) | APMG_L1_OFF);
    prph_write(APMG_PS, prph_read(APMG_PS) | APMG_PS_RESET);
    sleep_ms(1);
    u32 ps = prph_read(APMG_PS);
    prph_write(APMG_PS, ps & ~APMG_PS_RESET & ~APMG_PS_SRC);
    if (!nvm_semaphore())
        klogf("IWL: OTP semaphore not acquired, hwif=%x\n", rd(CSR_HW_IF));
    wr(CSR_EEPROM_GP, rd(CSR_EEPROM_GP) & ~CSR_EEPROM_GP_OWNER);
    (void)rd(CSR_OTP_GP);
    wr(CSR_OTP_GP, (rd(CSR_OTP_GP) & ~CSR_OTP_RELATIVE) | CSR_OTP_ECC_FIX | CSR_OTP_ECC_BAD);
    nvm_walk();
}

static u8 *chunk_page;

/* Сервисный канал читает один буфер из TFDIB, а не из кольца TFD.
 * Без младших бит «буфер действителен» передача не начинается. */
static int load_chunk(u32 dest, const u8 *src, u32 n) {
    if (!chunk_page) chunk_page = dma_page();
    if (!chunk_page || n == 0 || n > 4092) return 0;
    wr(CSR_GP, rd(CSR_GP) | CSR_GP_INIT_DONE | CSR_GP_ACCESS);
    if (!poll_ms(CSR_GP, CSR_GP_CLOCK, CSR_GP_CLOCK, 25)) {
        klogf("IWL: DMA clock is down, gp=%x\n", rd(CSR_GP));
        return 0;
    }
    memcpy(chunk_page, src, n);
    __asm__ volatile("mfence" ::: "memory");
    wr(CSR_INT, 1u << 27);
    wr(CSR_FH_INT, 3u);
    wr(FH_TX_CFG(9), 0);
    wr(FH_SRVC_ADDR, dest);
    wr(FH_TFDIB0(9), (u32)(uptr)chunk_page);
    wr(FH_TFDIB1(9), n);
    wr(FH_TX_STS(9), (1u << 20) | (1u << 12) | 3u);
    wr(FH_TX_CFG(9), 0x80000000u | 0x00100000u);
    for (int i = 0; i < 100; i++) {
        if (rd(CSR_INT) & (1u << 27)) {
            wr(CSR_FH_INT, 3u);
            wr(CSR_INT, 1u << 27);
            return 1;
        }
        sleep_ms(1);
    }
    klogf("IWL: service DMA timed out dest=%x n=%u int=%x fh=%x tssr=%x sts=%x\n",
          dest, n, rd(CSR_INT), rd(CSR_FH_INT), rd(FH_TSSR), rd(FH_TX_STS(9)));
    return 0;
}

static int load_image(const struct iwl_slice *text, u32 text_dest, const struct iwl_slice *data, u32 data_dest) {
    u32 off = 0;
    while (off < text->n) {
        u32 n = text->n - off;
        if (n > 4092) n = 4092;
        if (!load_chunk(text_dest + off, text->p + off, n)) return 0;
        off += n;
    }
    off = 0;
    while (off < data->n) {
        u32 n = data->n - off;
        if (n > 4092) n = 4092;
        if (!load_chunk(data_dest + off, data->p + off, n)) return 0;
        off += n;
    }
    return 1;
}

static u8 *kw_page;
static u32 seen_ir;

static u32 sram_dword(u32 addr) {
    wr(HBUS_MEM_RADDR, addr);
    return rd(HBUS_MEM_RDAT);
}

/* 0x1bc0 — адрес статусного слова в памяти хоста, не счётчик буферов.
 * Счётчик лежит в 0x1bc8 и должен быть кратен 8. Ноль на пустом кольце —
 * «принимать некуда», поэтому уведомление ALIVE не попадает в очередь. */
static void rx_setup(void) {
    if (!rx_bd) {
        u8 *meta = dma_page();
        rx_bd = (u32 *)meta;
        rx_closed = (volatile u32 *)(meta + 256);
        for (int i = 0; i < RX_N; i++) {
            rx_data[i] = dma_page();
            rx_bd[i] = (u32)(uptr)rx_data[i] >> 8;
        }
        kw_page = dma_page();
    }
    *rx_closed = 0;
    rx_read = 0;
    wr(FH_RX_CFG, 0);
    wr(FH_KW, (u32)(uptr)kw_page >> 4);
    wr(FH_RX_STTS, (u32)(uptr)rx_closed >> 4);
    wr(FH_RX_BASE, (u32)(uptr)rx_bd >> 8);
    wr(FH_RX_CFG, 0x80000000u | (4u << 20) | 0x1000u | (0x10u << 4) | 0x8000u | 4u);
    rx_wptr = (RX_N - 1u) & ~7u;
    wr(FH_RX_WPTR, rx_wptr);
}

static int rssi_dbm(const u8 *phy) {
    u32 agc = (u32)phy[4] | ((u32)phy[5] << 8) | ((u32)phy[6] << 16) | ((u32)phy[7] << 24);
    agc = (agc >> 9) & 0x7f;
    int best = 0;
    for (int i = 0; i < 3; i++) {
        int v = phy[8 + i * 2];
        if (v > best) best = v;
    }
    return best - (int)agc - 44;
}

static void save_calib(const u8 *payload, u32 n) {
    if (!want_calib || n < 4) return;
    int slot = -1;
    if (payload[0] == 8) slot = 0;
    else if (payload[0] == 9) slot = 1;
    else if (payload[0] == 11) slot = 2;
    else if (payload[0] == 16) slot = 3;
    if (slot < 0) return;
    if (n > sizeof(calib_blob[0])) n = sizeof(calib_blob[0]);
    memcpy(calib_blob[slot], payload, n);
    calib_n[slot] = (u16)n;
    calib_got++;
}

static void take_frame(const u8 *frame, u32 n, const u8 *status) {
    u32 flags = (u32)status[0] | ((u32)status[1] << 8) | ((u32)status[2] << 16) | ((u32)status[3] << 24);
    if ((flags & 3u) != 3u) return;
    rx_frames++;
    if (n >= 30 && !memcmp(frame + 16, join_bssid, 6)) {
        u8 fc = frame[0];
        if ((fc == 0x80 || fc == 0x50) && n >= 36) {
            memcpy(join_tsf, frame + 24, 8);
            join_bint = (u16)(frame[32] | (frame[33] << 8));
        } else if ((fc == 0x10 || fc == 0x30) && frame[26] == 0 && frame[27] == 0) {
            join_aid = (u16)((frame[28] | (frame[29] << 8)) & 0x3fff);
        }
    }
    wlan_rx(frame, n, have_phy ? last_rssi : -60);
}

/* 192 несёт уровень, 193 — сам кадр. Старый 0xc3 сюда не приходит.
 * Длина считается от заголовка из 4 байт, без самого слова длины.
 * Ответ на команду узнаётся по очереди и слоту: тип в нём не обязан совпадать с кодом,
 * у уведомлений старший бит очереди взведён. */
static void note_packet(const u8 *buf) {
    u32 size = (u32)buf[0] | ((u32)buf[1] << 8) | ((u32)buf[2] << 16) | ((u32)buf[3] << 24);
    size &= 0x3fffu;
    u8 cmd = buf[4];
    rx_notes++;
    rx_last_cmd = cmd;
    if (!(buf[7] & 0x80u) && (buf[7] & 0x1fu) == Q_CMD && buf[6] == want_slot) {
        reply_cmd = cmd;
        reply_seen = 1;
    }
    if (cmd == 28 && !(buf[7] & 0x80u) && (buf[7] & 0x1fu) == Q_TX) data_acked++;
    if (cmd == 1 && !alive) {
        alive = 1;
        klogf("IWL: ALIVE\n");
    }
    if (size < 4 || size > 4096) return;
    const u8 *payload = buf + 8;
    u32 plen = size - 4;
    if (cmd == 1 && plen >= 24) memcpy(&err_ptr, payload + 20, 4);
    if (cmd == 2 && plen >= 12) {
        err_type = (u32)payload[0] | ((u32)payload[1] << 8) | ((u32)payload[2] << 16) | ((u32)payload[3] << 24);
        err_cmd = payload[4];
        err_info = (u32)payload[8] | ((u32)payload[9] << 8) | ((u32)payload[10] << 16) | ((u32)payload[11] << 24);
        reply_err = 1;
        klogf("IWL: error type=%x cmd=%u seq=%02x%02x info=%x\n",
              err_type, err_cmd, payload[7], payload[6], err_info);
    } else if (cmd == 102) save_calib(payload, plen);
    else if (cmd == 103) want_calib = 0;
    else if (cmd == 130) klogf("IWL: scan start\n");
    else if (cmd == 132) scan_done = 1;
    else if (cmd == 192 && plen >= 20 + 14) {
        last_rssi = rssi_dbm(payload + 20);
        have_phy = 1;
    } else if (cmd == 193 && plen >= 8) {
        u32 n = (u32)payload[0] | ((u32)payload[1] << 8);
        if (n >= 24 && 4u + n + 4u <= plen) take_frame(payload + 4, n, payload + 4 + n);
    }
}

/* Счётчик закрытых буферов идёт по кольцу из RX_N, а не до 4096.
 * Указатель записи отстаёт от чтения на один буфер и кратен 8. */
static u32 rx_closed_idx(void) { return (*rx_closed & 0xfffu) & (RX_N - 1u); }

static volatile int rxlock;

static void poll_rx(void) {
    if (!rx_closed || __sync_lock_test_and_set(&rxlock, 1)) return;
    u32 closed = rx_closed_idx();
    int n = 0;
    for (; rx_read != closed && n < RX_N; n++) {
        if (rx_data[rx_read]) {
            u8 type = rx_data[rx_read][4];
            if (type != 0xc0 && type != 0xc1) {
                u32 h = rx_hist_n++ & 3u;
                memcpy(rx_hist[h], rx_data[rx_read], 12);
                rx_hist_slot[h] = (u8)rx_read;
            }
            note_packet(rx_data[rx_read]);
        }
        rx_read = (rx_read + 1u) & (RX_N - 1u);
    }
    u32 w = (rx_read ? rx_read - 1u : RX_N - 1u) & ~7u;
    if (n && w != rx_wptr) {
        rx_wptr = w;
        wr(FH_RX_WPTR, rx_wptr);
    }
    __sync_lock_release(&rxlock);
}

static void poll_once(void) {
    if (hw_blocked()) {
        if (!rfkill_noted) { klogf("IWL: radio is off\n"); rfkill_noted = 1; wlan_hw_block(1); }
        return;
    }
    if (rfkill_noted) { rfkill_noted = 0; wlan_hw_block(0); }
    u32 ir = rd(CSR_INT);
    seen_ir |= ir;
    if (ir) wr(CSR_INT, ir);
    if (rx_closed && rx_closed_idx() != rx_read) poll_rx();
}

static void iwl_thread(void *arg) {
    (void)arg;
    for (;;) {
        if (running && mmio && !rx_hold) poll_once();
        sleep_ms(10);
    }
}

static void put16(u8 *p, u16 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static void put32(u8 *p, u32 v) {
    p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24);
}

static void mem_write(u32 addr, u32 val) {
    wr(HBUS_MEM_WADDR, addr);
    wr(HBUS_MEM_WDAT, val);
}

static int alloc_rings(void) {
    if (cmd_ring) return 1;
    cmd_ring = dma_contig(8);
    data_ring = dma_contig(8);
    sched = dma_contig(4);
    if (!cmd_ring || !data_ring || !sched) {
        klogf("IWL: DMA pages are not contiguous\n");
        return 0;
    }
    for (int i = 0; i < 16; i++) cmd_body[i] = dma_page();
    for (int i = 0; i < 16; i++) data_body[i] = dma_page();
    return cmd_body[15] && data_body[15];
}

/* Сервисный канал грузит прошивку и без этого. Очереди хозяина молчат,
 * пока у каналов 0–7 нет бита DMA и пока в прошивке стоит «команды запрещены». */
static void program_cbbc(void) {
    for (int q = 0; q < Q_N; q++)
        wr(FH_CBBC + q * 4, (u32)(uptr)data_ring >> 8);
    wr(FH_CBBC + Q_CMD * 4, (u32)(uptr)cmd_ring >> 8);
    for (int ch = 0; ch < 8; ch++)
        wr(FH_TX_CFG(ch), (1u << 31) | (1u << 3));
    klogf("IWL: gp1 %x\n", rd(CSR_UCODE_GP1));
    wr(CSR_UCODE_GP1_CLR, UCODE_GP1_RFKILL | UCODE_GP1_CMD_BLOCKED);
}

static void sched_byte(int qid, u32 slot, u8 sta, u16 len) {
    u16 v = (u16)(((u16)sta << 12) | (u16)(len + 8));
    u16 *table = (u16 *)sched;
    table[(u32)qid * 320u + slot] = v;
    if (slot < 64) table[(u32)qid * 320u + slot + 256u] = v;
}

/* Доступ к регистрам планировщика нужен только на время настройки. Дальше его надо отпустить, иначе прошивка не забирает команды. */
static int sched_setup(void) {
    wr(CSR_GP, rd(CSR_GP) | CSR_GP_INIT_DONE | CSR_GP_ACCESS);
    u32 base = prph_read(SCD_BASE);
    klogf("IWL: scheduler base %x\n", base);
    if (!base) return 0;
    for (u32 i = 0; i < SCD_CTX_DWORDS; i++) mem_write(base + SCD_CTX + i * 4u, 0);
    prph_write(SCD_BASE + 0x008, (u32)(uptr)sched >> 10);
    wr(FH_TX_CHICKEN, rd(FH_TX_CHICKEN) | 2u);
    prph_write(SCD_BASE + 0x0e8, 0xfffefu);
    prph_write(SCD_BASE + 0x248, 0);
    static const u8 fifo[] = { 3, 2, 1, 0, 7, 5, 6 };
    for (int q = 0; q < Q_N; q++) {
        prph_write(SCD_BASE + 0x068 + (u32)q * 4u, 0);
        wr(HBUS_WRPTR, (u32)q << 8);
        mem_write(base + SCD_QCTX((u32)q), 0);
        mem_write(base + SCD_QCTX((u32)q) + 4u, (64u << 16) | 64u);
        if (q < 7) prph_write(SCD_BASE + 0x10c + (u32)q * 4u, 0x00ff0018u | fifo[q]);
    }
    prph_write(SCD_BASE + 0x108, 0xfffffu);
    prph_write(SCD_BASE + 0x010, 0xffu);
    cmd_w = 0;
    data_w = 0;
    data_acked = 0;
    wr(CSR_GP, (rd(CSR_GP) | CSR_GP_INIT_DONE) & ~CSR_GP_ACCESS);
    program_cbbc();
    return 1;
}

static int host_cmd(u8 code, const void *payload, u32 len) {
    if (!cmd_ring || !cmd_body[0] || len + 4 > 4000) return -1;
    while (__sync_lock_test_and_set(&qlock, 1)) sleep_ms(1);
    u8 *body = cmd_body[cmd_body_i++ & 15];
    u32 slot = cmd_w & 255u;
    memset(body, 0, 4 + len);
    body[0] = code;
    body[2] = (u8)slot;
    body[3] = Q_CMD;
    if (len && payload) memcpy(body + 4, payload, len);
    u8 *tfd = cmd_ring + slot * 128u;
    memset(tfd, 0, 128);
    tfd[3] = 1;
    u32 phys = (u32)(uptr)body;
    memcpy(tfd + 4, &phys, 4);
    u16 seglen = (u16)((4u + len) << 4);
    memcpy(tfd + 8, &seglen, 2);
    sched_byte(Q_CMD, slot, 0, (u16)(4u + len));
    want_slot = (u8)slot;
    cmd_w++;
    asm volatile("mfence" ::: "memory");
    wr(HBUS_WRPTR, (cmd_w & 0xffu) | ((u32)Q_CMD << 8));
    __sync_lock_release(&qlock);
    return 0;
}

/* Упавшая прошивка больше ничего не присылает. Причина остаётся только в её журнале в SRAM. */
static void dump_fw_error(void) {
    if (fw_dumped) return;
    fw_dumped = 1;
    u32 ir = seen_ir | rd(CSR_INT);
    if (err_ptr < 0x800000u || err_ptr >= 0x900000u) {
        klogf("IWL: firmware log ptr=%x int=%x\n", err_ptr, ir);
        return;
    }
    wr(CSR_GP, rd(CSR_GP) | CSR_GP_INIT_DONE | CSR_GP_ACCESS);
    poll_ms(CSR_GP, CSR_GP_CLOCK, CSR_GP_CLOCK, 25);
    u32 d[12];
    for (u32 i = 0; i < 12; i++) d[i] = sram_dword(err_ptr + i * 4u);
    wr(CSR_GP, (rd(CSR_GP) | CSR_GP_INIT_DONE) & ~CSR_GP_ACCESS);
    klogf("IWL: firmware error valid=%x id=%x pc=%x line=%x data=%x:%x blink=%x int=%x\n",
          d[0], d[1], d[2], d[9], d[7], d[8], d[3], ir);
}

/* Прошивка API 5 принимает следующую команду только после ответа на предыдущую. */
static int cmd_sync(u8 code, const void *payload, u32 len) {
    int held = rx_hold;
    rx_hold = 1;
    if (running && !held) sleep_ms(20);
    reply_seen = 0;
    reply_err = 0;
    reply_cmd = 0;
    seen_ir = 0;
    if (host_cmd(code, payload, len)) { if (!held) rx_hold = 0; return -1; }
    for (int i = 0; i < 200 && !reply_err && !reply_seen; i++) {
        poll_once();
        sleep_ms(10);
    }
    if (!reply_err && !reply_seen) poll_once();
    int rc = 0;
    if (reply_err) {
        klogf("IWL: cmd %u rejected\n", code);
        rc = -1;
    } else if (reply_seen && reply_cmd != code) {
        klogf("IWL: cmd %u answered as %u\n", code, reply_cmd);
    } else if (!reply_seen) {
        wr(CSR_GP, rd(CSR_GP) | CSR_GP_INIT_DONE | CSR_GP_ACCESS);
        poll_ms(CSR_GP, CSR_GP_CLOCK, CSR_GP_CLOCK, 25);
        u32 rdptr = prph_read(SCD_BASE + 0x068 + (u32)Q_CMD * 4u);
        wr(CSR_GP, (rd(CSR_GP) | CSR_GP_INIT_DONE) & ~CSR_GP_ACCESS);
        klogf("IWL: cmd %u slot %u no reply rdptr=%x gp1=%x notes=%u\n",
              code, want_slot, rdptr, rd(CSR_UCODE_GP1), rx_hist_n);
        for (u32 k = rx_hist_n > 4 ? rx_hist_n - 4 : 0; k < rx_hist_n; k++) {
            const u8 *b = rx_hist[k & 3u];
            klogf("IWL: rb%u %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x\n", rx_hist_slot[k & 3u],
                  b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11]);
        }
        dump_fw_error();
        rc = -1;
    }
    if (!held) rx_hold = 0;
    return rc;
}

/* У 1000 только антенны A и B. В RXON выбор цепочек остаётся прошивке, скан задаёт их явно. */
static u8 rx_mask(void) { return (rx_ant & 3u) ? (u8)(rx_ant & 3u) : 3u; }

static u16 rx_chain_value(void) {
    u32 m = rx_mask();
    u32 n = (m & 1u) + ((m >> 1) & 1u);
    return (u16)((m << 1) | (n << 10) | (n << 12));
}

static u16 scan_chain_value(void) {
    u32 m = rx_mask();
    return (u16)(1u | (m << 1) | (m << 4) | (m << 7));
}

static u8 tx_one(void) {
    if (tx_ant & 1) return 1;
    if (tx_ant & 2) return 2;
    return 1;
}

static u32 cck_rate(void) { return 10u | (1u << 9) | ((u32)tx_one() << 14); }

/* Три состояния RXON, как во FreeBSD: простой канал, подготовка к auth с BSSID
 * и связь после assoc с AID и фильтром BSS. wlap всегда свой MAC. */
enum { RXON_IDLE, RXON_JOIN, RXON_RUN };

static int send_rxon(int channel, const u8 *bssid, int mode) {
    struct rxon c;
    memset(&c, 0, sizeof(c));
    memcpy(c.node, mac, 6);
    memcpy(c.wlap, mac, 6);
    if (bssid) memcpy(c.bssid, bssid, 6);
    c.dev_type = 3;
    c.rx_chain = rx_chain_value();
    c.ht1 = c.ht2 = c.ht3 = 0xff;
    c.flags = (1u << 0) | (1u << 2) | (1u << 15) | (1u << 30);
    c.filter = (1u << 2) | (1u << 3);
    if (mode == RXON_IDLE) {
        c.cck = 0x0f;
        c.ofdm = 0xff;
        c.filter |= 1u << 6;
    } else if (mode == RXON_JOIN) {
        c.cck = 0x03;
        c.ofdm = 0x15;
        c.filter |= 1u << 6;
    } else {
        c.cck = 0x0f;
        c.ofdm = 0x15;
        c.assoc = join_aid;
        c.filter |= 1u << 5;
    }
    c.channel = (u16)channel;
    cur_ch = channel;
    return cmd_sync(0x10, &c, sizeof(c));
}

/* Маска двух потоков — ANT_AB даже при одной передающей антенне: ноль роняет прошивку. */
static int linkq(u8 id, u8 ampdu_max) {
    u8 q[88];
    memset(q, 0, sizeof(q));
    q[0] = id;
    q[6] = tx_one();
    q[7] = 3;
    put16(q + 12, 4000);
    q[14] = 3;
    q[15] = ampdu_max;
    u32 rate = cck_rate();
    for (int i = 0; i < 16; i++) put32(q + 20 + i * 4, rate);
    return cmd_sync(78, q, sizeof(q));
}

static int add_sta(const u8 *addr, u8 id) {
    u8 node[92];
    memset(node, 0, sizeof(node));
    memcpy(node + 4, addr, 6);
    node[12] = id;
    return cmd_sync(24, node, sizeof(node));
}

static int set_txpower(void) {
    u8 pwr[4] = { 32, 1u << 6, 0x7f, 0 };
    return cmd_sync(149, pwr, sizeof(pwr));
}

/* RXON стирает таблицу станций. Широковещательную FreeBSD возвращает только без фильтра BSS. */
static int rxon_commit(int channel, const u8 *bssid, int mode) {
    static const u8 bcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    if (send_rxon(channel, bssid, mode)) return -1;
    if (mode != RXON_RUN && (add_sta(bcast, STA_BCAST) || linkq(STA_BCAST, 64))) return -1;
    return set_txpower();
}

/* Время до следующего маяка в микросекундах: TSF по модулю интервала без 64-битного деления. */
static int send_timing(void) {
    u8 t[20];
    memset(t, 0, sizeof(t));
    u32 bint = join_bint ? join_bint : 100;
    u32 period = bint * 1024u;
    u32 lo, hi;
    memcpy(&lo, join_tsf, 4);
    memcpy(&hi, join_tsf + 4, 4);
    u32 r = hi % period;
    for (int i = 31; i >= 0; i--) r = ((r << 1) | ((lo >> i) & 1u)) % period;
    memcpy(t, join_tsf, 8);
    put16(t + 8, (u16)bint);
    put32(t + 12, period - r);
    put16(t + 16, 10);
    return cmd_sync(20, t, sizeof(t));
}

static int ensure_join(const u8 *bssid) {
    if (join_set && !memcmp(join_bssid, bssid, 6)) return 0;
    ap_set = 0;
    join_set = 0;
    join_aid = 0;
    join_bint = 0;
    memset(join_tsf, 0, sizeof(join_tsf));
    memcpy(join_bssid, bssid, 6);
    if (rxon_commit(cur_ch, bssid, RXON_JOIN)) return -1;
    join_set = 1;
    return 0;
}

static int ensure_ap(const u8 *bssid) {
    if (ap_set && !memcmp(ap_mac, bssid, 6)) return 0;
    if (ensure_join(bssid)) return -1;
    if (send_timing() || rxon_commit(cur_ch, bssid, RXON_RUN) ||
        add_sta(bssid, STA_AP) || linkq(STA_AP, 32)) return -1;
    memcpy(ap_mac, bssid, 6);
    ap_set = 1;
    return 0;
}

/* Заголовок 802.11 прошивка берёт из команды и требует, чтобы данные за ним
 * начинались с границы 4 байт. Без подтверждений 28 тело слота нельзя переиспользовать. */
static int send_data(const u8 *mpdu, u32 len, u8 sta) {
    if (!data_ring || !sched || !data_body[0] || len > 2000) return -1;
    u32 hdr = (mpdu[0] & 0x8c) == 0x88 ? 26u : 24u;
    if ((mpdu[1] & 3u) == 3u) hdr += 6;
    if (len < hdr) return -1;
    u32 pad = (4u - (hdr & 3u)) & 3u;
    for (int i = 0; i < 50 && data_w - data_acked >= 14u; i++) sleep_ms(2);
    while (__sync_lock_test_and_set(&qlock, 1)) sleep_ms(1);
    u8 *body = data_body[data_body_i++ & 15];
    u32 slot = data_w & 255u;
    memset(body, 0, 60 + pad);
    body[0] = 28;
    body[2] = (u8)slot;
    body[3] = Q_TX;
    u8 *tx = body + 4;
    put16(tx, (u16)len);
    u32 flags = 1u << 13;
    if ((mpdu[4] & 1) == 0) flags |= 1u << 3;
    if (pad) flags |= 1u << 20;
    put32(tx + 4, flags);
    put32(tx + 12, cck_rate());
    tx[16] = sta;
    put32(tx + 40, 0xffffffffu);
    put32(tx + 44, (u32)(uptr)(body + 12));
    tx[49] = 60;
    tx[50] = 15;
    if ((mpdu[0] & 0x0c) == 0) {
        u8 sub = (u8)(mpdu[0] & 0xf0);
        put16(tx + 52, (sub == 0x00 || sub == 0x20) ? 3 : 2);
    }
    memcpy(body + 60, mpdu, hdr);
    memcpy(body + 60 + hdr + pad, mpdu + hdr, len - hdr);
    u8 *tfd = data_ring + slot * 128u;
    memset(tfd, 0, 128);
    tfd[3] = 1;
    u32 phys = (u32)(uptr)body;
    memcpy(tfd + 4, &phys, 4);
    u16 seglen = (u16)((60u + len + pad) << 4);
    memcpy(tfd + 8, &seglen, 2);
    sched_byte(Q_TX, slot, sta, (u16)len);
    data_w++;
    asm volatile("mfence" ::: "memory");
    wr(HBUS_WRPTR, (data_w & 0xffu) | ((u32)Q_TX << 8));
    __sync_lock_release(&qlock);
    return 0;
}

static int drv_up(void *p) { (void)p; return alive ? 0 : -1; }
static void drv_down(void *p) { (void)p; running = 0; }
static int drv_channel(void *p, int ch) {
    (void)p;
    ap_set = 0;
    join_set = 0;
    return rxon_commit(ch, NULL, RXON_IDLE);
}
static int drv_tx(void *p, const void *mpdu, u32 len) {
    (void)p;
    const u8 *f = mpdu;
    if (!f || len < 24) return -1;
    if ((f[0] & 0x0c) == 0) {
        u8 sub = (u8)(f[0] & 0xf0);
        if ((sub == 0xb0 || sub == 0x00 || sub == 0x20) && ensure_join(f + 4)) return -1;
    }
    int data = (f[0] & 0x0c) == 0x08;
    u8 sta = (data && (f[4] & 1) == 0) ? STA_AP : STA_BCAST;
    if (sta == STA_AP && ensure_ap(f + 4)) return -1;
    return send_data(f, len, sta);
}
static int drv_radio(void *p) { (void)p; return hw_blocked(); }

static int drv_scan(void *p) {
    (void)p;
    static u8 buf[1400];
    u8 probe[128];
    int plen = wlan_build_probe(probe, sizeof(probe), mac, NULL, 0);
    int nchan = max_ch >= 1 && max_ch <= 13 ? max_ch : 11;
    u32 need = 28u + 56u + 20u * 34u + (u32)plen + (u32)nchan * 12u;
    if (plen < 24 || need > sizeof(buf)) return -1;
    memset(buf, 0, need);
    put16(buf, (u16)need);
    buf[3] = (u8)nchan;
    put16(buf + 4, 10);
    put16(buf + 6, 1);
    put16(buf + 8, 1);
    put16(buf + 10, scan_chain_value());
    put32(buf + 12, 250u * 1024u);
    put32(buf + 16, (4u << 22) | (100u * 1024u));
    put32(buf + 20, 1u | (1u << 2));
    put32(buf + 24, (1u << 2) | (1u << 6));
    u8 *tx = buf + 28;
    put16(tx, (u16)plen);
    put32(tx + 4, 1u << 13);
    put32(tx + 12, cck_rate());
    tx[16] = STA_BCAST;
    put32(tx + 40, 0xffffffffu);
    memcpy(buf + 28 + 56 + 20 * 34, probe, (u32)plen);
    u8 *ch = buf + 28 + 56 + 20 * 34 + plen;
    for (int i = 0; i < nchan; i++) {
        put32(ch, 3);
        put16(ch + 4, (u16)(i + 1));
        ch[6] = 0x28;
        ch[7] = 0x6e;
        put16(ch + 8, 40);
        put16(ch + 10, 110);
        ch += 12;
    }
    rx_hold = 1;
    sleep_ms(20);
    scan_done = 0;
    rx_frames = 0;
    rx_notes = 0;
    if (host_cmd(128, buf, need)) { rx_hold = 0; return -1; }
    for (int i = 0; i < 300 && !scan_done; i++) { poll_once(); sleep_ms(10); }
    if (scan_done) klogf("IWL: scan complete rx=%d\n", rx_frames);
    else klogf("IWL: scan timed out rx=%d notes=%d last=%u\n", rx_frames, rx_notes, rx_last_cmd);
    rx_hold = 0;
    return 0;
}

static void read_xtal(void) {
    u16 ptr = 0, lo = 0, hi = 0, temp = 0;
    if (!nvm_word(nvm_base + 0x067u * 2u, &ptr)) return;
    u32 at = nvm_base + ((u32)ptr + 0x128u) * 2u;
    if (!nvm_word(at, &lo) || !nvm_word(at + 2u, &hi)) return;
    xtal = (u32)lo | ((u32)hi << 16);
    if (nvm_word(nvm_base + ((u32)ptr + 0x12au) * 2u, &temp)) temp_off = temp;
}

/* Слово 0x048: передача в битах 11:8, приём в 15:12. Ноль — оставить A / A+B. */
static void read_chains(void) {
    u16 rf = 0;
    if (!nvm_word(nvm_base + 0x048u * 2u, &rf)) return;
    rfcfg = rf;
    u8 tx = (u8)((rf >> 8) & 3u);
    u8 rx = (u8)((rf >> 12) & 3u);
    if (tx) tx_ant = tx;
    if (rx) rx_ant = rx;
}

/* Без типа радиочасти INIT подтверждает запрос калибровки и ничего не меряет,
 * а runtime не отвечает на RXON. Сброс стирает эти биты, поэтому — перед каждой прошивкой. */
#define HW_IF_MAC_MASK 0x0000000fu
#define HW_IF_PHY_MASK 0x0000fc00u
#define HW_IF_MAC_SI 0x00000100u
#define HW_IF_RADIO_SI 0x00000200u
#define APMG_PS_EARLY_PWROFF_DIS 0x00400000u
static void nic_config(void) {
    u32 hw = rd(CSR_HW_IF) & ~HW_IF_MAC_MASK;
    hw |= rd(CSR_HW_REV) & 0xfu;
    hw &= ~HW_IF_PHY_MASK;
    hw |= ((u32)(rfcfg & 3u) << 10) | ((u32)((rfcfg >> 4) & 3u) << 12) | ((u32)((rfcfg >> 2) & 3u) << 14);
    wr(CSR_HW_IF, hw | HW_IF_MAC_SI | HW_IF_RADIO_SI);
    wr(CSR_GP, rd(CSR_GP) | CSR_GP_INIT_DONE | CSR_GP_ACCESS);
    u32 ps = prph_read(APMG_PS);
    prph_write(APMG_PS, (ps & ~APMG_PS_SRC) | APMG_PS_EARLY_PWROFF_DIS);
}

static int calib_count(void) {
    int n = 0;
    for (int i = 0; i < 4; i++) if (calib_n[i]) n++;
    return n;
}

static int send_wimax(void) {
    u8 wimax[68];
    memset(wimax, 0, sizeof(wimax));
    return cmd_sync(90, wimax, sizeof(wimax));
}

static int send_crystal(void) {
    u8 cr[8];
    memset(cr, 0, sizeof(cr));
    cr[0] = 15;
    cr[2] = 1;
    cr[3] = 1;
    cr[4] = (u8)xtal;
    cr[5] = (u8)(xtal >> 16);
    return cmd_sync(176, cr, sizeof(cr));
}

/* Бит 0 в Link Control PCIe — вход в L0s. Без него прошивке разрешают своё управление шиной. */
static int pcie_l0s(void) {
    unsigned cap = pci_read(dev.bus, dev.dev, dev.func, 0x34) & 0xfc;
    for (unsigned n = 0; cap >= 0x40 && cap <= 0xf8 && n < 48; n++) {
        u32 header = pci_read(dev.bus, dev.dev, dev.func, cap);
        if ((header & 255) == 0x10) return pci_read(dev.bus, dev.dev, dev.func, cap + 0x10) & 1;
        cap = (header >> 8) & 0xfc;
    }
    return 0;
}

/* Порядок FreeBSD для 1000: post_alive (WiMAX, кристалл, калибровки), затем config
 * (маска TX, RXON со станцией и мощностью, критическая температура, питание CAM).
 * Поправку температуры (код 18) прошивка 1000 не ждёт. */
static int runtime_tune(void) {
    if (send_wimax() || send_crystal()) return -1;
    for (int i = 0; i < 4; i++)
        if (calib_n[i] && cmd_sync(176, calib_blob[i], calib_n[i])) return -1;
    u32 txmask = tx_ant;
    if (cmd_sync(152, &txmask, sizeof(txmask)) || rxon_commit(1, NULL, RXON_IDLE)) return -1;
    wr(CSR_UCODE_GP1_CLR, 1u << 3);
    u8 crit[12];
    memset(crit, 0, sizeof(crit));
    put32(crit + 8, 110);
    if (cmd_sync(164, crit, sizeof(crit))) return -1;
    u8 pm[36];
    memset(pm, 0, sizeof(pm));
    if (!pcie_l0s()) put16(pm, 1u << 3);
    /* CAM — режим прошивки по умолчанию; без ответа на 119 она продолжает принимать. */
    cmd_sync(119, pm, sizeof(pm));
    u8 edca[36];
    memset(edca, 0, sizeof(edca));
    put32(edca, 1);
    static const u16 cwmin[] = { 15, 15, 7, 3 };
    static const u16 cwmax[] = { 1023, 1023, 15, 7 };
    static const u8 aifsn[] = { 3, 7, 2, 2 };
    static const u16 txop[] = { 0, 0, 3008, 1504 };
    for (int i = 0; i < 4; i++) {
        u8 *a = edca + 4 + i * 8;
        put16(a, cwmin[i]);
        put16(a + 2, cwmax[i]);
        a[4] = aifsn[i];
        put16(a + 6, txop[i]);
    }
    cmd_sync(19, edca, sizeof(edca));
    return 0;
}

static int wait_alive(const char *which) {
    seen_ir = 0;
    for (int i = 0; i < 300 && !alive; i++) { poll_once(); sleep_ms(10); }
    if (alive) return 1;
    klogf("IWL: %s firmware did not answer, int=%x closed=%x\n",
          which, seen_ir, rx_closed ? *rx_closed : 0);
    return 0;
}

static int bringup(void) {
    if (!nic_reset()) return -1;
    if (hw_blocked()) {
        klogf("IWL: radio is off\n");
        rfkill_noted = 1;
        return -1;
    }
    nvm_open();
    if (!read_mac()) return -1;
    read_channels();
    read_xtal();
    read_chains();
    klogf("IWL: MAC %02x:%02x:%02x:%02x:%02x:%02x base %x rev %x crystal %x temp %u rf %x\n",
          mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], nvm_base, rd(CSR_HW_REV),
          xtal, temp_off, rfcfg);
    if (!alloc_rings()) return -1;
    nic_config();
    rx_setup();
    if (!load_image(&fw.init, 0, &fw.init_data, 0x800000)) return -1;
    u32 expect = 0;
    memcpy(&expect, fw.init.p, 4);
    u32 got = sram_dword(0);
    if (got != expect) {
        klogf("IWL: SRAM mismatch got=%x expect=%x\n", got, expect);
        return -1;
    }
    program_cbbc();
    wr(CSR_GP, (rd(CSR_GP) | CSR_GP_INIT_DONE) & ~CSR_GP_ACCESS);
    wr(CSR_RESET, 0);
    if (!wait_alive("INIT")) return -1;
    if (!sched_setup()) return -1;
    /* Без кристалла INIT подтверждает команду 101 и не присылает измерения. */
    send_wimax();
    send_crystal();
    want_calib = 1;
    u8 cfg[92];
    memset(cfg, 0, sizeof(cfg));
    put32(cfg, 0xffffffffu);
    put32(cfg + 4, 0xffffffffu);
    put32(cfg + 8, 0xffffffffu);
    put32(cfg + 40, 0xffffffffu);
    rx_notes = 0;
    if (host_cmd(101, cfg, sizeof(cfg))) return -1;
    for (int i = 0; i < 200 && want_calib; i++) { poll_once(); sleep_ms(10); }
    want_calib = 0;
    klogf("IWL: calibration results %d notes=%d last=%u\n", calib_count(), rx_notes, rx_last_cmd);
    alive = 0;
    if (!nic_reset()) return -1;
    nic_config();
    rx_setup();
    if (!load_image(&fw.inst, 0, &fw.data, 0x800000)) return -1;
    program_cbbc();
    wr(CSR_GP, (rd(CSR_GP) | CSR_GP_INIT_DONE) & ~CSR_GP_ACCESS);
    wr(CSR_RESET, 0);
    if (!wait_alive("runtime")) return -1;
    if (!sched_setup()) return -1;
    if (runtime_tune()) return -1;
    running = 1;
    return 0;
}

static int iwl_init(void) {
    mod = module_current();
    int found = 0;
    static const u16 ids[] = { 0x0083, 0x0084 };
    for (unsigned i = 0; i < 2; i++) if (pci_find_device(0x8086, ids[i], &dev)) { found = 1; break; }
    if (!found) return -19;
    if (!power_d0() || !map_bar()) return -19;
    u32 n = 0;
    if (firmware_read("/lib/firmware/iwlwifi-1000-5.ucode", (void **)&fw_buf, &n)) {
        klogf("IWL: firmware file is missing\n");
        return -2;
    }
    if (iwl_fw_parse(fw_buf, n, &fw)) { klogf("IWL: firmware file is not API 5\n"); return -2; }
    if (bringup()) return -5;
    static const struct wlan_ops ops = { drv_up, drv_down, drv_channel, drv_tx, drv_radio, drv_scan };
    if (wlan_attach("wlan0", mac, &ops, NULL, max_ch)) return -16;
    task_create("wlan", iwl_thread, NULL);
    if (hw_blocked()) wlan_hw_block(1);
    return 0;
}

static void iwl_fini(void) {
    running = 0;
    wlan_detach();
    for (int i = 0; i < npages; i++) pmm_free(pages[i]);
    kfree(fw_buf);
}

struct myos_module myos_module = { MOD_MAGIC, "iwl1000", iwl_init, iwl_fini };
