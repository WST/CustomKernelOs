#include "video.h"

bool video_firmware_init(const struct boot_info *b, struct fb_info *out) {
    if (!b || !out || b->magic!=BOOT_INFO_MAGIC || b->version!=1 ||
        b->font_phys!=0x5000 || !b->fb_phys || b->bpp!=32 ||
        b->width<640 || b->width>FB_MAX_W || b->height<480 || b->height>FB_MAX_H ||
        (b->fb_phys&4095) || (b->pitch&3) || b->pitch<b->width*4 ||
        b->pitch>FB_VIRT_SZ/b->height || b->fb_phys>0xffffffffu-b->pitch*b->height)
        return false;
    *out=(struct fb_info){
        .phys=b->fb_phys,.width=b->width,.height=b->height,.bpp=32,.pitch=b->pitch,
        .vram=b->pitch*b->height,.max_w=b->width,.max_h=b->height,
        .caps=FB_CAP_COPY|FB_CAP_FILL|FB_CAP_FIXED_MODE,.gen=1,.driver="BIOS VBE framebuffer"
    };
    return true;
}
