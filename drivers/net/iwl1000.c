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
#define Q_CMD 4
#define Q_TX 0
#define RX_N 32

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
static u32 pages[48];
static int npages;
static u8 *fw_buf;
static struct iwl_fw fw;
static u32 *rx_bd;
static volatile u32 *rx_closed;
static u8 *rx_data[RX_N];
static u32 rx_read;
static u8 *tx_ring;
static u32 tx_w;
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
    if (wake_clock()) return 1;
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
 * Счётчик лежит в 0x1bc8 и должен быть кратен 8. Ноль — «принимать некуда»,
 * поэтому уведомление ALIVE не попадает в очередь. */
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
    wr(FH_RX_CFG, 0x80000000u | (5u << 20) | 0x1000u | (0x10u << 4) | 0x8000u | 4u);
    wr(FH_RX_WPTR, 24);
}

static void note_packet(const u8 *buf) {
    u8 cmd = buf[4];
    if (cmd == 1 && !alive) {
        alive = 1;
        klogf("IWL: ALIVE\n");
    }
    if (cmd == 0xc3 && rx_data[0]) {
        u16 n = (u16)(buf[8] | (buf[9] << 8));
        if (n >= 24 && n < 2000) wlan_rx(buf + 12, n, -60);
    }
}

static void poll_rx(void) {
    if (!rx_closed) return;
    u32 closed = *rx_closed & 0xfff;
    for (int n = 0; rx_read != closed && n < RX_N; n++) {
        u32 i = rx_read % RX_N;
        if (rx_data[i]) note_packet(rx_data[i]);
        rx_read++;
    }
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
    if (rx_closed && (*rx_closed & 0xfff) != rx_read) poll_rx();
}

static void iwl_thread(void *arg) {
    (void)arg;
    for (;;) {
        if (running && mmio) poll_once();
        sleep_ms(10);
    }
}

static u8 *tx_body[8];
static u32 tx_body_i;

static int kick_tx(const void *bytes, u32 len, int queue) {
    if (!tx_ring || len > 2000) return -1;
    if (!tx_body[0]) for (int i = 0; i < 8; i++) tx_body[i] = dma_page();
    u8 *body = tx_body[tx_body_i++ & 7];
    if (!body) return -1;
    u32 slot = tx_w & 31;
    u8 *tfd = tx_ring + slot * 128;
    memset(tfd, 0, 128);
    memcpy(body, bytes, len);
    tfd[3] = 1;
    u32 phys = (u32)(uptr)body;
    memcpy(tfd + 4, &phys, 4);
    u16 hi = (u16)(len << 4);
    memcpy(tfd + 8, &hi, 2);
    tx_w++;
    wr(HBUS_WRPTR, (tx_w & 0xff) | ((u32)queue << 8));
    return 0;
}

static int send_rxon(int channel, int associated) {
    struct rxon c;
    u8 pkt[4 + sizeof(c)];
    memset(&c, 0, sizeof(c));
    memset(pkt, 0, sizeof(pkt));
    memcpy(c.node, mac, 6);
    c.dev_type = 3;
    c.rx_chain = 0x182;
    c.cck = 0x0f;
    c.ofdm = 0xff;
    c.flags = 1 | 2 | (1 << 8) | (1 << 9);
    c.filter = (1 << 1) | (1 << 2) | (1 << 3) | (1 << 4);
    if (associated) c.filter |= 1 << 5;
    c.channel = (u16)channel;
    pkt[0] = 0x10;
    memcpy(pkt + 4, &c, sizeof(c));
    return kick_tx(pkt, sizeof(pkt), Q_CMD);
}

static int drv_up(void *p) { (void)p; return alive ? 0 : -1; }
static void drv_down(void *p) { (void)p; running = 0; }
static int drv_channel(void *p, int ch) { (void)p; return send_rxon(ch, 0) ? -1 : 0; }
static int drv_tx(void *p, const void *mpdu, u32 len) {
    (void)p;
    u8 pkt[1600];
    if (len + 4 > sizeof(pkt)) return -1;
    memset(pkt, 0, 4 + 60);
    pkt[0] = 0x1c;
    u16 l = (u16)len;
    memcpy(pkt + 4, &l, 2);
    u32 flags = (1u << 13) | (1u << 3);
    memcpy(pkt + 8, &flags, 4);
    u32 rate = 10 | 0x200;
    memcpy(pkt + 16, &rate, 4);
    pkt[50] = 7;
    memcpy(pkt + 4 + 60, mpdu, len);
    return kick_tx(pkt, 4 + 60 + len, Q_TX);
}
static int drv_radio(void *p) { (void)p; return hw_blocked(); }

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
    klogf("IWL: MAC %02x:%02x:%02x:%02x:%02x:%02x base %x rev %x\n",
          mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], nvm_base, rd(CSR_HW_REV));
    rx_setup();
    if (!load_image(&fw.init, 0, &fw.init_data, 0x800000)) return -1;
    u32 expect = 0;
    memcpy(&expect, fw.init.p, 4);
    u32 got = sram_dword(0);
    if (got != expect) {
        klogf("IWL: SRAM mismatch got=%x expect=%x\n", got, expect);
        return -1;
    }
    wr(CSR_GP, (rd(CSR_GP) | CSR_GP_INIT_DONE) & ~CSR_GP_ACCESS);
    wr(CSR_RESET, 0);
    seen_ir = 0;
    for (int i = 0; i < 300 && !alive; i++) { poll_once(); sleep_ms(10); }
    if (!alive) {
        klogf("IWL: INIT firmware did not answer, int=%x closed=%x\n",
              seen_ir, rx_closed ? *rx_closed : 0);
        return -1;
    }
    alive = 0;
    if (!nic_reset()) return -1;
    rx_setup();
    if (!load_image(&fw.inst, 0, &fw.data, 0x800000)) return -1;
    if (!tx_ring) tx_ring = dma_page();
    u32 base = (u32)(uptr)tx_ring >> 8;
    wr(FH_CBBC + Q_CMD * 4, base);
    wr(FH_CBBC + Q_TX * 4, base);
    wr(CSR_RESET, 0);
    for (int i = 0; i < 300 && !alive; i++) { poll_once(); sleep_ms(10); }
    if (!alive) { klogf("IWL: runtime firmware did not answer\n"); return -1; }
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
    static const struct wlan_ops ops = { drv_up, drv_down, drv_channel, drv_tx, drv_radio };
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
