#ifndef VIDEO_DRIVER_H
#define VIDEO_DRIVER_H
#include "../../kernel/bootinfo.h"
#include "../../kernel/fb.h"
/* Firmware establishes scanout; these drivers adopt and validate it. */
bool video_firmware_init(const struct boot_info *boot, struct fb_info *info);
void nvidia_firmware_identify(struct fb_info *info);
bool video_runtime_init(const struct boot_info *boot,struct fb_info *info);
bool video_runtime_mode_ok(u32 width,u32 height);
bool video_runtime_set_mode(u32 width,u32 height,struct fb_info *info);
#endif
