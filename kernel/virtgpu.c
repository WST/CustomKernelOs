/* Minimal VirtIO-GPU/VirGL display backend. Protocol references:
 * Linux include/uapi/linux/virtio_{pci,gpu}.h (BSD), and virgl_protocol.h.
 * No userspace command stream is accepted. Only bounded, driver-built blits
 * reach the host renderer. Fenced responses protect upload memory reuse.
 */
#include "virtgpu.h"
#include "pci.h"
#include "paging.h"
#include "pmm.h"
#include "heap.h"
#include "string.h"

#define MMIO_BASE 0xE3000000u
#define QN 8
#define CMD_BYTES 65536
#define BATCH_BYTES 8192
#define FORMAT 2u                 /* PIPE_FORMAT_B8G8R8X8_UNORM */
#define CTX 1u
#define BARRIER() __asm__ volatile ("mfence" : : : "memory")

struct common_cfg {
    u32 feature_sel, feature, guest_sel, guest;
    u16 msix, queues;
    u8 status, generation;
    u16 queue_sel, queue_size, queue_msix, queue_enable, notify_off;
    u32 desc_lo, desc_hi, avail_lo, avail_hi, used_lo, used_hi;
} __attribute__((packed));
struct desc { u64 addr; u32 len; u16 flags, next; } __attribute__((packed));
struct header { u32 type, flags; u64 fence; u32 ctx, padding; } __attribute__((packed));
struct rect { u32 x, y, w, h; };
static volatile struct common_cfg *cfg;
static volatile u16 *notify;
static volatile struct desc *descs;
static volatile u16 *avail, *used;
static u16 qindex;
static u32 queue_page, next_mmio, next_id = 1, front, back, upload;
static u32 width, height, error;
static u64 fence;
static bool virgl, live, changed;
static u32 damage_x0,damage_y0,damage_x1,damage_y1;
static void damage(u32 x,u32 y,u32 w,u32 h) {
    if(!changed) { damage_x0=x;damage_y0=y;damage_x1=x+w;damage_y1=y+h; }
    else {
        if(x<damage_x0)damage_x0=x;
        if(y<damage_y0)damage_y0=y;
        if(x+w>damage_x1)damage_x1=x+w;
        if(y+h>damage_y1)damage_y1=y+h;
    }
    changed=true;
}
static struct { u32 id, w, h, format; } surfaces[VGPU_SURFACE_LIMIT];
static u32 surface_bytes;
static u8 command[CMD_BYTES] __attribute__((aligned(4096)));
static u8 batch[BATCH_BYTES] __attribute__((aligned(4096)));
static u32 batch_words;
static struct header response __attribute__((aligned(16)));
/* Presentation owns immutable command/response storage until the host has
 * acknowledged it. Only display flush is deferred; uploads stay fenced. */
static u8 present_command[48] __attribute__((aligned(16)));
static struct header present_response __attribute__((aligned(16)));
static bool present_pending;
static u64 present_fence;
static struct pci_dev device;

static u32 pciread(u32 off) { return pci_read(device.bus, device.dev, device.func, off); }
static void pciwrite(u32 off, u32 v) { pci_write(device.bus, device.dev, device.func, off, v); }
static u32 pci8(u32 off) { return (pciread(off) >> ((off & 3) * 8)) & 255; }

static void *map_cap(u32 cap) {
    u32 bar = pci8(cap + 4);
    if (bar > 5) return NULL;
    u32 base = pciread(0x10 + bar * 4);
    if ((base & 1) || !base) return NULL;
    if ((base & 6) == 4 && pciread(0x14 + bar * 4)) return NULL;
    u32 off = pciread(cap + 8), len = pciread(cap + 12);
    if (!len || len > 0x10000 || off > 0xffffffffu - (base & ~15u)) return NULL;
    u32 addr = (base & ~15u) + off, start = addr & ~4095u;
    if (addr > 0xffffffffu - len) return NULL;
    u32 bytes = ((addr & 4095) + len + 4095) & ~4095u;
    u32 virt = next_mmio;
    if (virt > MMIO_BASE + 0x100000u - bytes) return NULL;
    for (u32 i = 0; i < bytes; i += 4096)
        if (vmm_map(virt + i, start + i, PG_WRITE | 0x18)) return NULL;
    next_mmio += bytes;
    return (void *)(virt + (addr & 4095));
}

static int control_complete(const struct header *reply,u64 expected,bool block) {
    if(!block && used[1]!=qindex)return 0;
    /* No interrupts are required, including during early boot. The bound also
     * prevents a failed host GPU from trapping the kernel in an infinite wait. */
    u32 spins = 100000000;
    /* Some VirGL hosts retire fences on a coarse timer if the guest goes
     * quiet after submitting. Re-notify the unchanged queue a bounded number
     * of times to let them observe a completed fence promptly. Descriptors,
     * response storage and upload backing remain pinned until completion. */
    u32 gap=1024,until_poll=gap,rechecks=0;
    while (used[1] != qindex && --spins) {
        __asm__ volatile ("pause" : : : "memory");
        if(virgl && rechecks<32 && !--until_poll) {
            BARRIER();*notify=0;rechecks++;
            if(gap<16384)gap*=2;
            until_poll=gap;
        }
    }
    BARRIER();
    if (!spins || reply->type != 0x1100 || reply->fence != expected) {
        error = spins ? reply->type : 0xffff;
        if (!spins) live = false;
        return -1;
    }
    return 1;
}
static bool present_complete(bool block) {
    if(!present_pending)return true;
    int result=control_complete(&present_response,present_fence,block);
    if(result)present_pending=false;
    if(result<0) { live=false;return false; }
    return true;
}
static bool send_now(u8 *packet, u32 bytes, u32 type, u32 ctx) {
    if (!live || bytes < sizeof(struct header) || bytes > CMD_BYTES || !present_complete(true)) return false;
    bool async=virgl && type==0x104 && bytes==sizeof(present_command);
    if(async) { memcpy(present_command,packet,bytes);packet=present_command; }
    struct header *h = (void *)packet;
    h->type = type; h->flags = 1; h->fence = ++fence; h->ctx = ctx; h->padding = 0;
    struct header *reply=async?&present_response:&response;
    reply->type = 0;
    u32 cmd_phys, resp_phys;
    if (!vmm_translate((u32)packet, &cmd_phys) || !vmm_translate((u32)reply, &resp_phys)) return false;
    descs[0] = (struct desc){ cmd_phys, bytes, 1, 1 };
    descs[1] = (struct desc){ resp_phys, sizeof(*reply), 2, 0 };
    avail[2 + (qindex % QN)] = 0;
    BARRIER();avail[1] = ++qindex;BARRIER();*notify = 0;
    if(async) { present_fence=h->fence;present_pending=true;return true; }
    return control_complete(reply,h->fence,true)>0;
}

/* Batch trusted draw/blit streams between transfers and scanout changes.
 * Uploads and readbacks remain synchronous: no user pages outlive a fence. */
static bool flush_batch(void) {
    if (!batch_words) return true;
    memset(batch, 0, 32);
    ((u32 *)batch)[6] = batch_words * 4;
    u32 bytes = 32 + batch_words * 4;
    batch_words = 0;
    return send_now(batch, bytes, 0x207, CTX);
}

static bool send(u32 bytes, u32 type, u32 ctx) {
    if (!live) return false;
    if (type == 0x207 && ctx == CTX) {
        if (bytes < 32 || bytes > CMD_BYTES) return false;
        u32 size = ((u32 *)command)[6];
        if (size != bytes - 32 || (size & 3)) return false;
        if (size > BATCH_BYTES - 32) return flush_batch() && send_now(command, bytes, type, ctx);
        if (32 + batch_words * 4 + size > BATCH_BYTES && !flush_batch()) return false;
        memcpy(batch + 32 + batch_words * 4, command + 32, size);
        batch_words += size / 4;
        return true;
    }
    return flush_batch() && send_now(command, bytes, type, ctx);
}

static bool resource_cmd(u32 type, u32 id, u32 ctx) {
    memset(command, 0, 32);
    ((u32 *)command)[6] = id;
    return send(32, type, ctx);
}

static u32 create_format(u32 w, u32 h, u32 format) {
    u32 id = next_id++;
    memset(command, 0, 72);
    u32 *p = (void *)command;
    p[6] = id;
    if (virgl) {
        p[7] = 2; p[8] = format; p[9] = 2 | 8 | (1u << 18); /* RT, sampler, scanout */
        p[10] = w; p[11] = h; p[12] = 1; p[13] = 1; p[16] = 1; /* Y_0_TOP */
        if (!send(72, 0x204, 0)) return 0;
        if (!resource_cmd(0x202, id, CTX)) { resource_cmd(0x102, id, 0); return 0; }
    } else {
        p[7] = format; p[8] = w; p[9] = h;
        if (!send(40, 0x101, 0)) return 0;
    }
    return id;
}
static u32 create(u32 w, u32 h) { return create_format(w, h, FORMAT); }

static void destroy(u32 id) {
    if (!id || !live) return;
    if (virgl) resource_cmd(0x203, id, CTX);
    resource_cmd(0x102, id, 0);
}

static bool attach_memory(u32 id, u32 base, u32 bytes) {
    u32 n = (bytes + (base & 4095) + 4095) / 4096;
    if (32 + n * 16 > CMD_BYTES) return false;
    memset(command, 0, 32 + n * 16);
    u32 *p = (void *)command;
    p[6] = id; p[7] = n;
    u32 done = 0;
    for (u32 i = 0; i < n; i++) {
        u32 phys;
        u32 addr = base + done, len = 4096 - (addr & 4095);
        if (len > bytes - done) len = bytes - done;
        if (!vmm_translate(addr, &phys)) return false;
        p[8 + i * 4] = phys;
        p[10 + i * 4] = len;
        done += len;
    }
    return send(32 + n * 16, 0x106, 0);
}
static bool attach(u32 id, u32 bytes) { return attach_memory(id, FB_VIRT, bytes); }

static bool transfer(u32 id, int x, int y, int w, int h, bool readback) {
    memset(command, 0, 72);
    u32 *p = (void *)command;
    p[6] = x; p[7] = y;
    if (virgl) {
        p[9] = w; p[10] = h; p[11] = 1;
        p[12] = (y * width + x) * 4;
        p[14] = id; p[16] = width * 4; p[17] = width * height * 4;
        return send(72, readback ? 0x206 : 0x205, CTX);
    }
    p[8] = w; p[9] = h; p[10] = (y * width + x) * 4; p[12] = id;
    return send(56, 0x105, 0);
}

/* Use GL blit through VirGL rather than CPU copies into the visible image.
 * Gallium's BLIT also handles overlap by using a separate scratch texture. */
static bool blit_format(u32 src, u32 dst, int sx, int sy, int dx, int dy, int w, int h, u32 format, u32 source_height) {
    memset(command, 0, 32 + 22 * 4);
    u32 *p = (void *)(command + 32);
    p[0] = 16 | (21u << 16); /* VIRGL_CCMD_BLIT */
    p[1] = 15; /* RGBA, nearest. Alpha layers use the explicit quad shader. */
    /* VirGL BLIT boxes use the opposite vertical origin from transfer rows. */
    p[4] = dst; p[6] = FORMAT; p[7] = dx; p[8] = height - dy - h; p[10] = w; p[11] = h; p[12] = 1;
    p[13] = src; p[15] = format; p[16] = sx; p[17] = source_height - sy - h; p[19] = w; p[20] = h; p[21] = 1;
    ((u32 *)command)[6] = 22 * 4;
    return send(32 + 22 * 4, 0x207, CTX);
}
static bool blit(u32 src, u32 dst, int sx, int sy, int dx, int dy, int w, int h) {
    return blit_format(src, dst, sx, sy, dx, dy, w, h, FORMAT, height);
}

#include "virtgpu_render.inc"

static bool scanout(u32 id) {
    memset(command, 0, 48);
    u32 *p = (void *)command;
    p[8] = width; p[9] = height; p[11] = id;
    return send(48, 0x103, 0);
}

static bool flush(u32 id) {
    memset(command, 0, 48);
    u32 *p = (void *)command;
    p[8] = width; p[9] = height; p[10] = id;
    return send(48, 0x104, 0);
}

bool vgpu_mode(u32 w, u32 h) {
    if (!live || w < 640 || h < 480 || w > FB_MAX_W || h > FB_MAX_H || w * h > FB_VIRT_SZ / 4) return false;
    u32 old_upload = upload, old_front = front, old_back = back;
    u32 old_width = width, old_height = height;
    bool old_changed = changed;
    u32 old_damage[4]={damage_x0,damage_y0,damage_x1,damage_y1};
    /* Preserve the previous mode until every new resource has been verified. */
    u32 old_bytes = width * height * 4;
    void *saved = old_bytes ? kmalloc(old_bytes) : NULL;
    if (old_bytes && !saved) return false;
    if (saved) memcpy(saved, (void *)FB_VIRT, old_bytes);
    u32 new_upload = create(w, h), new_front = 0, new_back = 0;
    if (!new_upload || !attach(new_upload, w * h * 4)) goto fail;
    if (virgl) {
        new_front = create(w, h); new_back = create(w, h);
        if (!new_front || !new_back) goto fail;
    }
    width = w; height = h; upload = new_upload; front = new_front; back = new_back;
    memset((void *)FB_VIRT, 0, w * h * 4);
    if (!transfer(upload, 0, 0, w, h, false)) goto fail;
    if (virgl) {
        /* Verify a real GPU copy/readback before advertising acceleration. */
        volatile u32 *px = (void *)FB_VIRT;
        px[0] = 0x123456; px[1] = 0xabcdef;
        if (!transfer(upload, 0, 0, 2, 1, false) ||
            !blit(upload, back, 0, 0, 0, 0, w, h) || !attach(back, w * h * 4)) goto fail;
        px[0] = px[1] = 0;
        if (!transfer(back, 0, 0, 2, 1, true) || px[0] != 0x123456 || px[1] != 0xabcdef) {
            error = 0xfffe;
            goto fail;
        }
        if (!resource_cmd(0x107, back, 0)) goto fail;
        px[0] = px[1] = 0;
        if (!transfer(upload, 0, 0, 2, 1, false) || !blit(upload, back, 0, 0, 0, 0, w, h)) goto fail;
    }
    changed = false; damage(0,0,w,h);
    if (!vgpu_commit()) goto fail;
    destroy(old_upload); destroy(old_front); destroy(old_back);
    vgpu_surfaces_clear();
    kfree(saved);
    return true;
fail:
    width = old_width; height = old_height;
    upload = old_upload; front = old_front; back = old_back; changed = old_changed;
    damage_x0=old_damage[0];damage_y0=old_damage[1];damage_x1=old_damage[2];damage_y1=old_damage[3];
    if (saved) memcpy((void *)FB_VIRT, saved, old_bytes);
    if (old_upload) { scanout(virgl ? old_front : old_upload); flush(virgl ? old_front : old_upload); }
    destroy(new_upload); destroy(new_front); destroy(new_back);
    kfree(saved);
    return false;
}

#include "virtgpu_cursor.inc"

bool vgpu_init(struct fb_info *info) {
    if (!pci_find_device(0x1af4, 0x1050, &device)) return false;
    cfg = NULL; notify = NULL; next_mmio = MMIO_BASE;
    u32 mult = 0, notify_len = 0;
    for (u32 cap = pci8(0x34) & ~3u, guard = 0; cap >= 0x40 && guard++ < 48; cap = pci8(cap + 1) & ~3u) {
        if (pci8(cap) != 9 || pci8(cap + 2) < 16) continue;
        u32 type = pci8(cap + 3);
        if (type == 1 && pciread(cap + 12) >= sizeof(*cfg)) cfg = map_cap(cap);
        if (type == 2 && pci8(cap + 2) >= 20) {
            notify = map_cap(cap); mult = pciread(cap + 16); notify_len = pciread(cap + 12);
        }
    }
    if (!cfg || !notify) return false;
    pciwrite(4, (pciread(4) & 0xffff) | 6 | (1u << 10)); /* memory, bus master; poll, no INTx */
    cfg->status = 0;
    for (u32 n = 1000000; cfg->status && --n;) __asm__ volatile ("pause");
    if (cfg->status) return false;
    cfg->status = 3;
    cfg->feature_sel = 0; virgl = (cfg->feature & 1) != 0;
    cfg->feature_sel = 1;
    if (!(cfg->feature & 1)) return false; /* VIRTIO_F_VERSION_1 */
    cfg->guest_sel = 0; cfg->guest = virgl ? 1 : 0;
    cfg->guest_sel = 1; cfg->guest = 1;
    cfg->status = 11;
    if (!(cfg->status & 8)) return false;
    cfg->queue_sel = 0;
    if (cfg->queue_size < QN) return false;
    cfg->queue_size = QN;
    queue_page = pmm_alloc();
    if (!queue_page) return false;
    memset((void *)queue_page, 0, 4096);
    descs = (void *)queue_page; avail = (void *)(queue_page + 128); used = (void *)(queue_page + 160);
    avail[0] = 1; /* VRING_AVAIL_F_NO_INTERRUPT */
    cfg->desc_lo = queue_page; cfg->desc_hi = 0;
    cfg->avail_lo = queue_page + 128; cfg->avail_hi = 0;
    cfg->used_lo = queue_page + 160; cfg->used_hi = 0;
    if (notify_len < 2 || (cfg->notify_off && mult > (notify_len - 2) / cfg->notify_off)) return false;
    volatile u16 *notify_base = notify;
    notify = (void *)((u32)notify + cfg->notify_off * mult);
    cfg->queue_enable = 1;
    bool have_cursor = cursor_queue_init(notify_base, mult, notify_len);
    cfg->status = 15;
    live = true;
    if (virgl) {
        memset(command, 0, 96);
        ((u32 *)command)[6] = 4;
        memcpy(command + 32, "MyOS", 4);
        if (!send(96, 0x200, CTX)) return false;
    }
    /* Map before process page directories are created. Kernel upload memory
     * stays private; backing entries describe its actual physical pages. */
    for (u32 off = 0; off < FB_VIRT_SZ; off += 4096) {
        u32 phys = pmm_alloc();
        if (!phys || vmm_map(FB_VIRT + off, phys, PG_WRITE)) return false;
    }
    if (have_cursor) {
        for (u32 off=0; off<64*64*4; off+=4096) {
            u32 phys=pmm_alloc();
            if (!phys || vmm_map(CURSOR_RAM+off, phys, PG_WRITE)) { have_cursor=false; break; }
        }
    }
    cursor_available = have_cursor;
    memset(info, 0, sizeof(*info));
    info->vram = FB_VIRT_SZ; info->max_w = FB_MAX_W; info->max_h = FB_MAX_H;
    info->driver = virgl ? "VirtIO GPU / VirGL" : "VirtIO GPU (software)";
    info->caps = FB_CAP_COPY | FB_CAP_FILL;
    if (virgl) info->caps |= FB_CAP_GPU;
    if (have_cursor) info->caps |= FB_CAP_CURSOR | FB_CAP_ALPHA_CURSOR;
    return true;
}

bool vgpu_upload(int x, int y, int w, int h) {
    if (!transfer(upload, x, y, w, h, false)) return false;
    if (virgl && !blit(upload, back, x, y, x, y, w, h)) return false;
    damage(x,y,w,h);
    return true;
}

bool vgpu_commit(void) {
    if (!live) return false;
    if(!present_complete(false))return false;
    if (!changed) return true;
    if (!scanout(virgl ? back : upload) || !flush(virgl ? back : upload)) return false;
    if (virgl) {
        u32 old = front; front = back; back = old;
        /* Both targets already agree outside this frame's damage. Copy only
         * the changed area, rather than a 16 MiB screen on every small move. */
        if (!blit(front, back, damage_x0,damage_y0,damage_x0,damage_y0,
                  damage_x1-damage_x0,damage_y1-damage_y0)) return false;
    }
    changed = false;
    return true;
}

bool vgpu_copy(int sx, int sy, int dx, int dy, int w, int h) {
    if (!virgl || !live) return false;
    /* The upload texture is scratch here: two distinct resources make even
     * overlapping scroll/drag copies well defined on every OpenGL backend. */
    if (!blit(back, upload, sx, sy, 0, 0, w, h) || !blit(upload, back, 0, 0, dx, dy, w, h)) return false;
    damage(dx,dy,w,h);
    return true;
}

bool vgpu_accelerated(void) { return live && virgl && back; }
u32 vgpu_error(void) { return error; }

void vgpu_surfaces_clear(void) {
    for (u32 i = 0; i < VGPU_SURFACE_LIMIT; i++) {
        destroy(surfaces[i].id);
        memset(&surfaces[i], 0, sizeof(surfaces[i]));
    }
    surface_bytes = 0;
}

bool vgpu_surface_upload(u32 slot, const u32 *pixels, u32 w, u32 h, bool alpha) {
    if (!vgpu_accelerated() || slot >= VGPU_SURFACE_LIMIT) return false;
    if (!w && !h) {
        destroy(surfaces[slot].id);
        surface_bytes -= surfaces[slot].w * surfaces[slot].h * 4;
        memset(&surfaces[slot], 0, sizeof(surfaces[slot]));
        return true;
    }
    if (!w || !h || w > 4096 || h > 4096 || w * h > FB_VIRT_SZ / 4) return false;
    u32 bytes = w * h * 4, format = alpha ? 1 : FORMAT;
    u32 old_bytes = surfaces[slot].w * surfaces[slot].h * 4;
    if (surface_bytes - old_bytes + bytes > 128u * 1024 * 1024) return false;
    if (surfaces[slot].w != w || surfaces[slot].h != h || surfaces[slot].format != format) {
        u32 id = create_format(w, h, format);
        if (!id) return false;
        destroy(surfaces[slot].id);
        surfaces[slot].id = id; surfaces[slot].w = w; surfaces[slot].h = h; surfaces[slot].format = format;
        surface_bytes = surface_bytes - old_bytes + bytes;
    }
    u32 id = surfaces[slot].id;
    /* Pin only for this synchronous, fenced upload, never beyond the syscall.
     * The syscall validates the full range and disables task switches. */
    if (!attach_memory(id, (u32)pixels, bytes)) return false;
    memset(command, 0, 72);
    u32 *p = (void *)command;
    p[9] = w; p[10] = h; p[11] = 1; p[14] = id; p[16] = w * 4; p[17] = bytes;
    bool ok = send(72, 0x205, CTX);
    bool detached = resource_cmd(0x107, id, 0);
    return ok && detached;
}

bool vgpu_surface_update(u32 slot, const u32 *pixels, u32 sw, u32 sh, u32 x, u32 y, u32 w, u32 h) {
    if (!vgpu_accelerated() || slot >= VGPU_SURFACE_LIMIT || !surfaces[slot].id ||
        sw != surfaces[slot].w || sh != surfaces[slot].h || !w || !h ||
        x >= sw || y >= sh || w > sw - x || h > sh - y) return false;
    u32 id = surfaces[slot].id;
    if (!attach_memory(id, (u32)pixels, sw * sh * 4)) return false;
    memset(command, 0, 72);
    u32 *p = (void *)command;
    p[6] = x; p[7] = y; p[9] = w; p[10] = h; p[11] = 1;
    p[12] = (y * sw + x) * 4; p[14] = id; p[16] = sw * 4; p[17] = sw * sh * 4;
    bool ok = send(72, 0x205, CTX);
    bool detached = resource_cmd(0x107, id, 0);
    return ok && detached;
}

bool vgpu_surface_blit(u32 slot, int sx, int sy, int dx, int dy, int w, int h) {
    if (!vgpu_accelerated() || slot >= VGPU_SURFACE_LIMIT || !surfaces[slot].id || sx < 0 || sy < 0 || dx < 0 || dy < 0 || w <= 0 || h <= 0) return false;
    if ((u32)sx > surfaces[slot].w || (u32)sy > surfaces[slot].h ||
        (u32)w > surfaces[slot].w - sx || (u32)h > surfaces[slot].h - sy ||
        (u32)dx > width || (u32)dy > height || (u32)w > width - dx || (u32)h > height - dy) return false;
    bool ok = surfaces[slot].format == 1
        ? alpha_blit(surfaces[slot].id, surfaces[slot].w, surfaces[slot].h, sx, sy, dx, dy, w, h, 1,255)
        : blit_format(surfaces[slot].id, back, sx, sy, dx, dy, w, h, FORMAT, surfaces[slot].h);
    if (!ok) return false;
    damage(dx,dy,w,h);
    return true;
}

/* Draw a complete retained surface into a destination rectangle, clipped in
 * destination space. UVs keep their original phase across dirty rectangles. */
bool vgpu_surface_scaled(u32 slot,int x,int y,int w,int h,int cx,int cy,int cw,int ch,u32 opacity) {
    if(!vgpu_accelerated() || slot>=VGPU_SURFACE_LIMIT || !surfaces[slot].id ||
       w<=0 || h<=0 || w>8192 || h>8192 || x < -8192 || y < -8192 ||
       x>8192 || y>8192 || cx<0 || cy<0 || cw<=0 || ch<=0 ||
       (u32)cx>width || (u32)cy>height || (u32)cw>width-cx || (u32)ch>height-cy || opacity>255) return false;
    int left=x>cx?x:cx, top=y>cy?y:cy;
    int right=x+w<cx+cw?x+w:cx+cw, bottom=y+h<cy+ch?y+h:cy+ch;
    if(left>=right || top>=bottom || !opacity)return true;
    if(!alpha_blit(surfaces[slot].id,w,h,left-x,top-y,left,top,right-left,bottom-top,surfaces[slot].format,opacity)) return false;
    damage(left,top,right-left,bottom-top);
    return true;
}

bool vgpu_readback(u32 *pixels) {
    if (!vgpu_accelerated() || !attach_memory(front, (u32)pixels, width * height * 4)) return false;
    bool ok = transfer(front, 0, 0, width, height, true);
    bool detached = resource_cmd(0x107, front, 0);
    return ok && detached;
}
