/* Compare every incremental drag frame with a full software recomposition. */
#define main desktop_main
#include "../user/desktop.c"
#undef main
int main(void) {
    if(gfx_open()<0)return 1;
    SW=gfx_w;SH=gfx_h;back=gfx_buf;gpu_scene=false;screen_copy_accelerated=false;
    load_apps();make_wallpaper();dock_build();
    struct win *w=&wins[0];w->used=true;w->app=-1;w->x=200;w->y=120;
    w->w=w->bw=320;w->h=w->bh=220;w->px=malloc(w->w*w->h*4);
    color_t *snapshot=malloc(SW*SH*4);if(!w->px || !snapshot)return 1;
    for(int y=0;y<w->h;y++)for(int x=0;x<w->w;x++)w->px[y*w->w+x]=RGB(x%256,y%256,(x+y)%256);
    strcpy(w->title,"Drag regression");zorder[0]=0;nz=1;focus=0;
    mouse_x=w->x+60;mouse_y=w->y+20;add_dirty(0,0,SW,SH);flush();
    const int positions[][2]={{214,120},{214,150},{190,150},{190,100},{220,130},{-100,80},{SW-140,SH-120},{200,120}};
    for(unsigned i=0;i<sizeof(positions)/sizeof(*positions);i++) {
        dirty_cursor(mouse_x,mouse_y);mouse_x=positions[i][0]+60;mouse_y=positions[i][1]+20;dirty_cursor(mouse_x,mouse_y);
        /* Pending client updates must reach the cached source before moving. */
        w->px[15*w->bw+20]^=0x00ffffff;add_dirty(w->x+20,w->y+HEAD_H+15,1,1);
        move_window(w,positions[i][0],positions[i][1]);flush();
        memcpy(snapshot,back,SW*SH*4);compose(mkrect(0,0,SW,SH));
        if(memcmp(snapshot,back,SW*SH*4)) {
            printf("DESKTOP DRAG FAIL frame=%u\n",i);gfx_close();return 1;
        }
    }
    gfx_close();free(snapshot);free(w->px);puts("DESKTOP DRAG PASS: clipped moves, cursor, client updates and full-frame equivalence");return 0;
}
