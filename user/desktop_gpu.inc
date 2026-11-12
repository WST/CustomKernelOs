/* Retained GPU scene. Programs still rasterize their own window pixels; the
 * desktop uploads changed surfaces, and VirGL composites windows and alpha
 * decorations into the hidden render target. Software compose stays available
 * for overview, a pending resize, or a resource allocation failure. */
static unsigned gpu_frame_hash[MAXWIN];
static bool gpu_content_ready[MAXWIN];
static unsigned gpu_overlay_hash[5];

static void gpu_reset(void) {
    struct gfx_info2 gi;
    bool valid=gfx_info2(&gi)==0;
    gpu_scene=valid && (gi.caps&GFX_CAP_COMPOSITOR);
    screen_copy_accelerated=valid && (gi.caps&GFX_CAP_GPU_ON);
    gpu_wallpaper_ready = false;
    memset(gpu_overlay_hash, 0, sizeof(gpu_overlay_hash));
    memset(gpu_frame_hash, 0, sizeof(gpu_frame_hash));
    memset(gpu_content_ready, 0, sizeof(gpu_content_ready));
    for (int i = 0; i < MAXWIN; i++) { wins[i].gpu_dirty = true; wins[i].gpu_damage = mkrect(0, 0, 0, 0); }
}

static bool gpu_part(int slot, struct rect image, struct rect clip) {
    struct rect part;
    if (!intersect(image, clip, &part)) return true;
    return gfx_surface_blit(slot, part.x - image.x, part.y - image.y,
                            part.x, part.y, part.w, part.h) == 0;
}

static unsigned frame_hash(int idx) {
    struct win *w = &wins[idx];
    int state[] = { w->w, w->h, w->app, w->maximized, w->resizable,
                    idx == focus, idx == hover_close, idx == hover_min, idx == hover_max };
    unsigned hash = 2166136261u;
    const unsigned char *p = (const unsigned char *)state;
    for (unsigned i = 0; i < sizeof(state); i++) hash = (hash ^ p[i]) * 16777619u;
    for (unsigned i = 0; i < sizeof(w->title); i++) hash = (hash ^ (unsigned char)w->title[i]) * 16777619u;
    return hash | 1;
}

static bool gpu_frame_surface(int idx) {
    unsigned hash = frame_hash(idx);
    if (gpu_frame_hash[idx] == hash) return true;
    struct win *w = &wins[idx];
    /* Maximized windows have only a title bar: do not rasterize millions of
     * transparent content pixels before the first animation frame. */
    int pad=w->maximized?0:SH_PAD;
    int tw = outer_w(w) + pad * 2, th = w->maximized?HEAD_H:outer_h(w)+pad*2;
    struct gfx_canvas cv;
    memset(&cv, 0, sizeof(cv));
    cv.w = tw; cv.h = th; cv.save_buf = gfx_buf; cv.save_w = gfx_w; cv.save_h = gfx_h;
    cv.saved_clip = gfx_clip_get(&cv.cx, &cv.cy, &cv.cw, &cv.ch);
    cv.black = malloc(tw * th * 4); cv.white = malloc(tw * th * 4);
    if (!cv.black || !cv.white) { free(cv.black); free(cv.white); return false; }
    int ox = w->x, oy = w->y;
    w->x = pad; w->y = pad;
    for (int pass = 0; pass < 2; pass++) {
        color_t *pixels = pass ? cv.white : cv.black;
        for (int i = 0; i < tw * th; i++) pixels[i] = pass ? 0xffffff : 0;
        gfx_buf = pixels; gfx_w = tw; gfx_h = th; gfx_no_clip();
        draw_frame(idx);
    }
    w->x = ox; w->y = oy;
    color_t *argb = gfx_canvas_end(&cv);
    int result = gfx_surface_upload(idx * 2, argb, tw, th, true);
    free(argb);
    if (result < 0) return false;
    gpu_frame_hash[idx] = hash;
    return true;
}

/* Each shell layer keeps its own small texture; moving a window does not
 * rasterize the dock, clock or cursor again. Layer order matches compose(). */
static unsigned gpu_hash_bytes(unsigned h, const void *data, unsigned len) {
    const unsigned char *p = data;
    for (unsigned i=0; i<len; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

static unsigned gpu_overlay_key(int layer, struct rect image) {
    int state[] = { layer, image.x, image.y, image.w, image.h };
    unsigned h = gpu_hash_bytes(2166136261u, state, sizeof(state));
    if (layer == 0) {
        int state[] = { hover_dock, focus, overview_open, DOCK_W, DOCK_CELL, DOCK_ICON, n_dock };
        h = gpu_hash_bytes(h, state, sizeof(state));
        for (int i=0; i<n_dock; i++) {
            int state[] = { dock[i].app, dock[i].win, dock_item_windows(i), dock_item_focused(i) };
            h = gpu_hash_bytes(h, state, sizeof(state));
            if (dock[i].app < 0) h = gpu_hash_bytes(h, wins[dock[i].win].title, sizeof(wins[0].title));
        }
    } else if (layer == 1) {
        int state[] = { hover_top, kb_lang, overview_open, sysmenu_open };
        h = gpu_hash_bytes(h, state, sizeof(state));
        h = gpu_hash_bytes(h, clock_text, sizeof(clock_text));
    } else if (layer == 2) {
        h = gpu_hash_bytes(h, &hover_sm, sizeof(hover_sm));
    } else if (layer == 3 && hover_dock >= 0) {
        const char *s = hover_dock == n_dock ? "Show Applications" :
            dock[hover_dock].app >= 0 ? apps[dock[hover_dock].app].name : wins[dock[hover_dock].win].title;
        h = gpu_hash_bytes(h, s, strlen(s));
    } else if (layer == 4) {
        int state[] = { mouse_x, mouse_y, gfx_cursor_get_shape() };
        h = gpu_hash_bytes(h, state, sizeof(state));
    }
    return h | 1;
}

static bool gpu_overlay_layer(int layer, struct rect image, struct rect dirty) {
    if (!intersect(image, mkrect(0, 0, SW, SH), &image) || !intersect(image, dirty, NULL)) return true;
    unsigned key = gpu_overlay_key(layer, image);
    int slot = 65 + layer;
    if (gpu_overlay_hash[layer] == key) return gpu_part(slot, image, dirty);
    int n = image.w * image.h;
    color_t *argb = malloc(n * 4);
    if (!argb) return false;
    gfx_set_clip(image.x, image.y, image.w, image.h);
    for (int pass = 0; pass < 2; pass++) {
        for (int y=0; y<image.h; y++) for (int x=0; x<image.w; x++)
            back[(image.y+y)*SW + image.x+x] = pass ? 0xffffff : 0;
        if (layer == 0) draw_dock();
        else if (layer == 1) draw_topbar();
        else if (layer == 2) draw_sysmenu();
        else if (layer == 3) draw_tip();
        else gfx_cursor_draw(mouse_x, mouse_y);
        for (int y=0; y<image.h; y++) for (int x=0; x<image.w; x++) {
            int k = y * image.w + x;
            color_t p = back[(image.y+y)*SW + image.x+x];
            if (!pass) { argb[k] = p; continue; }
            color_t b = argb[k];
            int a = 255 - (((int)(p&255) - (int)(b&255)) +
                ((int)((p>>8)&255) - (int)((b>>8)&255)) +
                ((int)((p>>16)&255) - (int)((b>>16)&255))) / 3;
            if (a <= 0) { argb[k]=0; continue; }
            if (a > 255) a=255;
            unsigned red=((b>>16)&255)*255/a, green=((b>>8)&255)*255/a, blue=(b&255)*255/a;
            if (red>255) red=255;
            if (green>255) green=255;
            if (blue>255) blue=255;
            argb[k]=(unsigned)a<<24 | red<<16 | green<<8 | blue;
        }
    }
    gfx_no_clip();
    bool ok = gfx_surface_upload(slot, argb, image.w, image.h, true) == 0;
    free(argb);
    if (!ok) return false;
    gpu_overlay_hash[layer] = key;
    return gpu_part(slot, image, dirty);
}

static bool gpu_overlay(struct rect r) {
    if (!gpu_overlay_layer(0, dock_rect(), r) ||
        !gpu_overlay_layer(1, mkrect(0, 0, SW, TOPBAR_H), r)) return false;
    if (sysmenu_open && !gpu_overlay_layer(2,
        mkrect(SW-SM_W-40, TOPBAR_H, SM_W+40, sysmenu_rect().h+30), r)) return false;
    if (!gpu_overlay_layer(3, tip_rect(), r)) return false;
    return hwcur || gpu_overlay_layer(4,
        mkrect(mouse_x-14, mouse_y-14, CURSOR_W+16, CURSOR_H+16), r);
}

static bool gpu_compose(struct rect r) {
    if (!gpu_wallpaper_ready) {
        if (gfx_surface_upload(64, wallpaper, SW, SH, false) < 0) return false;
        gpu_wallpaper_ready = true;
    }
    if (!gpu_part(64, mkrect(0, 0, SW, SH), r)) return false;
    for (int k = 0; k < nz; k++) {
        int idx = zorder[k];
        struct win *w = &wins[idx];
        if ((w->minimized && !w->motion) || !intersect(r, win_full(w), NULL)) continue;
        if (!gpu_frame_surface(idx)) return false;
        if(w->motion) {
            int pad=w->maximized?0:SH_PAD;
            int px=pad*w->visual.w/w->w,py=pad*w->visual.h/outer_h(w);
            int fh=w->maximized?HEAD_H*w->visual.h/outer_h(w):w->visual.h+py*2;
            if(fh<1)fh=1;
            if(gfx_surface_scaled(idx*2,w->visual.x-px,w->visual.y-py,
                w->visual.w+px*2,fh,r.x,r.y,r.w,r.h,w->opacity)<0)return false;
        } else {
            int pad=w->maximized?0:SH_PAD;
            if(!gpu_part(idx*2,mkrect(w->x-pad,w->y-pad,w->w+pad*2,
                w->maximized?HEAD_H:outer_h(w)+pad*2),r))return false;
        }
        if (w->gpu_dirty || !gpu_content_ready[idx]) {
            struct rect d = w->gpu_damage;
            int result = gpu_content_ready[idx] && d.w && d.h
                ? gfx_surface_update(idx * 2 + 1, w->px, w->bw, w->bh, d.x, d.y, d.w, d.h)
                : gfx_surface_upload(idx * 2 + 1, w->px, w->bw, w->bh, false);
            if (result < 0) return false;
            w->gpu_dirty = false; gpu_content_ready[idx] = true;
            w->gpu_damage = mkrect(0, 0, 0, 0);
        }
        struct rect cr = mkrect(w->x, w->y + HEAD_H, w->w, w->h);
        if(w->motion) {
            int head=HEAD_H*w->visual.h/outer_h(w);
            if(gfx_surface_scaled(idx*2+1,w->visual.x,w->visual.y+head,
                w->visual.w,w->visual.h-head,r.x,r.y,r.w,r.h,w->opacity)<0)return false;
        } else if (!gpu_part(idx * 2 + 1, cr, r)) return false;
    }
    return gpu_overlay(r);
}
