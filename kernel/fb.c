/* Графика: драйверы видеокарт VMware SVGA II и Bochs VBE.
 *
 * Обе карты дают линейную видеопамять (LFB): пиксель (x, y) лежит по адресу
 * LFB + y * pitch + x * 4 в формате 0x00RRGGBB. Разница — в остальном.
 *
 * Bochs VBE (QEMU -vga std) — два порта, 0x1CE (номер регистра) и 0x1CF
 * (значение). Записали ширину, высоту, глубину цвета — режим включён. Больше
 * карта ничего не умеет: заливку и копирование делает процессор.
 *
 * VMware SVGA II (QEMU -vga vmware, VirtualBox, VMware) — настоящий
 * GPU-интерфейс. У карты три «окна» (PCI BAR):
 *   BAR0 — порты ввода-вывода: индекс регистра и значение (по 32 бита);
 *   BAR1 — видеопамять;
 *   BAR2 — FIFO, кольцевая очередь команд в памяти: мы дописываем команды в
 *          конец и сдвигаем NEXT_CMD, карта выполняет их сама и сдвигает STOP.
 * Команды: UPDATE (показать изменённый прямоугольник — без неё карта может
 * не заметить, что мы что-то нарисовали), RECT_FILL, RECT_COPY (карта сама
 * заливает/двигает пиксели: прокрутка консоли и перетаскивание окон почти
 * ничего не стоят процессору), DEFINE_CURSOR (картинка аппаратного курсора;
 * дальше его положение — просто регистры, процессор курсор не рисует).
 *
 * Тонкость: команды выполняются не сразу. Прежде чем процессор сам что-то
 * пишет в видеопамять, нужно дождаться, пока карта доделает очередь (SYNC),
 * иначе запоздавшее копирование затрёт свежие пиксели. */
#include "fb.h"
#include "virtgpu.h"
#include "ipc.h"
#include "pci.h"
#include "paging.h"
#include "string.h"
#include "vga.h"
#include "bootinfo.h"

static inline void outl(u16 port, u32 v) { __asm__ volatile ("outl %0, %1" : : "a"(v), "Nd"(port)); }
static inline u32  inl(u16 port) { u32 v; __asm__ volatile ("inl %1, %0" : "=a"(v) : "Nd"(port)); return v; }

enum { DRV_NONE, DRV_BOCHS, DRV_SVGA, DRV_FIRMWARE, DRV_VIRTIO };
static u32 frame_depth;
static int drv;
static bool ready;
static struct fb_info info;
static volatile u32 *fb;            /* видимый кадр */
static u32 stride;                  /* пикселей в строке */
static u8 font[256 * FONT_H];
static bool gpu_ok;                 /* копирование на карте проверено в этом режиме */
static u32  probe_tries;            /* сколько ещё раз пробовать */
static void gpu_probe(void);

/* ================================================================ Bochs VBE */

#define VBE_INDEX   0x01CE
#define VBE_DATA    0x01CF
#define VBE_ID      0
#define VBE_XRES    1
#define VBE_YRES    2
#define VBE_BPP     3
#define VBE_ENABLE  4
#define VBE_VIRT_W  6
#define VBE_VIRT_H  7
#define VBE_VRAM64K 0x0A
#define VBE_ENABLED     0x01
#define VBE_LFB_ENABLED 0x40

static void vbe_write(u16 idx, u16 val) { outw(VBE_INDEX, idx); outw(VBE_DATA, val); }
static u16  vbe_read(u16 idx) { outw(VBE_INDEX, idx); return inw(VBE_DATA); }

static bool bochs_set_mode(u32 w, u32 h) {
    vbe_write(VBE_ENABLE, 0);
    vbe_write(VBE_XRES, w);
    vbe_write(VBE_YRES, h);
    vbe_write(VBE_BPP, FB_BPP);
    vbe_write(VBE_VIRT_W, w);
    vbe_write(VBE_VIRT_H, h);
    vbe_write(VBE_ENABLE, VBE_ENABLED | VBE_LFB_ENABLED);
    if (vbe_read(VBE_XRES) != w || vbe_read(VBE_YRES) != h || vbe_read(VBE_BPP) != FB_BPP) return false;
    info.pitch = vbe_read(VBE_VIRT_W) * 4;
    fb = (volatile u32 *)FB_VIRT;
    return true;
}

/* ================================================================ VMware SVGA II */

#define SVGA_VENDOR 0x15AD
#define SVGA_DEVICE 0x0405
#define SVGA_ID_2   0x90000002u

enum {
    R_ID = 0, R_ENABLE = 1, R_WIDTH = 2, R_HEIGHT = 3, R_MAX_WIDTH = 4, R_MAX_HEIGHT = 5,
    R_DEPTH = 6, R_BPP = 7, R_BYTES_PER_LINE = 12, R_FB_START = 13, R_FB_OFFSET = 14,
    R_VRAM_SIZE = 15, R_FB_SIZE = 16, R_CAPS = 17, R_MEM_START = 18, R_MEM_SIZE = 19,
    R_CONFIG_DONE = 20, R_SYNC = 21, R_BUSY = 22, R_GUEST_ID = 23,
    R_CURSOR_ID = 24, R_CURSOR_X = 25, R_CURSOR_Y = 26, R_CURSOR_ON = 27,
};
#define CAP_RECT_FILL 0x01
#define CAP_RECT_COPY 0x02
#define CAP_CURSOR    0x20
#define CAP_ALPHA_CURSOR 0x200
enum { F_MIN = 0, F_MAX = 1, F_NEXT_CMD = 2, F_STOP = 3, F_NUM_REGS = 293 };
enum { CMD_UPDATE = 1, CMD_RECT_FILL = 2, CMD_RECT_COPY = 3, CMD_DEFINE_CURSOR = 19, CMD_DEFINE_ALPHA_CURSOR = 22 };

static u16 svga_io;
static volatile u32 *fifo;
static u32 fifo_size;
static u32 svga_caps;
static bool gpu_busy;               /* в очереди есть команды, которые пишут в видеопамять */

static void svga_write(u32 idx, u32 v) { outl(svga_io, idx); outl(svga_io + 1, v); }
static u32  svga_read(u32 idx) { outl(svga_io, idx); return inl(svga_io + 1); }

/* дождаться, пока карта выполнит всю очередь */
static void svga_sync(void) {
    svga_write(R_SYNC, 1);
    for (u32 i = 0; i < 50000000u && svga_read(R_BUSY); i++) {}
    gpu_busy = false;
}

static u32 fifo_free(void) {
    u32 min = fifo[F_MIN], max = fifo[F_MAX], next = fifo[F_NEXT_CMD], stop = fifo[F_STOP];
    u32 used = next >= stop ? next - stop : (max - stop) + (next - min);
    return (max - min) - used - 4;
}

/* положить команду в очередь целиком (words — 32-битные слова) */
static void fifo_cmd(const u32 *words, u32 n) {
    u32 f = irq_save();
    if (fifo_free() < n * 4) svga_sync();
    u32 min = fifo[F_MIN], max = fifo[F_MAX], next = fifo[F_NEXT_CMD];
    for (u32 i = 0; i < n; i++) {
        fifo[next / 4] = words[i];
        next += 4;
        if (next >= max) next = min;
    }
    __asm__ volatile ("" : : : "memory");
    fifo[F_NEXT_CMD] = next;        /* только теперь карта видит команду */
    irq_restore(f);
}

static bool svga_set_mode(u32 w, u32 h) {
    u32 f = irq_save();
    svga_sync();
    svga_write(R_WIDTH, w);
    svga_write(R_HEIGHT, h);
    svga_write(R_BPP, FB_BPP);
    svga_write(R_ENABLE, 1);
    bool ok = svga_read(R_WIDTH) == w && svga_read(R_HEIGHT) == h && svga_read(R_BPP) == FB_BPP;
    if (ok) {
        info.pitch = svga_read(R_BYTES_PER_LINE);
        u32 off = svga_read(R_FB_OFFSET);
        fb = (volatile u32 *)(FB_VIRT + off);
        if (off + info.pitch * h > FB_VIRT_SZ) ok = false;
    }
    irq_restore(f);
    return ok;
}

static bool svga_init(void) {
    struct pci_dev d;
    if (!pci_find_device(SVGA_VENDOR, SVGA_DEVICE, &d)) return false;
    u32 cmd = pci_read(d.bus, d.dev, d.func, 0x04);
    pci_write(d.bus, d.dev, d.func, 0x04, cmd | 0x07);           /* порты, память, bus master */
    svga_io = d.bar0 & ~3u;
    svga_write(R_ID, SVGA_ID_2);
    if (svga_read(R_ID) != SVGA_ID_2) return false;

    info.phys = svga_read(R_FB_START);
    info.vram = svga_read(R_VRAM_SIZE);
    info.max_w = svga_read(R_MAX_WIDTH);
    info.max_h = svga_read(R_MAX_HEIGHT);
    svga_caps = svga_read(R_CAPS);
    u32 fifo_phys = svga_read(R_MEM_START);
    fifo_size = svga_read(R_MEM_SIZE);
    if (fifo_size > 0x40000) fifo_size = 0x40000;
    if (!info.phys || !fifo_phys || fifo_size < 0x4000) return false;

    for (u32 off = 0; off < fifo_size; off += 4096)
        if (vmm_map(FIFO_VIRT + off, fifo_phys + off, PG_WRITE | 0x18)) return false;
    fifo = (volatile u32 *)FIFO_VIRT;
    fifo[F_MIN] = F_NUM_REGS * 4;
    fifo[F_MAX] = fifo_size;
    fifo[F_NEXT_CMD] = F_NUM_REGS * 4;
    fifo[F_STOP] = F_NUM_REGS * 4;
    svga_write(R_GUEST_ID, 0x5010);                               /* «другая ОС» */
    svga_write(R_CONFIG_DONE, 1);

    info.caps = FB_CAP_COPY | FB_CAP_FILL;                        /* без GPU — сделаем процессором */
    if (svga_caps & CAP_RECT_COPY) info.caps |= FB_CAP_GPU;     /* заливка — тоже через копирование */
    if (svga_caps & CAP_CURSOR) info.caps |= FB_CAP_CURSOR;
    if ((svga_caps & CAP_CURSOR) && (svga_caps & CAP_ALPHA_CURSOR)) info.caps |= FB_CAP_ALPHA_CURSOR;
    info.driver = "VMware SVGA II";
    info.hwcaps = svga_caps;
    drv = DRV_SVGA;
    return true;
}

/* ================================================================ общее */

/* Шрифт текстового режима живёт в «плоскости 2» видеопамяти: 256 символов
 * по 32 байта (используются первые 16). Чтобы прочитать его, на время
 * перенастраиваем секвенсор и графический контроллер VGA так, чтобы по адресу
 * 0xA0000 была видна именно плоскость 2 — а потом возвращаем как было. */
static void grab_font(void) {
    outw(0x3C4, 0x0402);    /* запись — только в плоскость 2 */
    outw(0x3C4, 0x0704);    /* последовательная адресация */
    outw(0x3CE, 0x0204);    /* чтение из плоскости 2 */
    outw(0x3CE, 0x0005);    /* выключить режим чёт/нечет */
    outw(0x3CE, 0x0406);    /* окно 0xA0000, 64 КБ */
    const volatile u8 *src = (const volatile u8 *)0xA0000;
    for (int c = 0; c < 256; c++)
        for (int r = 0; r < FONT_H; r++)
            font[c * FONT_H + r] = src[c * 32 + r];
    outw(0x3C4, 0x0302);    /* вернуть настройки текстового режима */
    outw(0x3C4, 0x0304);
    outw(0x3CE, 0x0004);
    outw(0x3CE, 0x1005);
    outw(0x3CE, 0x0E06);
}

static bool bochs_init(void) {
    if (vbe_read(VBE_ID) < 0xB0C0) return false;                  /* нет Bochs VBE */
    /* видеокарта = PCI-устройство класса 3 (display); её память — BAR0 */
    struct pci_dev d;
    info.phys = 0xE0000000u;                                      /* адрес по умолчанию у Bochs */
    if (pci_find_class(0x03, 0x00, &d) && (d.bar0 & ~0xFu) && !(d.bar0 & 1)) info.phys = d.bar0 & ~0xFu;
    u32 vram = vbe_read(VBE_VRAM64K) * 65536u;
    info.vram = vram >= 0x400000 ? vram : 0x400000;
    info.max_w = FB_MAX_W;
    info.max_h = FB_MAX_H;
    info.caps = FB_CAP_COPY | FB_CAP_FILL;
    info.driver = "Bochs VBE";
    drv = DRV_BOCHS;
    return true;
}

bool fb_mode_ok(u32 w, u32 h) {
    if (drv==DRV_FIRMWARE) return w==info.width && h==info.height;
    if (!drv || w < 640 || h < 480 || (w & 7)) return false;
    if (w > info.max_w || h > info.max_h || w > FB_MAX_W || h > FB_MAX_H) return false;
    u32 room = info.vram < FB_VIRT_SZ ? info.vram : FB_VIRT_SZ;
    return w * h * 4 <= room;
}

/* ---------------------------------------------------------------- «грязная» область
 * Что мы нарисовали процессором, но ещё не показали (UPDATE у SVGA). */
static int dx0 = 1 << 30, dy0 = 1 << 30, dx1, dy1;

static void mark(int x, int y, int w, int h) {
    if ((drv != DRV_SVGA && drv != DRV_VIRTIO) || w <= 0 || h <= 0) return;
    u32 f = irq_save();
    if (x < dx0) dx0 = x;
    if (y < dy0) dy0 = y;
    if (x + w > dx1) dx1 = x + w;
    if (y + h > dy1) dy1 = y + h;
    irq_restore(f);
}

static void present(void) {
    if (drv == DRV_VIRTIO) {
        u32 f = irq_save();
        if (dx1 > dx0 && dy1 > dy0) {
            vgpu_upload(dx0, dy0, dx1 - dx0, dy1 - dy0);
            dx0 = dy0 = 1 << 30; dx1 = dy1 = 0;
        }
        if (!frame_depth) vgpu_commit();
        irq_restore(f);
        return;
    }
    if (drv != DRV_SVGA || frame_depth) return;
    u32 f = irq_save();
    if (dx1 > dx0 && dy1 > dy0) {
        u32 c[5] = { CMD_UPDATE, dx0, dy0, dx1 - dx0, dy1 - dy0 };
        fifo_cmd(c, 5);
    }
    dx0 = dy0 = 1 << 30;
    dx1 = dy1 = 0;
    irq_restore(f);
}

static u32 tick_count;
void fb_tick(void) {
    if (drv == DRV_VIRTIO) vgpu_cursor_tick();
    if (!ready || frame_depth) return;
    present();
    if (!gpu_ok && probe_tries && (++tick_count & 7) == 0) gpu_probe();
}

/* процессор сейчас будет писать в видеопамять — карта должна закончить своё */
static inline void cpu_touch(void) {
    if (gpu_busy) { u32 f = irq_save(); if (gpu_busy) svga_sync(); irq_restore(f); }
}

/* ---------------------------------------------------------------- готова ли карта копировать
 * QEMU проверяет команды по своему «образу экрана», а меняет его размер только
 * при очередной перерисовке окна (а без окна, -display none, — никогда). До
 * этого RECT_COPY в новом режиме отбрасывается или копирует не то. Поэтому
 * после смены режима проверяем: процессор ставит в углу экрана метку,
 * карта копирует её на соседний пиксель — сошлось, значит можно доверять. */
static bool cursor_on, cursor_defined;
static void gpu_copy(int sx, int sy, int dx, int dy, int w, int h);

static void gpu_probe(void) {
    if (drv != DRV_SVGA || !(info.caps & FB_CAP_GPU) || gpu_ok || !probe_tries) return;
    u32 f = irq_save();
    probe_tries--;
    int x = info.width - 1, y = info.height - 1;
    volatile u32 *a = fb + y * stride + x, *b = a - 1;
    u32 sa = *a, sb = *b, magic = 0x00A5C3E1u ^ (info.gen << 8) ^ probe_tries;
    svga_sync();
    *a = magic;
    *b = ~magic & 0xFFFFFF;
    gpu_copy(x, y, x - 1, y, 1, 1);
    svga_sync();
    gpu_ok = *b == magic;
    *a = sa;                        /* вернуть как было (на экран это не попадало) */
    *b = sb;
    irq_restore(f);
}

static inline bool use_gpu(void) {
    if (drv != DRV_SVGA || !(info.caps & FB_CAP_GPU)) return false;
    if (!gpu_ok) gpu_probe();
    return gpu_ok;
}

static bool clip(int *x, int *y, int *w, int *h) {
    if (*x < 0) { *w += *x; *x = 0; }
    if (*y < 0) { *h += *y; *y = 0; }
    if (*x + *w > (int)info.width)  *w = info.width - *x;
    if (*y + *h > (int)info.height) *h = info.height - *y;
    return *w > 0 && *h > 0;
}

int fb_set_mode(u32 w, u32 h) {
    if (!fb_mode_ok(w, h)) return -1;
    if (drv==DRV_FIRMWARE) return 0;
    u32 ow = info.width, oh = info.height;
    if (drv == DRV_VIRTIO) {
        u32 f = irq_save();
        bool ok = vgpu_mode(w, h);
        if (ok) {
            info.width = w; info.height = h; info.pitch = w * 4; info.bpp = FB_BPP;
            stride = w; fb = (volatile u32 *)FB_VIRT; info.gen++;
            dx0 = dy0 = 1 << 30; dx1 = dy1 = 0;
        }
        irq_restore(f);
        return ok ? 0 : -1;
    }
    bool ok = drv == DRV_SVGA ? svga_set_mode(w, h) : bochs_set_mode(w, h);
    if (!ok) {                                                    /* не вышло — вернуть старый */
        if (ow) { if (drv == DRV_SVGA) svga_set_mode(ow, oh); else bochs_set_mode(ow, oh); }
        return -1;
    }
    u32 f = irq_save();
    info.width = w;
    info.height = h;
    info.bpp = FB_BPP;
    stride = info.pitch / 4;
    info.gen++;
    dx0 = dy0 = 1 << 30; dx1 = dy1 = 0;
    gpu_ok = false;
    probe_tries = 400;              /* ~30 секунд проверок из таймера */
    irq_restore(f);
    return 0;
}

bool fb_init(void) {
    const struct boot_info *boot=boot_info_get();
    if (boot && boot->fb_phys) {
        if (boot->font_phys!=0x5000 || boot->bpp!=32 || boot->width<640 || boot->width>FB_MAX_W ||
            boot->height<480 || boot->height>FB_MAX_H ||
            (boot->fb_phys&4095) || (boot->pitch&3) ||
            boot->pitch<boot->width*4 || boot->pitch>FB_VIRT_SZ/boot->height ||
            boot->fb_phys>0xffffffffu-boot->pitch*boot->height) return false;
        drv=DRV_FIRMWARE;
        info.phys=boot->fb_phys; info.width=boot->width; info.height=boot->height;
        info.bpp=32; info.pitch=boot->pitch; info.vram=boot->pitch*boot->height;
        info.max_w=boot->width; info.max_h=boot->height;
        info.caps=FB_CAP_COPY|FB_CAP_FILL; info.gen=1;
        info.driver="BIOS VBE framebuffer";
    } else {
        /* Capture the BIOS font before a VirtIO scanout takes over VGA. */
        if (vgpu_init(&info)) {
            grab_font();
            drv = DRV_VIRTIO;
            if (fb_set_mode(FB_DEF_W, FB_DEF_H) < 0) return false;
            ready = true;
            return true;
        }
        if (!svga_init() && !bochs_init()) return false;
    }

    /* отобразить видеопамять в ядро — до создания процессов, тогда эти записи
     * каталога попадут и во все каталоги процессов */
    u32 size = info.vram < FB_VIRT_SZ ? info.vram : FB_VIRT_SZ;
    for (u32 off = 0; off < size; off += 4096)
        if (vmm_map(FB_VIRT + off, info.phys + off, PG_WRITE)) return false;

    if (drv==DRV_FIRMWARE) {
        memcpy(font,(const void *)boot->font_phys,sizeof(font));
        fb=(volatile u32 *)FB_VIRT; stride=info.pitch/4;
    } else {
        grab_font();
        if (fb_set_mode(FB_DEF_W, FB_DEF_H) < 0) return false;
    }
    ready = true;
    fb_fill(0, 0, info.width, info.height, 0);
    return true;
}

bool fb_ready(void) { return ready; }
void fb_get_info(struct fb_info *out) {
    *out = info;
    if (gpu_ok || (drv == DRV_VIRTIO && vgpu_accelerated())) out->caps |= FB_CAP_GPU_ON;
    if (drv == DRV_VIRTIO && vgpu_accelerated()) out->caps |= FB_CAP_BUFFERED | FB_CAP_COMPOSITOR;
    if (cursor_on) out->caps |= FB_CAP_CURSOR_ON;
}
const u8 *fb_font(void) { return font; }

/* ---------------------------------------------------------------- рисование */

void fb_glyph(int col, int row, u8 ch, u32 fg, u32 bg) {
    int x = col * FONT_W, y = row * FONT_H;
    if (x + FONT_W > (int)info.width || y + FONT_H > (int)info.height) return;
    cpu_touch();
    volatile u32 *p = fb + y * stride + x;
    const u8 *g = font + ch * FONT_H;
    for (int j = 0; j < FONT_H; j++, p += stride) {
        u8 bits = g[j];
        p[0] = bits & 0x80 ? fg : bg;  p[1] = bits & 0x40 ? fg : bg;
        p[2] = bits & 0x20 ? fg : bg;  p[3] = bits & 0x10 ? fg : bg;
        p[4] = bits & 0x08 ? fg : bg;  p[5] = bits & 0x04 ? fg : bg;
        p[6] = bits & 0x02 ? fg : bg;  p[7] = bits & 0x01 ? fg : bg;
    }
    mark(x, y, FONT_W, FONT_H);
}

static void cpu_fill(int x, int y, int w, int h, u32 color) {
    cpu_touch();
    for (int j = 0; j < h; j++) {
        volatile u32 *p = fb + (y + j) * stride + x;
        for (int i = 0; i < w; i++) p[i] = color;
    }
    mark(x, y, w, h);
}

static void gpu_copy(int sx, int sy, int dx, int dy, int w, int h) {
    u32 c[7] = { CMD_RECT_COPY, sx, sy, dx, dy, w, h };
    fifo_cmd(c, 7);
    gpu_busy = true;
}

/* Заливка. Команда RECT_FILL у SVGA устаревшая, и QEMU, хоть и объявляет её,
 * не выполняет. Поэтому так: процессор заливает одну строку, а дальше карта
 * копирует уже залитое вниз, каждый раз вдвое больше: 1, 2, 4, 8... строк —
 * на экран 1080 строк это 11 команд вместо миллиона записей в память. */
void fb_fill(int x, int y, int w, int h, u32 color) {
    if (!clip(&x, &y, &w, &h)) return;
    if (h < 8 || !use_gpu()) { cpu_fill(x, y, w, h, color); return; }
    cpu_fill(x, y, w, 1, color);
    u32 f = irq_save();
    present();                                                    /* первая строка — на экран */
    for (int done = 1; done < h; ) {
        int n = done < h - done ? done : h - done;
        gpu_copy(x, y, x, y + done, w, n);
        done += n;
    }
    irq_restore(f);
}

/* быстрые копирования по 4 байта: rep movsl в сотни раз быстрее байтового цикла */
static inline void copy32(volatile void *dst, const volatile void *src, u32 words) {
    __asm__ volatile ("cld; rep movsl" : "+D"(dst), "+S"(src), "+c"(words) : : "memory");
}
static inline void copy32_back(volatile u32 *dst, const volatile u32 *src, u32 words) {
    if (!words) return;
    volatile u32 *d = dst + words - 1;
    const volatile u32 *s = src + words - 1;
    __asm__ volatile ("std; rep movsl; cld" : "+D"(d), "+S"(s), "+c"(words) : : "memory");
}

void fb_copy(int sx, int sy, int dx, int dy, int w, int h) {
    /* обрезаем так, чтобы и источник, и приёмник были на экране */
    if (sx < 0) { w += sx; dx -= sx; sx = 0; }
    if (sy < 0) { h += sy; dy -= sy; sy = 0; }
    if (dx < 0) { w += dx; sx -= dx; dx = 0; }
    if (dy < 0) { h += dy; sy -= dy; dy = 0; }
    int W = info.width, H = info.height;
    if (sx + w > W) w = W - sx;
    if (dx + w > W) w = W - dx;
    if (sy + h > H) h = H - sy;
    if (dy + h > H) h = H - dy;
    if (w <= 0 || h <= 0 || (sx == dx && sy == dy)) return;
    bool virtio_copy = drv == DRV_VIRTIO && vgpu_accelerated();
    if (virtio_copy) {
        u32 f = irq_save();
        /* Upload earlier CPU changes before the GPU reads its source. */
        frame_depth++;
        present();
        virtio_copy = vgpu_copy(sx, sy, dx, dy, w, h);
        frame_depth--;
        irq_restore(f);
    }
    if (use_gpu()) {
        u32 f = irq_save();
        present();
        gpu_copy(sx, sy, dx, dy, w, h);
        irq_restore(f);
        return;
    }
    cpu_touch();
    if (dy < sy || (dy == sy && dx < sx)) {
        for (int j = 0; j < h; j++) copy32(fb + (dy + j) * stride + dx, fb + (sy + j) * stride + sx, w);
    } else if (dy > sy) {
        for (int j = h - 1; j >= 0; j--) copy32(fb + (dy + j) * stride + dx, fb + (sy + j) * stride + sx, w);
    } else {                                                      /* та же строка, вправо */
        for (int j = 0; j < h; j++) copy32_back(fb + (dy + j) * stride + dx, fb + (sy + j) * stride + sx, w);
    }
    /* Keep the RAM shadow coherent for console drawing and software fallback.
     * The visible copy itself was executed between GPU textures. */
    if (!virtio_copy) mark(dx, dy, w, h);
    if (drv == DRV_VIRTIO && !frame_depth) present();
}

void fb_scroll(int pixels, u32 bg) {
    fb_copy(0, pixels, 0, 0, info.width, info.height - pixels);
    fb_fill(0, info.height - pixels, info.width, pixels, bg);
}

void fb_blit(const u32 *src, u32 sstride, int x, int y, int w, int h) {
    if (x < 0 || y < 0 || w <= 0 || h <= 0) return;
    if (x + w > (int)info.width)  w = info.width - x;
    if (y + h > (int)info.height) h = info.height - y;
    if (w <= 0 || h <= 0) return;
    cpu_touch();
    for (int j = 0; j < h; j++)
        copy32(fb + (y + j) * stride + x, src + (y + j) * sstride + x, w);
    mark(x, y, w, h);
    present();                                                    /* программе важна задержка */
}

/* ---------------------------------------------------------------- аппаратный курсор */


int fb_cursor_define(const u32 *argb, int w, int h, int hot_x, int hot_y) {
    if (drv == DRV_VIRTIO) {
        u32 flags=irq_save();
        bool ok=vgpu_cursor_define(argb,w,h,hot_x,hot_y);
        cursor_defined=ok;
        irq_restore(flags);
        return ok ? 0 : -1;
    }
    if (drv != DRV_SVGA || !(info.caps & FB_CAP_CURSOR)) return -1;
    if (w <= 0 || h <= 0 || w > 32 || h > 32) return -1;
    if (info.caps & FB_CAP_ALPHA_CURSOR) {
        /* настоящая прозрачность: 32 бита BGRA на пиксель, цвет уже умножен на альфу
         * (так курсор с мягкой тенью и сглаженным краем рисует сама карта) */
        static u32 a[6 + 32 * 32];
        a[0] = CMD_DEFINE_ALPHA_CURSOR; a[1] = 0; a[2] = hot_x; a[3] = hot_y; a[4] = w; a[5] = h;
        for (int i = 0; i < w * h; i++) {
            u32 p = argb[i], al = p >> 24;
            u32 r = (p >> 16 & 255) * al / 255, g = (p >> 8 & 255) * al / 255, b = (p & 255) * al / 255;
            a[6 + i] = al << 24 | r << 16 | g << 8 | b;
        }
        fifo_cmd(a, 6 + w * h);
        cursor_defined = true;
        return 0;
    }
    /* всегда 32x32: строка маски — ровно одно слово, как ни считай выравнивание */
    static u32 c[8 + 32 + 32 * 32];
    c[0] = CMD_DEFINE_CURSOR; c[1] = 0; c[2] = hot_x; c[3] = hot_y;
    c[4] = 32; c[5] = 32;
    c[6] = 1;                                                     /* AND-маска: 1 бит на пиксель */
    c[7] = 32;                                                    /* картинка: 32 бита на пиксель */
    u32 *mask = c + 8, *img = c + 8 + 32;
    for (int y = 0; y < 32; y++) {
        u8 bytes[4] = { 0xFF, 0xFF, 0xFF, 0xFF };                /* 1 = прозрачно */
        for (int x = 0; x < 32; x++) {
            u32 p = (x < w && y < h) ? argb[y * w + x] : 0;
            bool on = (p >> 24) >= 128;                           /* у карты прозрачность только «да/нет» */
            if (on) bytes[x / 8] &= ~(0x80 >> (x % 8));
            img[y * 32 + x] = on ? (p & 0xFFFFFF) : 0;
        }
        mask[y] = bytes[0] | bytes[1] << 8 | bytes[2] << 16 | (u32)bytes[3] << 24;
    }
    fifo_cmd(c, 8 + 32 + 32 * 32);
    cursor_defined = true;
    return 0;
}

int fb_cursor_enable(bool on) {
    if (drv == DRV_VIRTIO) {
        u32 flags=irq_save();
        bool ok=vgpu_cursor_enable(on);
        if (ok) cursor_on=on;
        irq_restore(flags);
        return ok ? 0 : -1;
    }
    if (on && (!cursor_defined || drv != DRV_SVGA || !(info.caps & FB_CAP_CURSOR))) return -1;
    u32 f = irq_save();
    cursor_on = on;
    if (drv == DRV_SVGA && (info.caps & FB_CAP_CURSOR)) {
        svga_write(R_CURSOR_ID, 0);
        svga_write(R_CURSOR_ON, on ? 1 : 0);
    }
    irq_restore(f);
    return 0;
}

bool fb_cursor_enabled(void) { return cursor_on; }

void fb_cursor_move(int x, int y) {
    if (!cursor_on) return;
    u32 f = irq_save();
    if (drv == DRV_VIRTIO) { vgpu_cursor_move(x,y); irq_restore(f); return; }
    svga_write(R_CURSOR_ID, 0);
    svga_write(R_CURSOR_X, x);
    svga_write(R_CURSOR_Y, y);
    svga_write(R_CURSOR_ON, 1);         /* QEMU применяет положение при записи ON */
    irq_restore(f);
}

/* ---------------------------------------------------------------- владелец экрана
 * Пока программа рисует сама, консоль не должна поверх неё печатать. */
static u32 owner;
void fb_input_notify(void) { if (owner) msg_input_notify(owner); }

void fb_upload_pending(void) {
    u32 f = irq_save();
    frame_depth++; present(); frame_depth--;
    irq_restore(f);
}

int fb_frame(bool begin) {
    u32 f = irq_save();
    if (begin) {
        if (frame_depth >= 8) { irq_restore(f); return -1; }
        frame_depth++;
    } else {
        if (!frame_depth) { irq_restore(f); return -1; }
        frame_depth--;
        if (!frame_depth) present();
    }
    irq_restore(f);
    return 0;
}

int fb_acquire(u32 pid, bool on) {
    if (!ready) return -1;
    if (on) {
        if (owner && owner != pid) return -2;     /* экран уже занят другим */
        owner = pid;
        vga_suspend(true);
    } else if (owner == pid) {
        owner = 0;
        frame_depth = 0; present();
        if (drv == DRV_VIRTIO) { u32 f = irq_save(); vgpu_surfaces_clear(); irq_restore(f); }
        fb_cursor_enable(false);
        vga_suspend(false);
    }
    return 0;
}

bool fb_is_owner(u32 pid) { return owner && owner == pid; }

void fb_release(u32 pid) {
    if (owner && owner == pid) fb_acquire(pid, false);
}
