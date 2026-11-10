/* Native paint benchmark: run from a desktop session, without network or timers. */
#ifndef BROWSER_PAINT_SOURCE
#define BROWSER_PAINT_SOURCE "../user/browser.c"
#endif
#ifndef PAINT_LABEL
#define PAINT_LABEL "updated"
#endif
#define main browser_app_main
#include BROWSER_PAINT_SOURCE
#undef main

int main(int argc,char **argv) {
    int count=argc>1?atoi(argv[1]):20;
    if(count<1 || count>1000)return 1;
    W=1280;H=800;win=win_create("Browser paint benchmark",W,H);
    if(!win)return 1;
    W=win->w;H=win->h;
    items=malloc(sizeof(struct item)*MAX_ITEMS);boxes=malloc(sizeof(struct box)*MAX_BOXES);
    body=malloc(MAX_PAGE+1);arena_cap=1024*1024;arena=malloc(arena_cap);
    if(!items || !boxes || !body || !arena){win_destroy(win);return 1;}
    init_styles();dom_init(&dom_host);dom_set_viewport(view_w(),VIEW_H);
    const char *html="<style>body{margin:0;background:#f3f5f8}.card{height:220px;padding:24px;margin:16px;"
        "background:#fb8b24;color:white;border-radius:18px;box-sizing:border-box}h2{font-size:32px}</style>"
        "<div class=card><h2>Rendering benchmark</h2><p>Rounded panels and cached text.</p></div>"
        "<div class=card><h2>Second panel</h2><p>Scroll by the same 48 pixels as the mouse wheel.</p></div>"
        "<div class=card><h2>Third panel</h2><p>Repaint only the newly exposed area.</p></div>"
        "<div class=card><h2>Fourth panel</h2></div><div class=card><h2>Fifth panel</h2></div>";
    page_is_dom=true;dom_load(html,strlen(html),"http://paint.local/");relayout(false);redraw();
    unsigned start=ticks();
    for(int i=0;i<count;i++)draw_page();
    unsigned paint=ticks()-start;start=ticks();
    for(int i=0;i<count;i++){scroll_by(i%2?-48:48);redraw();}
    printf("PAINTBENCH %s frames=%d width=%d height=%d page_ms=%u scroll_ms=%u\n",PAINT_LABEL,count,W,H,paint,ticks()-start);
    win_destroy(win);return 0;
}
