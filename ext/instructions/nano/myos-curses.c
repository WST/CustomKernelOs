/* ANSI curses subset used by GNU nano. All drawing is retained in RAM;
 * doupdate writes only changed runs. No video memory or privileged I/O. */
#include "config.h"
#include "curses.h"
#include "termios.h"
#include <string.h>
#include <stdarg.h>
WINDOW *stdscr,*curscr;int LINES,COLS;
static struct nano_cell *scene,*shown;
static struct termios saved;
static bool ended=true,initialized;
static int cursor_y,cursor_x,cursor_visible=1,half_timeout=-1,pushed=-1;
static char output[8192];static unsigned output_size;
static void emit_flush(void){unsigned done=0;while(done<output_size){int n=write(1,output+done,output_size-done);if(n<=0)break;done+=n;}output_size=0;}
static void emit(const char *s,unsigned n){while(n){unsigned room=sizeof(output)-output_size,k=n<room?n:room;if(!room){emit_flush();continue;}memcpy(output+output_size,s,k);output_size+=k;s+=k;n-=k;}}
static void text(const char *s){emit(s,strlen(s));}
static void position(int y,int x){char seq[48];int n=snprintf(seq,sizeof(seq),"\033[%d;%dH",y+1,x+1);emit(seq,n);}
static void dimensions(int *h,int *w){int value=ttyctl(0,TTYCTL_GETSIZE,0);*w=value<0?term_cols():(value>>16)&65535;*h=value<0?term_rows():value&65535;if(!*w)*w=80;if(!*h)*h=24;if(*w<2)*w=2;if(*h<2)*h=2;if(*w>1000)*w=1000;if(*h>1000)*h=1000;}
static void cell_set(struct nano_cell *cell,unsigned ch,unsigned attr){cell->ch=ch;cell->attr=attr;}
static void blank(struct nano_cell *cells,int count,unsigned attr){for(int i=0;i<count;i++){cells[i].ch=' ';cells[i].attr=attr;}}
static int screen_size(void){int h,w;dimensions(&h,&w);if(scene&&h==LINES&&w==COLS)return 0;struct nano_cell *next=malloc(h*w*sizeof(*next)),*old=malloc(h*w*sizeof(*old));if(!next||!old){free(next);free(old);return ERR;}free(scene);free(shown);scene=next;shown=old;LINES=h;COLS=w;blank(scene,h*w,0);memset(shown,255,h*w*sizeof(*shown));if(stdscr){free(stdscr->cells);stdscr->cells=calloc(h*w,sizeof(*scene));stdscr->h=h;stdscr->w=w;stdscr->cx=stdscr->cy=0;if(!stdscr->cells)return ERR;blank(stdscr->cells,h*w,0);}return 0;}
WINDOW *newwin(int h,int w,int y,int x){if(!h)h=LINES-y;if(!w)w=COLS-x;if(h<1||w<1||h>1000||w>1000)return NULL;WINDOW *win=calloc(1,sizeof(*win));if(!win)return NULL;win->cells=malloc(h*w*sizeof(*win->cells));if(!win->cells){free(win);return NULL;}win->h=h;win->w=w;win->y=y;win->x=x;win->delay=-1;blank(win->cells,h*w,0);return win;}
WINDOW *initscr(void){if(!initialized){if(nano_tcgetattr(0,&saved)<0)return NULL;initialized=true;}if(screen_size()<0)return NULL;if(!stdscr)stdscr=newwin(LINES,COLS,0,0);curscr=stdscr;if(!stdscr)return NULL;ended=false;text("\033[?1049h\033[2J\033[H");emit_flush();return stdscr;}
int endwin(void){if(!initialized)return OK;if(!ended){text("\033[0m\033[?25h\033[?1049l");emit_flush();nano_terminal_restore(&saved);ended=true;}return OK;}
int isendwin(void){return ended;}
int delwin(WINDOW *w){if(!w||w==stdscr)return ERR;free(w->cells);free(w);return OK;}
int wmove(WINDOW *w,int y,int x){if(!w||y<0||x<0||y>=w->h||x>=w->w)return ERR;w->cy=y;w->cx=x;return OK;}
int waddch(WINDOW *w,chtype value){if(!w)return ERR;unsigned c=value&A_CHARTEXT,attr=w->attr|(value&~A_CHARTEXT);if(c=='\r'){w->cx=0;return OK;}if(c=='\n'){wclrtoeol(w);w->cx=0;if(w->cy<w->h-1)w->cy++;return OK;}if(c=='\t'){int n=8-w->cx%8;while(n--)waddch(w,' ');return OK;}if(w->cx>=w->w){w->cx=0;w->cy++;}if(w->cy>=w->h){w->cy=w->h-1;w->cx=w->w-1;return ERR;}cell_set(&w->cells[w->cy*w->w+w->cx],c,attr);int width=nano_wcwidth(c);if(width<1)width=1;w->cx++;if(width==2&&w->cx<w->w)cell_set(&w->cells[w->cy*w->w+w->cx++],0,attr);return OK;}
int waddnstr(WINDOW *w,const char *s,int n){if(!s)return ERR;if(n<0)n=strlen(s);for(int off=0;off<n&&s[off];){wchar_t c;int k=nano_mbrtowc(&c,s+off,n-off,NULL);if(k<=0){c='?';k=1;}waddch(w,c);off+=k;}return OK;}
int waddstr(WINDOW *w,const char *s){return waddnstr(w,s,-1);}
int mvwaddch(WINDOW *w,int y,int x,chtype c){return wmove(w,y,x)<0?ERR:waddch(w,c);}
int mvwaddnstr(WINDOW *w,int y,int x,const char *s,int n){return wmove(w,y,x)<0?ERR:waddnstr(w,s,n);}
int mvwaddstr(WINDOW *w,int y,int x,const char *s){return mvwaddnstr(w,y,x,s,-1);}
static int formatted(WINDOW *w,const char *fmt,va_list ap){char buf[4096];vsnprintf(buf,sizeof(buf),fmt,ap);return waddstr(w,buf);}
int wprintw(WINDOW *w,const char *fmt,...){va_list ap;va_start(ap,fmt);int r=formatted(w,fmt,ap);va_end(ap);return r;}
int mvwprintw(WINDOW *w,int y,int x,const char *fmt,...){if(wmove(w,y,x)<0)return ERR;va_list ap;va_start(ap,fmt);int r=formatted(w,fmt,ap);va_end(ap);return r;}
int wnoutrefresh(WINDOW *w){if(!w||!scene)return ERR;for(int y=0;y<w->h;y++){int row=w->y+y;if(row<0||row>=LINES)continue;for(int x=0;x<w->w;x++){int col=w->x+x;if(col>=0&&col<COLS)scene[row*COLS+col]=w->cells[y*w->w+x];}}if(!w->leave){cursor_y=w->y+w->cy;cursor_x=w->x+w->cx;if(cursor_x>=COLS)cursor_x=COLS-1;}return OK;}
int doupdate(void){if(ended){if(screen_size()<0)return ERR;ended=false;text("\033[?1049h\033[2J");raw();}unsigned attr=~0u;for(int y=0;y<LINES;y++){int start=0;while(start<COLS){while(start<COLS&&!memcmp(scene+y*COLS+start,shown+y*COLS+start,sizeof(*scene)))start++;if(start==COLS)break;int end=COLS-1;while(end>start&&!memcmp(scene+y*COLS+end,shown+y*COLS+end,sizeof(*scene)))end--;text("\033[?25l");position(y,start);for(int x=start;x<=end;x++){struct nano_cell cell=scene[y*COLS+x];if(attr!=cell.attr){text("\033[0m");if(cell.attr&A_REVERSE)text("\033[7m");if(cell.attr&A_BOLD)text("\033[1m");if(cell.attr&A_ITALIC)text("\033[3m");attr=cell.attr;}if(cell.ch){char bytes[4];int n=nano_wctomb(bytes,cell.ch);if(n<1){bytes[0]='?';n=1;}emit(bytes,n);}shown[y*COLS+x]=cell;}start=end+1;}}text("\033[0m");position(cursor_y<LINES?cursor_y:LINES-1,cursor_x<COLS?cursor_x:COLS-1);text(cursor_visible?"\033[?25h":"\033[?25l");emit_flush();return OK;}
int wrefresh(WINDOW *w){if(w==curscr){if(ended&&screen_size()<0)return ERR;memset(shown,255,LINES*COLS*sizeof(*shown));}else wnoutrefresh(w);return doupdate();}
int refresh(void){return wrefresh(stdscr);}
int wclrtoeol(WINDOW *w){if(!w||w->cy<0||w->cy>=w->h)return ERR;for(int x=w->cx;x<w->w;x++)if(x>=0)cell_set(&w->cells[w->cy*w->w+x],' ',w->attr);return OK;}
int werase(WINDOW *w){if(!w)return ERR;blank(w->cells,w->h*w->w,w->attr);w->cx=w->cy=0;return OK;}
int wattroff(WINDOW *w,int attr){w->attr&=~attr;return OK;}
int wattron(WINDOW *w,int attr){w->attr|=attr;return OK;}
int wattrset(WINDOW *w,int attr){w->attr=attr;return OK;}
int wredrawln(WINDOW *w,int first,int count){for(int y=first;y<first+count;y++)if(y+w->y>=0&&y+w->y<LINES)memset(shown+(y+w->y)*COLS,255,COLS*sizeof(*shown));return OK;}
int wscrl(WINDOW *w,int lines){if(!w)return ERR;if(lines>=w->h||lines<=-w->h)return werase(w);if(lines>0){memmove(w->cells,w->cells+lines*w->w,(w->h-lines)*w->w*sizeof(*scene));blank(w->cells+(w->h-lines)*w->w,lines*w->w,w->attr);}else if(lines<0){lines=-lines;memmove(w->cells+lines*w->w,w->cells,(w->h-lines)*w->w*sizeof(*scene));blank(w->cells,lines*w->w,w->attr);}return OK;}
int raw(void){half_timeout=-1;struct termios mode;nano_tcgetattr(0,&mode);mode.c_lflag=0;return nano_tcsetattr(0,0,&mode);}
int cbreak(void){return raw();}int noecho(void){return raw();}int nonl(void){return OK;}
int keypad(WINDOW *w,int on){w->keys=on;return OK;}int meta(WINDOW *w,int on){(void)w;(void)on;return OK;}
int nodelay(WINDOW *w,int on){w->delay=on?0:-1;return OK;}void wtimeout(WINDOW *w,int ms){w->delay=ms;}
int halfdelay(int tenths){half_timeout=tenths*100;return OK;}
int wgetch(WINDOW *w){if(pushed>=0){int c=pushed;pushed=-1;return c;}unsigned start=ticks();int delay=w->delay>=0?w->delay:half_timeout;for(;;){int h,cols;dimensions(&h,&cols);if(h!=LINES||cols!=COLS){nano_signal_resize();return ERR;}unsigned char c;int n=read(0,&c,1);if(n==1)return c;if(n<0&&errno!=EAGAIN&&errno!=EINTR)return ERR;if(delay==0 || (delay>0&&ticks()-start>=(unsigned)delay))return ERR;sleep_ms(5);}}
int ungetch(int c){pushed=c;return OK;}int napms(int ms){if(ms>0)sleep_ms(ms);return OK;}
int curs_set(int value){int old=cursor_visible;cursor_visible=value!=0;if(initialized&&!ended){text(cursor_visible?"\033[?25h":"\033[?25l");emit_flush();}return old;}
int beep(void){text("\a");emit_flush();return OK;}
int leaveok(WINDOW *w,int on){w->leave=on;return OK;}
int clearok(WINDOW *w,int on){(void)w;if(on)memset(shown,255,LINES*COLS*sizeof(*shown));return OK;}
int typeahead(int fd){(void)fd;return OK;}int intrflush(WINDOW *w,int on){(void)w;(void)on;return OK;}
int scrollok(WINDOW *w,int on){(void)w;(void)on;return OK;}int idlok(WINDOW *w,int on){(void)w;(void)on;return OK;}
int notimeout(WINDOW *w,int on){(void)w;(void)on;return OK;}
int has_colors(void){return FALSE;}int start_color(void){return ERR;}int init_pair(short a,short b,short c){(void)a;(void)b;(void)c;return ERR;}int use_default_colors(void){return OK;}
char *tigetstr(const char *name){(void)name;return NULL;}int key_defined(const char *s){(void)s;return 0;}int define_key(const char *s,int n){(void)s;(void)n;return OK;}
int use_extended_names(int yes){return yes;}int set_escdelay(int ms){(void)ms;return OK;}
