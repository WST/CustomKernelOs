/* Real guest-side protocol and frame tests; installed only on a test disk. */
#include "gfx.h"
static int abs(int v) { return v < 0 ? -v : v; }
static color_t opaque[64 * 64], translucent[64 * 64];
static void require(int ok, const char *what);
static void dump(const char *name) {
    color_t *pixels = malloc(gfx_w * gfx_h * 4);
    require(pixels != NULL, "readback allocation");
    require(gfx_readback(pixels) == 0, "visible GPU readback");
    unsigned hash = 2166136261u;
    for (int i = 0; i < gfx_w * gfx_h; i++) hash = (hash ^ (pixels[i] & 0xffffff)) * 16777619u;
    printf("GPUCHECK PIXELS %s %d %d %u", name, gfx_w, gfx_h, hash);
    int points[][2] = { {10,10}, {50,50}, {70,70}, {120,120}, {220,120},
                       {58,58}, {78,78}, {108,108}, {128,128}, {100,100}, {50,90} };
    for (unsigned i = 0; i < sizeof(points) / sizeof(points[0]); i++)
        printf(" %u", pixels[points[i][1] * gfx_w + points[i][0]] & 0xffffff);
    printf(" %u\n", pixels[gfx_w * gfx_h - 1] & 0xffffff);
    free(pixels);
}
static void require(int ok, const char *what) {
    if (!ok) { gfx_close(); printf("GPUCHECK FAIL: %s\n", what); exit(1); }
}
static void step(const char *name) {
    struct gfx_info2 gi; gfx_info2(&gi);
    if (gi.caps & GFX_CAP_COMPOSITOR) dump(name);
    printf("GPUCHECK %s\n", name);
    while (readkey() <= 0) sleep_ms(10);
}
int main(void) {
    require(gfx_open() == 0, "open");
    require(gfx_set_mode(800, 600) == 0, "800x600");
    gfx_check_mode();
    struct gfx_info2 gi; gfx_info2(&gi);
    printf("GPUCHECK ADAPTER %s CAPS %u\n", gi.driver, gi.caps);
    gfx_clear(RGB(20, 40, 60));
    if (!(gi.caps & GFX_CAP_COMPOSITOR)) {
        gfx_fill_rect(40, 40, 64, 64, RGB(200, 40, 20)); gfx_flip();
        step("SOFTWARE"); gfx_close(); printf("GPUCHECK PASS software\n"); return 0;
    }
    for (int i = 0; i < 64 * 64; i++) { opaque[i] = (i / 64 < 32) ? RGB(200, 40, 20) : RGB(40, 20, 200); translucent[i] = (i / 64 < 32) ? 0x8000c864 : 0x80c86400; }
    int child = fork(); require(child >= 0, "owner test fork");
    if (!child) exit(gfx_surface_upload(0, opaque, 64, 64, false) < 0 ? 0 : 1);
    int status = -1;
    require(waitpid(child, &status, 0) == child && status == 0, "owner-only textures");
    require(gfx_surface_upload(0, opaque, 64, 64, false) == 0, "opaque upload");
    require(gfx_surface_upload(1, translucent, 64, 64, true) == 0, "alpha upload");
    require(gfx_surface_upload(70, opaque, 64, 64, false) < 0, "slot bounds");
    require(gfx_surface_upload(2, (const color_t *)0, 64, 64, false) < 0, "pointer validation");
    require(gfx_surface_blit(0, 0, 0, 790, 590, 64, 64) < 0, "destination bounds");
    require(gfx_frame_begin() == 0, "begin");
    require(gfx_frame_begin() == 0, "nested begin");
    gfx_flip();
    require(gfx_surface_blit(0, 0, 0, 40, 40, 64, 64) == 0, "opaque blit");
    require(gfx_surface_blit(1, 0, 0, 60, 60, 64, 64) == 0, "alpha blit");
    require(gfx_surface_blit(1, 0, 0, 200, 100, 64, 64) == 0, "second alpha layer");
    require(gfx_frame_end() == 0 && gfx_frame_end() == 0, "end");
    step("COMPOSITE");
    require(gfx_frame_begin() == 0, "hidden begin");
    require(gfx_surface_blit(0, 0, 0, 200, 100, 64, 64) == 0, "hidden blit");
    step("HIDDEN");
    require(gfx_frame_end() == 0, "hidden end");
    step("COMMITTED");
    require(gfx_frame_begin() == 0, "copy begin");
    require(gfx_hw_copy(40, 40, 48, 48, 84, 84) == 0, "overlapping GPU copy");
    require(gfx_frame_end() == 0, "copy end");
    step("COPY");
    /* Only the requested rows/columns may reach an already retained texture. */
    opaque[16 * 64 + 16] = RGB(3, 77, 199);
    opaque[17 * 64 + 16] = RGB(8, 91, 123);
    opaque[0] = RGB(255, 0, 255); /* deliberately outside the upload rectangle */
    require(gfx_surface_update(0, opaque, 64, 64, 16, 16, 1, 2) == 0, "partial upload");
    require(gfx_surface_update(0, opaque, 64, 64, 63, 63, 2, 2) < 0, "partial bounds");
    require(gfx_surface_update(0, opaque, 32, 64, 0, 0, 1, 1) < 0, "partial dimensions");
    require(gfx_frame_begin() == 0, "partial begin");
    require(gfx_surface_blit(0, 0, 0, 300, 200, 64, 64) == 0, "partial blit");
    require(gfx_frame_end() == 0, "partial end");
    color_t *check = malloc(gfx_w * gfx_h * 4);
    require(check != NULL, "partial readback allocation");
    require(gfx_readback(check) == 0, "partial readback");
    require((check[216 * gfx_w + 316] & 0xffffff) == RGB(3, 77, 199) &&
            (check[217 * gfx_w + 316] & 0xffffff) == RGB(8, 91, 123) &&
            (check[200 * gfx_w + 300] & 0xffffff) == RGB(200, 40, 20), "partial pixels/stride");
    free(check);
    printf("GPUCHECK PARTIAL PASS\n");
    require(gfx_frame_begin() == 0, "batch begin");
    for (int i = 0; i < 100; i++) {
        require(gfx_surface_blit(0, 0, 0, 400, 400, 64, 64) == 0, "batched opaque");
        require(gfx_surface_blit(1, 0, 0, 400, 400, 64, 64) == 0, "batched alpha");
    }
    require(gfx_frame_end() == 0, "batch end");
    check = malloc(gfx_w * gfx_h * 4);
    require(check && gfx_readback(check) == 0, "batch readback");
    unsigned c = check[410 * gfx_w + 410];
    require(abs((int)((c >> 16) & 255) - 100) <= 2 &&
            abs((int)((c >> 8) & 255) - 120) <= 2 &&
            abs((int)(c & 255) - 60) <= 2, "batch flush ordering");
    free(check);
    printf("GPUCHECK BATCH PASS\n");
    require(gfx_surface_scaled(70,0,0,64,64,0,0,800,600,255)<0,"scaled slot bounds");
    require(gfx_surface_scaled(0,0,0,0,64,0,0,800,600,255)<0,"scaled size bounds");
    require(gfx_surface_scaled(0,0,0,64,64,-1,0,800,600,255)<0,"scaled clip bounds");
    require(gfx_surface_scaled(0,0,0,64,64,0,0,800,600,256)<0,"scaled opacity bounds");
    require(gfx_frame_begin()==0,"scaled begin");
    require(gfx_surface_scaled(0,500,100,128,128,500,100,64,128,255)==0,"scaled left clip");
    require(gfx_surface_scaled(0,500,100,128,128,564,100,64,128,255)==0,"scaled right clip");
    require(gfx_surface_scaled(0,660,100,64,64,0,0,800,600,128)==0,"opaque fade");
    require(gfx_surface_scaled(0,-32,260,128,128,0,0,800,600,255)==0,"offscreen scaled clip");
    require(gfx_frame_end()==0,"scaled end");
    check=malloc(gfx_w*gfx_h*4);
    require(check && gfx_readback(check)==0,"scaled readback");
    require((check[120*gfx_w+520]&0xffffff)==RGB(200,40,20) &&
            (check[212*gfx_w+520]&0xffffff)==RGB(40,20,200) &&
            (check[270*gfx_w+10]&0xffffff)==RGB(200,40,20),"scaled pixels/origin");
    c=check[110*gfx_w+670];
    require(abs((int)((c>>16)&255)-110)<=2 && abs((int)((c>>8)&255)-40)<=2 &&
            abs((int)(c&255)-40)<=2,"scaled opacity pixels");
    free(check);
    require(gfx_frame_begin()==0,"small damage begin");
    require(gfx_surface_blit(0,0,0,700,400,8,8)==0,"small retained damage");
    require(gfx_frame_end()==0,"small damage end");
    check=malloc(gfx_w*gfx_h*4);
    require(check && gfx_readback(check)==0,"retained damage readback");
    require((check[120*gfx_w+520]&0xffffff)==RGB(200,40,20),"damage preserves unchanged scanout");
    free(check);
    printf("GPUCHECK SCALED PASS\n");
    require(gfx_surface_upload(1, NULL, 0, 0, false) == 0, "delete surface");
    require(gfx_surface_blit(1, 0, 0, 0, 0, 1, 1) < 0, "deleted handle");
    require(gfx_set_mode(1280, 720) == 0, "1280x720"); gfx_check_mode();
    require(gfx_surface_blit(0, 0, 0, 0, 0, 1, 1) < 0, "mode clears handles");
    gfx_clear(RGB(20, 40, 60)); gfx_flip();
    require(gfx_surface_upload(0, opaque, 64, 64, false) == 0, "upload after mode");
    require(gfx_frame_begin() == 0, "resized begin");
    require(gfx_surface_blit(0, 0, 0, 1216, 656, 64, 64) == 0, "bottom right");
    require(gfx_frame_end() == 0, "resized end");
    step("RESIZED");
    gfx_close(); printf("GPUCHECK PASS GPU\n"); return 0;
}
