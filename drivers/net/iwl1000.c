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

#define MMIO_VIRT 0xE5000000u
#define MMIO_BYTES 0x4000u
#define CSR_INT 0x008
#define CSR_INT_MASK 0x00c
#define CSR_RESET 0x020
#define CSR_GP 0x024
#define CSR_HW_REV 0x028
#define CSR_EEPROM 0x02c
#define CSR_INT_FH_RX 0x80000000u
#define CSR_RESET_SW 0x80u
#define CSR_RESET_MASTER_DIS 0x100u
#define CSR_RESET_STOP 0x200u
#define CSR_GP_CLOCK 1u
#define CSR_GP_INIT_DONE 4u
#define CSR_GP_ACCESS 8u
#define CSR_GP_RFKILL 0x08000000u
#define FH_KW 0x197c
#define FH_CBBC 0x19d0
#define FH_SRVC_ADDR 0x19c8
#define FH_RX_WPTR 0x1bc0
#define FH_RX_BASE 0x1bc4
#define FH_RX_CFG 0x1c00
#define FH_TX_CFG(ch) (0x1d00u + 0x20u * (ch))
#define FH_TX_STS(ch) (0x1d00u + 0x20u * (ch) + 8u)
#define FH_TSSR 0x1eb0
#define HBUS_MEM_WADDR 0x410
#define HBUS_MEM_WDAT 0x418
#define HBUS_PRPH_WADDR 0x444
#define HBUS_PRPH_WDAT 0x44c
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

static int wait_bit(u32 reg, u32 mask, u32 want, u32 spins) {
    for (u32 i = 0; i < spins; i++) if ((rd(reg) & mask) == want) return 1;
    return 0;
}

static int grab_access(void) {
    wr(CSR_GP, rd(CSR_GP) | CSR_GP_ACCESS);
    return wait_bit(CSR_GP, CSR_GP_CLOCK, CSR_GP_CLOCK, 200000);
}

static void prph_write(u32 addr, u32 val) {
    wr(HBUS_PRPH_WADDR, (addr & 0x000fffffu) | (3u << 24));
    wr(HBUS_PRPH_WDAT, val);
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
    pci_write(dev.bus, dev.dev, dev.func, 4, cmd | 6);
    return 1;
}

static u16 eeprom_word(u32 addr) {
    wr(CSR_EEPROM, (addr << 2) | 2);
    for (int i = 0; i < 10000; i++) {
        u32 v = rd(CSR_EEPROM);
        if (v & 1) return (u16)(v >> 16);
    }
    return 0xffff;
}

static int read_mac(void) {
    for (int i = 0; i < 3; i++) {
        u16 w = eeprom_word(0x15 + (u32)i);
        mac[i * 2] = (u8)w;
        mac[i * 2 + 1] = (u8)(w >> 8);
    }
    unsigned any = 0, all = 255;
    for (int i = 0; i < 6; i++) { any |= mac[i]; all &= mac[i]; }
    return any && all != 255 && !(mac[0] & 1);
}

static void read_channels(void) {
    int allow = 0;
    for (int i = 0; i < 13; i++) {
        u16 f = eeprom_word(0x35 + (u32)i);
        if (f && f != 0xffff && (f & 1)) allow |= 1 << (i + 1);
    }
    int n = 0;
    for (int i = 1; i <= 11; i++) if (allow & (1 << i)) n++;
    max_ch = 11;
    if (n >= 11 && (allow & (1 << 12)) && (allow & (1 << 13))) max_ch = 13;
}

static int hw_blocked(void) { return (rd(CSR_GP) & CSR_GP_RFKILL) != 0; }

static int nic_reset(void) {
    wr(CSR_GP, rd(CSR_GP) | CSR_GP_INIT_DONE);
    wr(CSR_RESET, CSR_RESET_STOP);
    if (!wait_bit(CSR_RESET, CSR_RESET_MASTER_DIS, CSR_RESET_MASTER_DIS, 200000)) return 0;
    wr(CSR_RESET, CSR_RESET_SW);
    sleep_ms(10);
    wr(CSR_INT, 0xffffffffu);
    wr(CSR_INT_MASK, 0);
    return grab_access();
}

static u8 *chunk_page, *chunk_tfd;

static int load_chunk(u32 dest, const u8 *src, u32 n) {
    if (!chunk_page) { chunk_page = dma_page(); chunk_tfd = dma_page(); }
    if (!chunk_page || !chunk_tfd || n == 0 || n > 4092) return 0;
    memset(chunk_tfd, 0, 128);
    memcpy(chunk_page, src, n);
    chunk_tfd[3] = 1;
    u32 phys = (u32)(uptr)chunk_page;
    memcpy(chunk_tfd + 4, &phys, 4);
    u16 hi = (u16)(n << 4);
    memcpy(chunk_tfd + 8, &hi, 2);
    wr(FH_SRVC_ADDR, dest);
    wr(FH_CBBC + 9 * 4, (u32)(uptr)chunk_tfd >> 8);
    wr(FH_TX_CFG(9), 0x80000000u | 0x00100000u);
    for (int i = 0; i < 100000; i++) {
        if (rd(FH_TSSR) & (1u << 25)) return 1;
    }
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
    wr(FH_KW, (u32)(uptr)kw_page >> 4);
    wr(FH_RX_WPTR, (u32)(uptr)rx_closed >> 4);
    wr(FH_RX_BASE, (u32)(uptr)rx_bd >> 8);
    wr(FH_RX_CFG, 0x80000000u | (5u << 20) | 0x1000u | (0x10u << 4) | 0x8000u);
    rx_read = 0;
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
    if (!ir) return;
    wr(CSR_INT, ir);
    if (ir & CSR_INT_FH_RX) poll_rx();
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
    if (!read_mac()) { klogf("IWL: EEPROM has no MAC\n"); return -1; }
    read_channels();
    klogf("IWL: MAC %02x:%02x:%02x:%02x:%02x:%02x rev %x\n",
          mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], rd(CSR_HW_REV));
    rx_setup();
    if (!load_image(&fw.init, 0, &fw.init_data, 0x800000)) return -1;
    wr(CSR_RESET, 0);
    for (int i = 0; i < 300 && !alive; i++) { poll_once(); sleep_ms(10); }
    if (!alive) { klogf("IWL: INIT firmware did not answer\n"); return -1; }
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
    (void)prph_write;
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
