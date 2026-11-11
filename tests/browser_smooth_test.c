/* Exercise the actual browser scroll state against native layout/painting. */
#define BROWSER_HOST_MAIN browser_host_program_main
#include "browser_host.c"
static void require_smooth(int ok,const char *what) {
    if(!ok) { printf("SMOOTH SCROLL FAIL %s\n",what);exit(1); }
}
static void advance_scroll(unsigned elapsed) {
    scroll_started=ticks()-elapsed;scroll_deadline=ticks();smooth_scroll_tick();
}
int main(void) {
    char *args[]={"browser-host","tests/web/smooth.html",NULL};
    require_smooth(browser_host_program_main(2,args)==0,"load fixture");
    require_smooth(doc_h>VIEW_H+300,"scrollable page");
    scroll=0;smooth_scroll_by(48);
    require_smooth(scroll==0 && scroll_animating,"wheel starts without a jump");
    int previous=0;
    for(unsigned t=20;t<=160;t+=20) {
        advance_scroll(t);
        require_smooth(scroll>=previous && scroll<=48,"monotonic bounded interpolation");previous=scroll;
    }
    require_smooth(scroll==48 && !scroll_animating,"exact endpoint");
    smooth_scroll_by(48);advance_scroll(40);smooth_scroll_by(48);
    require_smooth(scroll_target==144,"coalesced wheel distance");
    advance_scroll(200);require_smooth(scroll==144,"retarget endpoint");
    smooth_scroll_by(doc_h);advance_scroll(200);
    require_smooth(scroll==doc_h-VIEW_H,"bottom clamp");
    smooth_scroll_by(-doc_h);advance_scroll(200);
    require_smooth(!scroll,"top clamp");
    smooth_scroll_by(96);smooth_scroll_by(-96);
    require_smooth(!scroll_animating && !scroll,"reverse input cancels pending motion");
    smooth_scroll_by(48);scroll_by(16);
    require_smooth(!scroll_animating && scroll==16,"direct offsets remain immediate");
    doc_h=VIEW_H-1;scroll=0;smooth_scroll_by(48);
    require_smooth(!scroll_animating && !scroll,"short page stays still");
    puts("SMOOTH SCROLL PASS");return 0;
}
