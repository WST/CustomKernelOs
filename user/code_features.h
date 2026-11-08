/* Private implementation for Code; all offsets into document text are bytes. */
#include "code_index.h"
#define CODE_BG RGB(30,30,30)
#define CODE_FG RGB(212,212,212)
#define CODE_COMMENT RGB(106,153,85)
#define CODE_STRING RGB(206,145,120)
#define CODE_KEYWORD RGB(86,156,214)
#define CODE_TYPE RGB(78,201,176)
#define CODE_FUNCTION RGB(220,220,170)
#define CODE_NUMBER RGB(181,206,168)
#define CODE_DIRECTIVE RGB(197,134,192)
static const char *code_words[] = {
    "auto", "bool", "break", "case", "char", "const", "continue", "default",
    "do", "double", "else", "enum", "extern", "false", "float", "for", "if",
    "int", "long", "NULL", "return", "short", "signed", "sizeof", "static",
    "struct", "switch", "true", "typedef", "union", "unsigned", "void",
    "volatile", "while", "class", "function", "let", "var", "new", "this",
    "import", "export", "async", "await", "def", "elif", "except", "from",
    "None", "pass", "raise", "try", "with", "yield", "printf", "malloc",
    "free", "memcpy", "strlen", "open", "read", "write", "close"
};
static char suggestions[6][64];
static int suggestion_symbol[6];
static int suggestion_count, suggestion_index, prefix_start;
static int completion_delim;
static bool completion_hidden;
static bool ident(int c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '_';
}
static int language(void) {
    const char *ext = strrchr(path, '.');
    if (!ext) return path[0] ? 0 : 1;
    if (!strcmp(ext, ".py")) return 2;
    if (!strcmp(ext, ".c") || !strcmp(ext, ".h") || !strcmp(ext, ".cpp") ||
        !strcmp(ext, ".hpp") || !strcmp(ext, ".js") || !strcmp(ext, ".ts") ||
        !strcmp(ext, ".json")) return 1;
    return 0;
}
#include "code_signature.h"
static void offer(const char *s, int n, int prefix) {
    if (n <= prefix || n >= 64 || suggestion_count == 6 ||
        memcmp(s, lines[cur_r] + prefix_start, prefix)) return;
    for (int i = 0; i < suggestion_count; i++)
        if ((int)strlen(suggestions[i]) == n && !memcmp(suggestions[i], s, n)) return;
    memcpy(suggestions[suggestion_count], s, n);
    suggestions[suggestion_count][n] = 0;
    suggestion_symbol[suggestion_count++] = -1;
}
static void complete_refresh(void) {
    suggestion_count = 0;
    completion_delim=0;
    if (editing_name || completion_hidden || has_selection()) return;
    int begin=0;
    while(begin<cur_c && lines[cur_r][begin]==' ') begin++;
    if(cur_c-begin>=8 && !memcmp(lines[cur_r]+begin,"#include",8)) {
        int p=begin+8;while(p<cur_c && lines[cur_r][p]==' ') p++;
        if(p<cur_c && (lines[cur_r][p]=='<' || lines[cur_r][p]=='"')) {
            completion_delim=lines[cur_r][p]=='<'?'>':'"';prefix_start=p+1;
            for(int i=prefix_start;i<cur_c;i++) if(lines[cur_r][i]==completion_delim) {completion_delim=0;return;}
            int prefix=cur_c-prefix_start;
            for(int i=0;i<symbol_count && suggestion_count<6;i++) {
                int before=suggestion_count;
                offer(symbols[i].header,strlen(symbols[i].header),prefix);
                if(suggestion_count>before) suggestion_symbol[before]=i;
            }
            if(suggestion_index>=suggestion_count) suggestion_index=0;
            return;
        }
    }
    call_scan();
    if(call_lex) return;
    prefix_start = cur_c;
    while (prefix_start > 0 && ident((unsigned char)lines[cur_r][prefix_start - 1])) prefix_start--;
    int prefix = cur_c - prefix_start;
    if (prefix < 2 || (cur_c < lens[cur_r] && ident((unsigned char)lines[cur_r][cur_c]))) return;
    for (unsigned i = 0; i < sizeof(code_words) / sizeof(code_words[0]); i++)
        offer(code_words[i], strlen(code_words[i]), prefix);
    for (int i = 0; i < symbol_count && suggestion_count < 6; i++) {
        int before=suggestion_count;
        offer(symbols[i].name,strlen(symbols[i].name),prefix);
        if (suggestion_count>before) suggestion_symbol[before]=i;
    }
    for (int r = 0; r < n_lines && suggestion_count < 6; r++) {
        for (int b = 0; b < lens[r] && suggestion_count < 6;) {
            if (!ident((unsigned char)lines[r][b])) { b++; continue; }
            int start = b++;
            while (b < lens[r] && ident((unsigned char)lines[r][b])) b++;
            offer(lines[r] + start, b - start, prefix);
        }
    }
    for(int j=0;j<suggestion_count;j++)
        if(suggestion_symbol[j]<0) for(int i=0;i<symbol_count;i++)
            if(!strcmp(suggestions[j],symbols[i].name)) {suggestion_symbol[j]=i;break;}
    if (suggestion_index >= suggestion_count) suggestion_index = 0;
}
static bool complete_key(int k) {
    complete_refresh();
    if (k == 27) { completion_hidden = true; signature_hidden=true; return true; }
    signature_hidden=false;
    if (suggestion_count && (k == KEY_UP || k == KEY_DOWN)) {
        suggestion_index = (suggestion_index + (k == KEY_DOWN ? 1 : suggestion_count - 1)) % suggestion_count;
        return true;
    }
    if (suggestion_count && k == '\t') {
        char *s = suggestions[suggestion_index];
        int prefix = cur_c - prefix_start, extra = strlen(s) - prefix;
        int close=completion_delim && (cur_c==lens[cur_r] || lines[cur_r][cur_c]!=completion_delim);
        ensure_cap(cur_r, lens[cur_r] + extra + close + 1);
        memmove(lines[cur_r] + cur_c + extra + close, lines[cur_r] + cur_c, lens[cur_r] - cur_c);
        memcpy(lines[cur_r] + cur_c, s + prefix, extra);
        if(close) lines[cur_r][cur_c+extra]=completion_delim;
        lens[cur_r] += extra+close; cur_c += extra+close;
        changed(); drop_selection(); completion_hidden = true;
        return true;
    }
    completion_hidden = false; suggestion_index = 0;
    return false;
}
/* Carry block comments across lines. Strings and line comments end at EOL. */
static void syntax_line(int r, int *block, bool paint, int y) {
    int lang = language(), b = 0;
    while (b < lens[r]) {
        int start = b;
        unsigned color = CODE_FG;
        char c = lines[r][b];
        if (lang && *block) {
            while (b < lens[r]) {
                if (b + 1 < lens[r] && lines[r][b] == '*' && lines[r][b+1] == '/') {
                    b += 2; *block = 0; break;
                }
                b++;
            }
            color = CODE_COMMENT;
        } else if (lang && ((lang == 2 && c == '#') ||
                   (lang == 1 && c == '/' && b + 1 < lens[r] && lines[r][b+1] == '/'))) {
            b = lens[r]; color = CODE_COMMENT;
        } else if (lang == 1 && c == '#') {
            b++;
            while (b < lens[r] && (ident(lines[r][b]) || lines[r][b]==' ')) b++;
            color = CODE_DIRECTIVE;
        } else if (lang == 1 && c == '/' && b + 1 < lens[r] && lines[r][b+1] == '*') {
            b += 2; *block = 1;
            while (b < lens[r]) {
                if (b + 1 < lens[r] && lines[r][b] == '*' && lines[r][b+1] == '/') {
                    b += 2; *block = 0; break;
                }
                b++;
            }
            color = CODE_COMMENT;
        } else if (lang == 1 && c == '<' && lens[r]>=8 && !memcmp(lines[r],"#include",8)) {
            b++;while(b<lens[r] && lines[r][b]!='>') b++;
            if(b<lens[r]) b++;
            color=CODE_STRING;
        } else if (lang && (c == '\'' || c == '"')) {
            b++;
            while (b < lens[r]) {
                if (lines[r][b] == '\\') { b += b + 1 < lens[r] ? 2 : 1; continue; }
                if (lines[r][b++] == c) break;
            }
            color = CODE_STRING;
        } else if (ident((unsigned char)c)) {
            while (b < lens[r] && ident((unsigned char)lines[r][b])) b++;
            if (lang && c >= '0' && c <= '9') color = CODE_NUMBER;
            else if (lang) {
                int next=b;
                while(next<lens[r] && (lines[r][next]==' ' || lines[r][next]=='\t')) next++;
                if(next<lens[r] && lines[r][next]=='(') color=CODE_FUNCTION;
                for (int i=0;i<symbol_count;i++)
                    if((int)strlen(symbols[i].name)==b-start && !memcmp(lines[r]+start,symbols[i].name,b-start)) {
                        color=strchr(symbols[i].signature,'(')?CODE_FUNCTION:CODE_TYPE;break;
                    }
                for (unsigned i = 0; i < sizeof(code_words)/sizeof(code_words[0]); i++)
                if ((int)strlen(code_words[i]) == b-start && !memcmp(lines[r]+start,code_words[i],b-start)) {
                    color = i < 34 ? CODE_KEYWORD : CODE_FUNCTION; break;
                }
            }
        } else { b++; while (b < lens[r] && cont(lines[r][b])) b++; }
        if (!paint) continue;
        int col = col_of(r, start), end = col_of(r, b);
        if (end <= left || col >= left + VIS_COLS) continue;
        if (col < left) { start = byte_of(r, left); col = left; }
        int paint_end = end > left + VIS_COLS ? byte_of(r, left + VIS_COLS) : b;
        /* A bounded temporary avoids requiring NUL-terminated document lines. */
        for (int p = start; p < paint_end;) {
            char glyph[5]; int n = 0;
            glyph[n++] = lines[r][p++];
            while (p < paint_end && cont(lines[r][p]) && n < 4) glyph[n++] = lines[r][p++];
            glyph[n] = 0;
            gfx_text(TEXT_X + (col++ - left) * 8, y, glyph, color, TRANSPARENT);
        }
    }
}
static void completion_draw(void) {
    complete_refresh();
    if (!suggestion_count) return;
    int x = TEXT_X + (col_of(cur_r,cur_c)-left)*8;
    int h = suggestion_count * 18 + 42, width = 360;
    int y = TEXT_Y + (cur_r-top+1)*16;
    if (width > W-TEXT_X-12) width=W-TEXT_X-12;
    if (x + width > W - 12) x = W - 12 - width;
    if (y + h > H - STATUS_H) y = TEXT_Y + (cur_r-top)*16 - h;
    if (y < BAR_H) y = BAR_H;
    gfx_fill_rect(x,y,width,h,RGB(45,45,48));
    for (int i = 0; i < suggestion_count; i++) {
        if (i == suggestion_index) gfx_fill_rect(x+1,y+2+i*18,width-2,18,RGB(9,71,113));
        char label[64]; snprintf(label,sizeof(label),"%s%s",suggestions[i],suggestion_symbol[i]>=0?"  [C API]":"");
        label[(width-8)/8]=0;
        gfx_text(x+4,y+3+i*18,label,CODE_FG,TRANSPARENT);
    }
    int si=suggestion_symbol[suggestion_index];
    char info[144];
    snprintf(info,sizeof(info),"%.143s",si>=0?symbols[si].signature:"Tab: accept  Up/Down: select  Esc: hide");
    info[(width-8)/8]=0;
    gfx_text(x+4,y+h-36,info,CODE_TYPE,TRANSPARENT);
    snprintf(info,sizeof(info),si>=0?"#include <%s>":"Local document / language",si>=0?symbols[si].header:"");
    info[(width-8)/8]=0;
    gfx_text(x+4,y+h-18,info,CODE_COMMENT,TRANSPARENT);
}
