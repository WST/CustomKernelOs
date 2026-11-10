/* Cursor packets use their own queue and never wait for a rendering fence.
 * In-flight packets remain immutable; when full, retain only the latest move. */
#define CURSOR_RAM 0xE3300000u
static volatile struct desc *cursor_descs;
static volatile u16 *cursor_avail, *cursor_used, *cursor_notify;
static u32 cursor_page, cursor_id, cursor_hot_x, cursor_hot_y;
static u16 cursor_posted, cursor_consumed;
static bool cursor_busy[QN], cursor_available, cursor_visible, cursor_pending, cursor_update;
static int cursor_x, cursor_y;

static bool cursor_queue_init(volatile u16 *base, u32 mult, u32 len) {
    if (cfg->queues < 2) return false;
    cfg->queue_sel = 1;
    if (cfg->queue_size < QN || len < 2 ||
        (cfg->notify_off && mult > (len - 2) / cfg->notify_off)) { cfg->queue_sel = 0; return false; }
    cursor_page = pmm_alloc();
    if (!cursor_page) { cfg->queue_sel = 0; return false; }
    memset((void *)cursor_page, 0, 4096);
    cursor_descs = (void *)cursor_page; cursor_avail = (void *)(cursor_page+128); cursor_used = (void *)(cursor_page+160);
    cursor_avail[0] = 1;
    cfg->queue_size = QN;
    cfg->desc_lo = cursor_page; cfg->desc_hi = 0;
    cfg->avail_lo = cursor_page+128; cfg->avail_hi = 0;
    cfg->used_lo = cursor_page+160; cfg->used_hi = 0;
    cursor_notify = (void *)((u32)base + cfg->notify_off * mult);
    cfg->queue_enable = 1; cfg->queue_sel = 0;
    return true;
}

void vgpu_cursor_tick(void) {
    if (!cursor_available || !live) return;
    while (cursor_consumed != cursor_used[1]) {
        BARRIER();
        volatile u32 *entry = (void *)((u32)cursor_used+4+(cursor_consumed%QN)*8);
        if (entry[0] >= QN) { cursor_available = false; return; }
        cursor_busy[entry[0]] = false; cursor_consumed++;
    }
    if (!cursor_pending) return;
    u32 slot = 0;
    while (slot < QN && cursor_busy[slot]) slot++;
    if (slot == QN) return;
    u32 *p = (void *)(cursor_page + 256 + slot * 56);
    memset(p, 0, 56);
    p[0] = cursor_update ? 0x300 : 0x301;
    p[7] = cursor_x; p[8] = cursor_y;
    p[10] = cursor_visible ? cursor_id : 0; p[11] = cursor_hot_x; p[12] = cursor_hot_y;
    cursor_descs[slot] = (struct desc){ (u32)p, 56, 0, 0 };
    cursor_busy[slot] = true;
    cursor_avail[2+(cursor_posted%QN)] = slot;
    BARRIER(); cursor_avail[1] = ++cursor_posted; BARRIER(); *cursor_notify = 1;
    cursor_pending = cursor_update = false;
}

bool vgpu_cursor_define(const u32 *argb, int w, int h, int hot_x, int hot_y) {
    if (!cursor_available || !live || w < 1 || h < 1 || w > 32 || h > 32 ||
        hot_x < 0 || hot_y < 0 || hot_x >= w || hot_y >= h) return false;
    if (!cursor_id) {
        cursor_id = create_format(64, 64, 1);
        if (!cursor_id || !attach_memory(cursor_id, CURSOR_RAM, 64*64*4)) return false;
    }
    u32 *pixels = (void *)CURSOR_RAM;
    memset(pixels, 0, 64*64*4);
    for (int y=0; y<h; y++) memcpy(pixels+y*64, argb+y*w, w*4);
    memset(command, 0, 72);
    u32 *p = (void *)command;
    if (virgl) {
        p[9]=64; p[10]=64; p[11]=1; p[14]=cursor_id; p[16]=64*4; p[17]=64*64*4;
        if (!send(72, 0x205, CTX)) return false;
    } else {
        p[8]=64; p[9]=64; p[12]=cursor_id;
        if (!send(56, 0x105, 0)) return false;
    }
    cursor_hot_x=hot_x; cursor_hot_y=hot_y; cursor_update=cursor_pending=true;
    vgpu_cursor_tick();
    return true;
}

bool vgpu_cursor_enable(bool on) {
    if (!cursor_available || (on && !cursor_id)) return !on;
    cursor_visible=on; cursor_update=cursor_pending=true; vgpu_cursor_tick();
    return true;
}

void vgpu_cursor_move(int x, int y) {
    cursor_x=x; cursor_y=y;
    if (cursor_visible) { cursor_pending=true; vgpu_cursor_tick(); }
}
