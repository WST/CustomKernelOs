#ifndef VIRTGPU_H
#define VIRTGPU_H
#include "fb.h"

#define VGPU_SURFACE_LIMIT 70

/* VirtIO 1.0 PCI control queue; VirGL textures are never mapped as a LFB.
 * FB_VIRT is a RAM upload surface. Completed frames are switched at scanout. */
bool vgpu_init(struct fb_info *info);
bool vgpu_mode(u32 w, u32 h);
bool vgpu_upload(int x, int y, int w, int h);
bool vgpu_commit(void);
bool vgpu_copy(int sx, int sy, int dx, int dy, int w, int h);
bool vgpu_accelerated(void);
u32 vgpu_error(void);
bool vgpu_cursor_define(const u32 *argb, int w, int h, int hot_x, int hot_y);
bool vgpu_cursor_enable(bool on);
void vgpu_cursor_move(int x, int y);
void vgpu_cursor_tick(void);
bool vgpu_surface_upload(u32 slot, const u32 *pixels, u32 w, u32 h, bool alpha);
bool vgpu_surface_update(u32 slot, const u32 *pixels, u32 sw, u32 sh, u32 x, u32 y, u32 w, u32 h);
bool vgpu_surface_blit(u32 slot, int sx, int sy, int dx, int dy, int w, int h);
void vgpu_surfaces_clear(void);
bool vgpu_readback(u32 *pixels);
#endif
