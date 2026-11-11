/* Exercise the real console against a RAM framebuffer without privileged I/O. */
#define HOST_TEST
#include "../kernel/types.h"
#define outb(port,value) ((void)(port),(void)(value))
#define inb(port) ((void)(port),0)
#include "../kernel/vga.c"

extern int printf(const char *,...);
extern void exit(int);
static u32 pixels[80*64];
static unsigned glyphs,underlines,scrolls;
#define CHECK(condition) do { if (!(condition)) { printf("console check failed at %d: %s\n",__LINE__,#condition);exit(1); } } while(0)

static u32 glyph_pixel(u8 ch,int x,int y,u32 fg,u32 bg) {
    return ch!=' ' && ((ch>>(x%8))^(y&1))&1 ? fg:bg;
}
void fb_get_info(struct fb_info *fi) { memset(fi,0,sizeof(*fi));fi->width=80;fi->height=64; }
void fb_glyph(int x,int y,u8 ch,u32 fg,u32 bg) {
    glyphs++;
    CHECK(x>=0 && x<10 && y>=0 && y<4);
    for(int j=0;j<16;j++)for(int i=0;i<8;i++)pixels[(y*16+j)*80+x*8+i]=glyph_pixel(ch,i,j,fg,bg);
}
void fb_fill(int x,int y,int w,int h,u32 color) {
    CHECK(x>=0 && y>=0 && x+w<=80 && y+h<=64);
    if(w==8 && h==2)underlines++;
    for(int j=y;j<y+h;j++)for(int i=x;i<x+w;i++)pixels[j*80+i]=color;
}
void fb_scroll(int n,u32 bg) {
    scrolls++;
    memmove(pixels,pixels+n*80,(64-n)*80*4);
    fb_fill(0,64-n,80,n,bg);
}
static void verify(void) {
    CHECK(row>=0 && row<H && col>=0 && col<W);
    for(int y=0;y<64;y++)for(int x=0;x<80;x++) {
        u16 c=cells[(y/16)*W+x/8];u8 a=c>>8;
        u32 expected=glyph_pixel(c&255,x%8,y%16,palette[a&15],palette[a>>4]);
        if(cur_x==x/8 && cur_y==y/16 && y%16>=13 && y%16<15)
            expected=palette[(a&15)?(a&15):7];
        CHECK(pixels[y*80+x]==expected);
    }
}
static void setup(bool firmware) {
    cells=shadow;W=80;H=25;fbmode=false;suspended=false;cursor_defer=0;
    col=row=0;cur_x=cur_y=-1;cur_on=true;attr=7;esc_state=0;
    vga_init();vga_save_text();vga_switch_to_fb();
    firmware_console=firmware;glyphs=underlines=scrolls=0;
    verify();
}
int main(void) {
    for(int mode=0;mode<2;mode++) {
        setup(mode);
        vga_puts("abcdefgh");verify();CHECK(underlines==1);
        underlines=0;kprintf("%s %u %x","test",123u,42u);verify();CHECK(underlines<=1);
        for(int i=0;i<30;i++) { kprintf("\033[%umline %u\n",31u+i%7,(unsigned)i);verify(); }
        CHECK(mode?scrolls==0:scrolls>0);
        if(mode) {
            unsigned before=glyphs;
            vga_write("one\ntwo\nthree\nfour\nfive\nsix\nseven\neight\n",40);
            verify();
            printf("Buffered console: %u glyph draws for eight lines (screen %u cells)\n",glyphs-before,(unsigned)(W*H));
            CHECK(glyphs-before<=(unsigned)(W*H+1));
        }
        vga_puts("\033[2J\033[H\033[?25l");verify();
        unsigned before=glyphs;
        vga_puts("\033[2J");CHECK(glyphs==before);verify();
        vga_write("ab\0cd",5);verify();CHECK(col==5);
        vga_puts("\033[999999999999999999999999999999999H");verify();
        vga_puts("\033[999999999999999999999999999999999A");verify();
        vga_puts("\033[999999999999999999999999999999999C");verify();
        vga_puts("\033[?25h");verify();
        vga_suspend(true);before=glyphs;
        vga_puts("hidden\ntext\nmore\n");CHECK(glyphs==before);
        vga_suspend(false);verify();
        vga_clear();verify();
    }
    printf("Console output: cursor batching, unchanged cells, scroll pixels, ANSI bounds and suspend/resume: PASS\n");
    return 0;
}
