#ifndef MYOS_NANO_CURSES_H
#define MYOS_NANO_CURSES_H
#include <myos.h>
#include <stdio.h>
typedef unsigned chtype;typedef unsigned attr_t;
struct nano_cell {unsigned ch,attr;};
typedef struct nano_window {int h,w,y,x,cy,cx,delay;bool keys,leave;unsigned attr;struct nano_cell *cells;} WINDOW;
extern WINDOW *stdscr,*curscr;extern int LINES,COLS;
#define TRUE 1
#define FALSE 0
#define ERR (-1)
#define OK 0
#define A_NORMAL 0
#define A_REVERSE 0x1000000
#define A_BOLD 0x2000000
#define A_ITALIC 0x4000000
#define A_UNDERLINE 0x8000000
#define A_CHARTEXT 0x1fffff
#define COLOR_PAIR(n) 0
#define COLOR_BLACK 0
#define COLOR_RED 1
#define COLOR_GREEN 2
#define COLOR_YELLOW 3
#define COLOR_BLUE 4
#define COLOR_MAGENTA 5
#define COLOR_CYAN 6
#define COLOR_WHITE 7
#define KEY_F0 264
#define KEY_F(n) (KEY_F0+(n))
#define KEY_DOWN 258
#define KEY_UP 259
#define KEY_LEFT 260
#define KEY_RIGHT 261
#define KEY_HOME 262
#define KEY_BACKSPACE 263
#define KEY_DC 330
#define KEY_IC 331
#define KEY_NPAGE 338
#define KEY_PPAGE 339
#define KEY_ENTER 343
#define KEY_END 360
#define KEY_BTAB 353
#define KEY_RESIZE 410
#define KEY_BEG 354
#define KEY_CANCEL 355
#define KEY_CLOSE 356
#define KEY_EOL 335
#define KEY_A1 348
#define KEY_A3 349
#define KEY_B2 350
#define KEY_C1 351
#define KEY_C3 352
#define KEY_SBEG 378
#define KEY_SCANCEL 379
#define KEY_SDC 383
#define KEY_SEND 386
#define KEY_SHOME 391
#define KEY_SIC 392
#define KEY_SLEFT 393
#define KEY_SNEXT 396
#define KEY_SPREVIOUS 398
#define KEY_SRIGHT 402
#define KEY_SSUSPEND 405
#define KEY_SUSPEND 407
#define KEY_SF 336
#define KEY_SR 337
WINDOW *initscr(void);WINDOW *newwin(int,int,int,int);int delwin(WINDOW*);int endwin(void);int isendwin(void);
int wmove(WINDOW*,int,int);int waddch(WINDOW*,chtype);int waddnstr(WINDOW*,const char*,int);int waddstr(WINDOW*,const char*);
int mvwaddch(WINDOW*,int,int,chtype);int mvwaddnstr(WINDOW*,int,int,const char*,int);int mvwaddstr(WINDOW*,int,int,const char*);
int wprintw(WINDOW*,const char*,...);int mvwprintw(WINDOW*,int,int,const char*,...);
int wnoutrefresh(WINDOW*);int doupdate(void);int wrefresh(WINDOW*);int refresh(void);int wclrtoeol(WINDOW*);int werase(WINDOW*);
int wattroff(WINDOW*,int);int wattron(WINDOW*,int);int wattrset(WINDOW*,int);int wredrawln(WINDOW*,int,int);int wscrl(WINDOW*,int);
int beep(void);int curs_set(int);int raw(void);int cbreak(void);int noecho(void);int nonl(void);int keypad(WINDOW*,int);int meta(WINDOW*,int);
int nodelay(WINDOW*,int);void wtimeout(WINDOW*,int);int halfdelay(int);int wgetch(WINDOW*);int ungetch(int);int napms(int);
int typeahead(int);int intrflush(WINDOW*,int);int use_default_colors(void);int has_colors(void);int start_color(void);int init_pair(short,short,short);
int leaveok(WINDOW*,int);int scrollok(WINDOW*,int);int idlok(WINDOW*,int);int notimeout(WINDOW*,int);int clearok(WINDOW*,int);
char *tigetstr(const char*);int key_defined(const char*);int define_key(const char*,int);int use_extended_names(int);int set_escdelay(int);
#define getmaxyx(win,y,x) ((y)=(win)->h,(x)=(win)->w)
#define getyx(win,y,x) ((y)=(win)->cy,(x)=(win)->cx)
#define getbegyx(win,yy,xx) ((yy)=(win)->y,(xx)=(win)->x)
#define getmaxy(win) ((win)->h)
#define getmaxx(win) ((win)->w)
#endif
