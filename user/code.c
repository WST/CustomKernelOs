/* notepad [файл] — текстовый редактор в окне.
 *
 *   стрелки, Home/End, PgUp/PgDn — двигаться; мышь — поставить курсор
 *   Ctrl+S — сохранить, Ctrl+O — открыть (имя в поле сверху), Ctrl+N — новый
 *   Tab — 4 пробела
 * Текст — UTF-8 (русские буквы с клавиатуры); курсор и столбцы — в буквах.
 * Окно можно растягивать и разворачивать. */
#include "win.h"

static int W = 1100, H = 680;
#define SIDE_W 228
#define BAR_H 90
#define STATUS_H 22
#define TEXT_X (SIDE_W + 54)
#define TEXT_Y (BAR_H + 4)
#define VIS_ROWS ((H - BAR_H - STATUS_H - 8) / 16)
#define VIS_COLS ((W - TEXT_X - 18) / 8)
#define MAX_LINES 4000

static window *win;
static char *lines[MAX_LINES];
static int   lens[MAX_LINES], caps[MAX_LINES];
static int   n_lines;
static int   cur_r, cur_c, top, left;
static bool  modified;
static char  path[96];
static bool  editing_name;                  /* курсор в поле имени файла */
static bool opening_name;
static char previous_path[96];
static int   name_len;
static char  status_msg[192];
static int   pressed_btn = -1;
static int anchor_r, anchor_c;
static int discard_key;
static bool selecting;
static void changed(void);

/* ------------------------------------------------------------ текст */

/* UTF-8: байт-продолжение? */
static bool cont(char c) { return ((unsigned char)c & 0xC0) == 0x80; }
static int col_of(int li, int byte) {                   /* байт -> столбец (буква) */
    int k = 0;
    for (int i = 0; i < byte && i < lens[li]; i++) if (!cont(lines[li][i])) k++;
    return k;
}
static int byte_of(int li, int col) {                   /* столбец -> байт */
    int i = 0;
    while (i < lens[li] && col > 0) { i++; while (i < lens[li] && cont(lines[li][i])) i++; col--; }
    return i;
}
static int u8_chars(const char *s) { int k = 0; for (; *s; s++) if (!cont(*s)) k++; return k; }

static void ensure_cap(int r, int need) {
    if (caps[r] >= need) return;
    int cap = caps[r] ? caps[r] : 16;
    while (cap < need) cap *= 2;
    char *p = malloc(cap);
    if (lines[r]) memcpy(p, lines[r], lens[r]);
    free(lines[r]);
    lines[r] = p;
    caps[r] = cap;
}

static void insert_line(int at, const char *s, int n) {
    if (n_lines >= MAX_LINES) return;
    for (int i = n_lines; i > at; i--) { lines[i] = lines[i - 1]; lens[i] = lens[i - 1]; caps[i] = caps[i - 1]; }
    lines[at] = NULL; lens[at] = 0; caps[at] = 0;
    ensure_cap(at, n + 1);
    memcpy(lines[at], s, n);
    lens[at] = n;
    n_lines++;
}

static void delete_line(int at) {
    free(lines[at]);
    for (int i = at; i < n_lines - 1; i++) { lines[i] = lines[i + 1]; lens[i] = lens[i + 1]; caps[i] = caps[i + 1]; }
    n_lines--;
    lines[n_lines] = NULL; lens[n_lines] = 0; caps[n_lines] = 0;
}

static void clear_text(void) {
    while (n_lines) delete_line(n_lines - 1);
    insert_line(0, "", 0);
    cur_r = cur_c = top = left = 0;
    anchor_r = anchor_c = 0;
    selecting = false;
}

static void set_title(void) {
    char t[64];
    const char *b = path[0] ? strrchr(path, '/') : NULL;
    snprintf(t, sizeof(t), "%s%s - Code", modified ? "*" : "", path[0] ? (b ? b + 1 : path) : "Untitled");
    win_set_title(win, t);
}

static bool load(const char *p) {
    int fd = open(p, O_READ);
    if (fd < 0) { snprintf(status_msg, sizeof(status_msg), "cannot open %s", p); return false; }
    int capacity=4096,total=0,n;
    char *data=malloc(capacity);
    if(!data) {close(fd);strcpy(status_msg,"Out of memory");return false;}
    bool valid=true;
    while((n=read(fd,data+total,capacity-total))>0) {
        total+=n;
        if(total==capacity) {
            if(capacity>=2*1024*1024) {valid=false;break;}
            char *next=malloc(capacity*2);
            if(!next) {valid=false;break;}
            memcpy(next,data,total);free(data);data=next;capacity*=2;
        }
    }
    close(fd);
    if(n<0) valid=false;
    int count=1;
    for(int i=0;i<total;i++) {if(!data[i]) valid=false;if(data[i]=='\n') count++;}
    if(count>MAX_LINES) valid=false;
    if(!valid) {free(data);strcpy(status_msg,"Cannot load: binary, read error, or document exceeds limits");return false;}
    char **fresh=malloc(count*sizeof(char*));
    int *sizes=malloc(count*sizeof(int)),allocated=0;
    if(!fresh || !sizes) valid=false;
    for(int start=0,r=0;valid && r<count;r++) {
        int end=start,expanded=0;
        while(end<total && data[end]!='\n') {expanded+=data[end]=='\t'?4:data[end]!='\r';end++;}
        fresh[r]=malloc(expanded+1);if(!fresh[r]) {valid=false;break;}
        allocated++;int out=0;
        for(int b=start;b<end;b++) {
            if(data[b]=='\t') {for(int k=0;k<4;k++) fresh[r][out++]=' ';}
            else if(data[b]!='\r') fresh[r][out++]=data[b];
        }
        fresh[r][out]=0;sizes[r]=out;start=end+1;
    }
    free(data);
    if(!valid) {
        for(int r=0;r<allocated;r++) free(fresh[r]);
        free(fresh);free(sizes);
        strcpy(status_msg,"Out of memory; current document kept");return false;
    }
    while(n_lines) delete_line(n_lines-1);
    n_lines=count;
    for(int r=0;r<count;r++) {lines[r]=fresh[r];lens[r]=sizes[r];caps[r]=sizes[r]+1;}
    free(fresh);free(sizes);
    cur_r = cur_c = top = left = 0;
    anchor_r = anchor_c = 0;
    selecting = false;
    modified = false;
    snprintf(status_msg, sizeof(status_msg), "opened, %d lines", n_lines);
    return true;
}

static void save(void) {
    if (!path[0]) { editing_name = true; opening_name=false; name_len=0; strcpy(status_msg, "type a file name, then Enter"); return; }
    int fd = open(path, O_WRITE);
    if (fd < 0) { snprintf(status_msg, sizeof(status_msg), "cannot save: %s", strerror(errno)); return; }
    int total = 0;
    for (int i = 0; i < n_lines; i++) {
        int written=0;
        while(written<lens[i]) {
            int n=write(fd,lines[i]+written,lens[i]-written);
            if(n<=0) {close(fd);strcpy(status_msg,"Save failed: incomplete write");return;}written+=n;
        }
        if ((i < n_lines - 1 || lens[i]) && write(fd, "\n", 1)!=1) {
            close(fd);strcpy(status_msg,"Save failed: incomplete write");return;
        }
        total += lens[i] + 1;
    }
    close(fd);
    modified = false;
    snprintf(status_msg, sizeof(status_msg), "saved %d bytes", total);
}

/* ------------------------------------------------------------ рисование */

static bool has_selection(void) { return anchor_r != cur_r || anchor_c != cur_c; }
static void selection_bounds(int *r0, int *c0, int *r1, int *c1) {
    bool forward = anchor_r < cur_r || (anchor_r == cur_r && anchor_c <= cur_c);
    *r0 = forward ? anchor_r : cur_r; *c0 = forward ? anchor_c : cur_c;
    *r1 = forward ? cur_r : anchor_r; *c1 = forward ? cur_c : anchor_c;
}
static void drop_selection(void) { anchor_r = cur_r; anchor_c = cur_c; }
static void erase_selection(void) {
    if (!has_selection()) return;
    int r0, c0, r1, c1;
    selection_bounds(&r0, &c0, &r1, &c1);
    if (r0 == r1) {
        memmove(lines[r0] + c0, lines[r0] + c1, lens[r0] - c1);
        lens[r0] -= c1 - c0;
    } else {
        int tail = lens[r1] - c1;
        ensure_cap(r0, c0 + tail + 1);
        memcpy(lines[r0] + c0, lines[r1] + c1, tail);
        lens[r0] = c0 + tail;
        for (int r = r1; r > r0; r--) delete_line(r);
    }
    cur_r = r0; cur_c = c0; drop_selection(); changed();
}
static void copy_selection(bool cut) {
    if (!has_selection()) return;
    int r0, c0, r1, c1;
    selection_bounds(&r0, &c0, &r1, &c1);
    int length = 0;
    for (int r = r0; r <= r1; r++)
        length += (r == r1 ? c1 : lens[r]) - (r == r0 ? c0 : 0) + (r < r1);
    if (length > CLIPBOARD_MAX) { strcpy(status_msg, "selection exceeds 64 KiB"); return; }
    char *s = malloc(length + 1);
    if (!s) { strcpy(status_msg, "out of memory"); return; }
    int n = 0;
    for (int r = r0; r <= r1; r++) {
        int start = r == r0 ? c0 : 0, end = r == r1 ? c1 : lens[r];
        memcpy(s + n, lines[r] + start, end - start); n += end - start;
        if (r < r1) s[n++] = '\n';
    }
    int result = clipboard_set(s, n);
    free(s);
    if (result < 0) { strcpy(status_msg, "clipboard copy failed"); return; }
    if (cut) erase_selection();
}

/* Decode before editing: reject unsupported/malformed UTF-8 without losing
 * the selected text. This editor's key representation supports BMP text. */
static int paste_code(const char *s, int length, int *offset) {
    unsigned c = (unsigned char)s[(*offset)++];
    if (c < 128) return c;
    int count = c >= 0xC2 && c <= 0xDF ? 1 : c >= 0xE0 && c <= 0xEF ? 2 : -1;
    if (count < 0 || *offset + count > length) return -1;
    unsigned cp = c & (count == 1 ? 31 : 15);
    for (int i = 0; i < count; i++) {
        c = (unsigned char)s[(*offset)++];
        if ((c & 0xC0) != 0x80) return -1;
        cp = (cp << 6) | (c & 63);
    }
    if ((count == 2 && cp < 0x800) || (cp >= 0xD800 && cp <= 0xDFFF)) return -1;
    return cp;
}
static bool literal_paste;
static void text_key(int k);
static void paste_text(void) {
    char *s = malloc(CLIPBOARD_MAX);
    if (!s) return;
    int length = clipboard_get(s, CLIPBOARD_MAX), extra = 0;
    if (length <= 0) { free(s); return; }
    for (int i = 0; i < length;) {
        int cp = paste_code(s, length, &i);
        if (cp < 0) { strcpy(status_msg, "unsupported clipboard encoding"); free(s); return; }
        if (cp == '\n' || (cp == '\r' && (i == length || s[i] != '\n'))) extra++;
    }
    int r0, c0, r1, c1;
    selection_bounds(&r0, &c0, &r1, &c1);
    if (n_lines - (r1 - r0) + extra > MAX_LINES) {
        strcpy(status_msg, "too many lines to paste"); free(s); return;
    }
    erase_selection();
    literal_paste = true;
    for (int i = 0; i < length;) {
        int cp = paste_code(s, length, &i);
        if (cp == '\r') { if (i < length && s[i] == '\n') continue; cp = '\n'; }
        if (cp == '\n' || cp == '\t') text_key(cp);
        else if (cp >= 32 && cp != 127) {
            char u[4]; int n = utf8_put(u, cp);
            ensure_cap(cur_r, lens[cur_r] + n + 1);
            memmove(lines[cur_r] + cur_c + n, lines[cur_r] + cur_c, lens[cur_r] - cur_c);
            memcpy(lines[cur_r] + cur_c, u, n);
            cur_c += n; lens[cur_r] += n; changed(); drop_selection();
        }
    }
    literal_paste = false;
    free(s);
}

#include "code_features.h"
#include "code_workspace.h"
#include "code_chrome.h"

static void draw(void) {
    chrome_draw();

    int th = H - BAR_H - STATUS_H;
    gfx_fill_rect(SIDE_W, BAR_H, W-SIDE_W, th, CODE_BG);
    tree_draw();
    int block = 0;
    for (int r = 0; r < top; r++) syntax_line(r, &block, false, 0);
    gfx_fill_rect(SIDE_W, BAR_H, TEXT_X - SIDE_W - 6, th, CODE_BG);
    for (int r = 0; r < VIS_ROWS && top + r < n_lines; r++) {
        int li = top + r;
        if(li==cur_r && !editing_name) gfx_fill_rect(SIDE_W,TEXT_Y+r*16,W-SIDE_W-12,16,RGB(39,39,39));
        if (has_selection() && !editing_name) {
            int r0, c0, r1, c1;
            selection_bounds(&r0, &c0, &r1, &c1);
            if (li >= r0 && li <= r1) {
                int start = li == r0 ? col_of(li, c0) : 0;
                int end = li == r1 ? col_of(li, c1) : col_of(li, lens[li]) + 1;
                if (start < left) start = left;
                if (end > left + VIS_COLS) end = left + VIS_COLS;
                if (end > start) gfx_fill_rect(TEXT_X + (start - left) * 8,
                    TEXT_Y + r * 16, (end - start) * 8, 16, UI_SELECT);
            }
        }
        char number[12]; snprintf(number, sizeof(number), "%4d", li + 1);
        gfx_text(SIDE_W+8, TEXT_Y + r * 16, number, RGB(133,133,133), TRANSPARENT);
        syntax_line(li, &block, true, TEXT_Y + r * 16);
    }
    int ccol = col_of(cur_r, cur_c);
    if (!editing_name && cur_r >= top && cur_r < top + VIS_ROWS && ccol >= left)
        gfx_fill_rect(TEXT_X + (ccol - left) * 8, TEXT_Y + (cur_r - top) * 16, 2, 16, CODE_FG);
    /* полоса прокрутки */
    int sb_x = W - 12;
    gfx_fill_rect(sb_x, BAR_H, 12, th, RGB(30,30,30));
    if (n_lines > VIS_ROWS) {
        int kh = th * VIS_ROWS / n_lines;
        if (kh < 20) kh = 20;
        int ky = BAR_H + (th - kh) * top / (n_lines - VIS_ROWS);
        gfx_fill_rect(sb_x + 2, ky, 8, kh, RGB(75,75,75));
    }
    gfx_fill_rect(0, H - STATUS_H, W, STATUS_H, RGB(0,122,204));
    gfx_fill_rect(0, H - STATUS_H, W, 1, RGB(0,122,204));
    char st[256];
    snprintf(st, sizeof(st), "%s | Ln %d, Col %d | %d C API | %s", language()==1?"C / JS":language()==2?"Python":"Text", cur_r+1, ccol+1, symbol_count, status_msg);
    gfx_text(8, H - STATUS_H + 3, st, C_WHITE, TRANSPARENT);
    if(!editing_folder) {completion_draw();signature_draw();}
    chrome_overlay();
    win_update(win);
}

static void keep_visible(void) {
    if (cur_r < top) top = cur_r;
    if (cur_r >= top + VIS_ROWS) top = cur_r - VIS_ROWS + 1;
    int col = col_of(cur_r, cur_c);
    if (col < left) left = col;
    if (col >= left + VIS_COLS) left = col - VIS_COLS + 1;
}

static void changed(void) {
    if (!modified) { modified = true; set_title(); }
    status_msg[0] = 0;
}

/* ------------------------------------------------------------ клавиши */

static void name_key(int k) {
    if (k == '\n' || k == '\r') {
        editing_name = false;
        if (path[0]) {
            if(path[0]!='/') {
                char absolute[128];
                if(!join_path(absolute,sizeof(absolute),workspace,path) || strlen(absolute)>=sizeof(path)) {
                    strcpy(status_msg,"File path too long");return;
                }
                strcpy(path,absolute);name_len=strlen(path);
            }
            if(opening_name) {
                if(!load(path)) strcpy(path,previous_path);
            } else save();
            set_title();
        }
    } else if (k == 27) {
        editing_name = false;
        if(opening_name) strcpy(path,previous_path);
    } else if ((k == 8 || k == 127) && name_len) {
        name_len--;
        while (name_len > 0 && cont(path[name_len])) name_len--;
        path[name_len] = 0;
    } else if (((k >= 32 && k < 127) || (k >= 0xA0 && k < 0x10000 && (k < KEY_UP || k > KEY_INS)))
               && name_len < (int)sizeof(path) - 4) {
        name_len += utf8_put(path + name_len, k);
        path[name_len] = 0;
    }
}

static void text_key(int k) {
    if ((k == CTRL('n') || k == CTRL('o')) && modified && discard_key != k) {
        discard_key = k;
        strcpy(status_msg, "unsaved! repeat shortcut to discard, Ctrl+S to save");
        return;
    }
    discard_key = 0;
    if (!literal_paste && paired_key(k)) {signature_hidden=false;completion_hidden=false;return;}
    if (!literal_paste && complete_key(k)) return;
    if (k == CTRL('b')) { build_start(); return; }
    if (k == CTRL('a')) { anchor_r = anchor_c = 0; cur_r = n_lines - 1; cur_c = lens[cur_r]; return; }
    if (k == CTRL('c') || k == KEY_COPY) { copy_selection(false); return; }
    if (k == CTRL('x')) { copy_selection(true); return; }
    if (k == CTRL('v') || k == KEY_PASTE || k == (KEY_INS | KEY_SHIFT)) { paste_text(); return; }
    bool shift = (k & KEY_SHIFT) != 0;
    k &= ~KEY_SHIFT;
    bool movement = k >= KEY_UP && k <= KEY_PGDN;
    if (movement && !shift) drop_selection();
    if (has_selection() && (k == 8 || k == 127 || k == KEY_DEL)) { erase_selection(); return; }
    if (has_selection() && (k == '\n' || k == '\r' || k == '\t' ||
        (k >= 32 && k < 127) || (k >= 0xA0 && k < 0x10000 && (k < KEY_UP || k > KEY_INS)))) erase_selection();
    if (k == CTRL('s')) { save(); set_title(); return; }
    if (k == CTRL('o')) { editing_name = true; opening_name=true; strcpy(previous_path,path); name_len = strlen(path); strcpy(status_msg, "file name, then Enter"); return; }
    if (k == CTRL('n')) { clear_text(); path[0] = 0; modified = false; set_title(); return; }
    int col = col_of(cur_r, cur_c);                       /* вверх-вниз — в тот же столбец (в буквах) */
    switch (k) {
    case KEY_UP:    if (cur_r > 0) cur_r--; cur_c = byte_of(cur_r, col); break;
    case KEY_DOWN:  if (cur_r < n_lines - 1) cur_r++; cur_c = byte_of(cur_r, col); break;
    case KEY_LEFT:
        if (cur_c > 0) { cur_c--; while (cur_c > 0 && cont(lines[cur_r][cur_c])) cur_c--; }
        else if (cur_r > 0) { cur_r--; cur_c = lens[cur_r]; }
        break;
    case KEY_RIGHT:
        if (cur_c < lens[cur_r]) { cur_c++; while (cur_c < lens[cur_r] && cont(lines[cur_r][cur_c])) cur_c++; }
        else if (cur_r < n_lines - 1) { cur_r++; cur_c = 0; }
        break;
    case KEY_HOME: {
        int first=0;while(first<lens[cur_r] && (lines[cur_r][first]==' ' || lines[cur_r][first]=='\t')) first++;
        cur_c=cur_c==first?0:first;break;
    }
    case KEY_END:   cur_c = lens[cur_r]; break;
    case KEY_PGUP:  cur_r -= VIS_ROWS; if (cur_r < 0) cur_r = 0; cur_c = byte_of(cur_r, col); break;
    case KEY_PGDN:  cur_r += VIS_ROWS; if (cur_r >= n_lines) cur_r = n_lines - 1; cur_c = byte_of(cur_r, col); break;
    case '\n': case '\r':
        if (n_lines >= MAX_LINES) { strcpy(status_msg, "line limit reached"); return; }
        int indent = 0;
        if (!literal_paste && language()) {
            while (indent < cur_c && lines[cur_r][indent] == ' ') indent++;
            if (cur_c && (lines[cur_r][cur_c-1] == '{' || lines[cur_r][cur_c-1] == ':')) indent += 4;
        }
        insert_line(cur_r + 1, lines[cur_r] + cur_c, lens[cur_r] - cur_c);
        lens[cur_r] = cur_c;
        cur_r++; cur_c = 0;
        if (indent) {
            ensure_cap(cur_r, lens[cur_r] + indent + 1);
            memmove(lines[cur_r] + indent, lines[cur_r], lens[cur_r]);
            memset(lines[cur_r], ' ', indent); lens[cur_r] += indent; cur_c = indent;
        }
        changed();
        break;
    case 8: case 127:
        if (cur_c > 0) {
            int st = cur_c - 1;
            while (st > 0 && cont(lines[cur_r][st])) st--;           /* буква целиком */
            memmove(lines[cur_r] + st, lines[cur_r] + cur_c, lens[cur_r] - cur_c);
            lens[cur_r] -= cur_c - st; cur_c = st;
        } else if (cur_r > 0) {
            int pl = lens[cur_r - 1];
            ensure_cap(cur_r - 1, pl + lens[cur_r] + 1);
            memcpy(lines[cur_r - 1] + pl, lines[cur_r], lens[cur_r]);
            lens[cur_r - 1] += lens[cur_r];
            delete_line(cur_r);
            cur_r--; cur_c = pl;
        }
        changed();
        break;
    case KEY_DEL:
        if (cur_c < lens[cur_r]) {
            int e = cur_c + 1;
            while (e < lens[cur_r] && cont(lines[cur_r][e])) e++;
            memmove(lines[cur_r] + cur_c, lines[cur_r] + e, lens[cur_r] - e);
            lens[cur_r] -= e - cur_c;
        } else if (cur_r < n_lines - 1) {
            ensure_cap(cur_r, lens[cur_r] + lens[cur_r + 1] + 1);
            memcpy(lines[cur_r] + lens[cur_r], lines[cur_r + 1], lens[cur_r + 1]);
            lens[cur_r] += lens[cur_r + 1];
            delete_line(cur_r + 1);
        }
        changed();
        break;
    case '\t':
        for (int i = 0; i < 4; i++) text_key(' ');
        return;
    default: {
        bool other = k >= 0xA0 && k < 0x10000 && (k < KEY_UP || k > KEY_INS);   /* русская буква и т.п. */
        if ((k < 32 || k >= 127) && !other) return;
        char u[4];
        int n = utf8_put(u, k);
        ensure_cap(cur_r, lens[cur_r] + n + 1);
        memmove(lines[cur_r] + cur_c + n, lines[cur_r] + cur_c, lens[cur_r] - cur_c);
        memcpy(lines[cur_r] + cur_c, u, n);
        cur_c += n;
        lens[cur_r] += n;
        changed();
    }
    }
    if (cur_c > lens[cur_r]) cur_c = lens[cur_r];
    if (!movement || !shift) drop_selection();
}

static void click(int x, int y) {
    if(chrome_click(x,y)) return;
    if (x<SIDE_W && y>=34 && y<H-STATUS_H) {
        if(y<34+28 && x>=170) {tree_top+= x<196?-5:5;return;}
        tree_click(x,y);return;
    }
    if (y >= BAR_H && y < H - STATUS_H && x >= SIDE_W && x < W - 12) {
        editing_folder = false;
        editing_name = false;
        int r = top + (y - TEXT_Y) / 16;
        if (r < 0) r = 0;
        if (r >= n_lines) r = n_lines - 1;
        int c = left + (x - TEXT_X + 4) / 8;
        if (c < 0) c = 0;
        cur_r = r; cur_c = byte_of(r, c);
        drop_selection();
        selecting = true;
    }
}

int main(int argc, char **argv) {
    win = win_create("Code", W, H);
    if (!win) { dprintf(STDERR, "code: the desktop is not running\n"); return 1; }
    win_set_resizable(win, 720, 320);
    clear_text();
    char initial_dir[128];getcwd(initial_dir,sizeof(initial_dir));
    workspace_open(initial_dir);
    index_headers("/include","",0);
    if (argc > 1) {
        struct dirent_info st;
        if(dirstat(argv[1],&st)==0 && st.type==2) workspace_open(argv[1]);
        else {
            char absolute[128];
            bool valid=argv[1][0]=='/' ? (snprintf(absolute,sizeof(absolute),"%s",argv[1])<(int)sizeof(absolute)) : join_path(absolute,sizeof(absolute),initial_dir,argv[1]);
            if(valid && strlen(absolute)<sizeof(path)) {
                strcpy(path,absolute);name_len=strlen(path);
                if(dirstat(path,&st)==0) load(path);else strcpy(status_msg,"new file");
                char *slash=strrchr(absolute,'/');if(slash) {if(slash==absolute) slash[1]=0;else *slash=0;workspace_open(absolute);}
            } else strcpy(status_msg,"Source path is too long");
        }
    }
    set_title();
    draw();
    struct event e;
    bool close_armed = false;
    while (win_event(&e, 150)) {
        int previous_build=build_pid;
        build_poll();
        if(e.type==EV_TIMEOUT && previous_build==build_pid) continue;
        if (e.type == EV_CLOSE) {
            if (!modified || close_armed) break;
            close_armed = true;                                   /* как в Windows: не терять текст молча */
            strcpy(status_msg, "not saved! Ctrl+S to save, X again to discard");
            draw();
            continue;
        }
        if (e.type == EV_KEY) { menu_open=-1; if (editing_folder) folder_key(e.key); else if (editing_name) name_key(e.key); else text_key(e.key); keep_visible(); }
        else if (e.type == EV_MOUSE_WHEEL) {
            if (e.x < SIDE_W && e.y >= 34 && e.y < H - STATUS_H) tree_top -= e.wheel * 3;
            else if (e.y >= BAR_H && e.y < H - STATUS_H) {
                top -= e.wheel * 3;
                if (top > n_lines - VIS_ROWS) top = n_lines - VIS_ROWS;
                if (top < 0) top = 0;
            }
        }
        else if (e.type == EV_MOUSE_DOWN && (e.buttons & MOUSE_LEFT)) { selecting = false; click(e.x, e.y); keep_visible(); }
        else if (e.type == EV_MOUSE_MOVE && selecting) {
            int r = top + (e.y - TEXT_Y) / 16;
            if (r < 0) r = 0;
            if (r >= n_lines) r = n_lines - 1;
            int c = left + (e.x - TEXT_X + 4) / 8;
            if (c < 0) c = 0;
            cur_r = r; cur_c = byte_of(r, c); keep_visible();
        }
        else if (e.type == EV_MOUSE_UP) { pressed_btn = -1; selecting = false; }
        else if (e.type == EV_FOCUS && !e.key) { selecting = false; pressed_btn = -1; }
        else if (e.type == EV_RESIZE) { W = e.x; H = e.y; keep_visible(); }
        else if(e.type!=EV_TIMEOUT) continue;
        draw();
    }
    win_destroy(win);
    return 0;
}
