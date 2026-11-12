/* Run the production overlapping drag copy against a snapshot reference.
 * The display stub observes uploads; no GPU/framebuffer reads are available. */
#define gfx_flip_rect observed_flip
#define main desktop_main
#include "../user/desktop.c"
#undef main
#undef gfx_flip_rect
static color_t *shown;
static int uploads;
unsigned ticks(void){return 100;}
void observed_flip(int x,int y,int w,int h) {
    uploads++;
    for(int row=y;row<y+h;row++)memcpy(shown+row*SW+x,back+row*SW+x,w*4);
}
static void check(bool ok,const char *message){if(!ok){printf("FAIL: %s\n",message);exit(1);}}
int main(void) {
    SW=73;SH=59;unsigned bytes=SW*SH*4;
    back=malloc(bytes);shown=malloc(bytes);color_t *before=malloc(bytes),*expected=malloc(bytes);
    check(back && shown && before && expected,"allocation");
    unsigned random=12345;
    for(int iteration=0;iteration<1000;iteration++) {
        for(int i=0;i<SW*SH;i++)before[i]=0xff000000u+(unsigned)i;
        memcpy(back,before,bytes);memcpy(shown,before,bytes);memcpy(expected,before,bytes);
        random=random*1664525+1013904223;int w=1+random%SW;
        random=random*1664525+1013904223;int h=1+random%SH;
        random=random*1664525+1013904223;int sx=random%(SW-w+1);
        random=random*1664525+1013904223;int sy=random%(SH-h+1);
        random=random*1664525+1013904223;int dx=random%(SW-w+1);
        random=random*1664525+1013904223;int dy=random%(SH-h+1);
        for(int j=0;j<h;j++)memcpy(expected+(dy+j)*SW+dx,before+(sy+j)*SW+sx,w*4);
        uploads=0;copy_window_pixels(mkrect(dx,dy,w,h),dx-sx,dy-sy);
        check(!memcmp(back,expected,bytes),"RAM overlap copy");
        check(!memcmp(shown,expected,bytes) && uploads==1,"single coherent upload");
    }
    check(motion_wait()==10,"idle input polling interval");
    wins[0].used=true;wins[0].motion=true;motion_deadline=200;
    check(motion_wait()==10,"animation must not delay input");
    motion_deadline=105;check(motion_wait()==5,"earlier animation deadline");
    motion_deadline=99;check(motion_wait()==0,"expired animation deadline");
    puts("PASS: 1000 overlapping RAM drag copies, coherent uploads and <=10ms input polling");
    free(expected);free(before);free(shown);free(back);return 0;
}
