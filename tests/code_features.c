#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define RGB(r,g,b) (((r)<<16)|((g)<<8)|(b))
#define UI_TEXT 0
#define UI_SELECT 1
#define TRANSPARENT 0
#define TEXT_X 54
#define TEXT_Y 36
#define VIS_COLS 30
#define STATUS_H 22
#define BAR_H 32
#define KEY_UP 256
#define KEY_DOWN 257
static char *lines[8], path[96];
static int lens[8], cur_r, cur_c, n_lines, left, top, W=900,H=600;
static bool editing_name, selected;
static unsigned painted[512];
static int has_selection(void) { return selected; }
static bool cont(char c) { return ((unsigned char)c & 0xc0)==0x80; }
static int col_of(int r,int b) { int n=0; for(int i=0;i<b;i++) n+=!cont(lines[r][i]); return n; }
static int byte_of(int r,int col) { int b=0; while(b<lens[r] && col) { b++; while(b<lens[r] && cont(lines[r][b])) b++; col--; } return b; }
static void ensure_cap(int r,int n) { lines[r]=realloc(lines[r],n); }
static void changed(void) {}
static void drop_selection(void) { selected=false; }
static void gfx_text(int x,int y,const char*s,unsigned c,int bg) {
    (void)y;(void)s;(void)bg;int col=(x-TEXT_X)/8;
    if(col>=0 && col<512) painted[col]=c;
}
static void gfx_fill_rect(int x,int y,int w,int h,unsigned c) {(void)x;(void)y;(void)w;(void)h;(void)c;}
#include "../user/code_features.h"
#define CHECK(x) do { if(!(x)) { fprintf(stderr,"failed line %d: %s\n",__LINE__,#x); return 1; } } while(0)
static void document(const char*s) {
    free(lines[0]); lines[0]=strdup(s); lens[0]=strlen(s); n_lines=1; cur_r=0;cur_c=lens[0];
    completion_hidden=false;signature_hidden=false;suggestion_index=0;selected=false;
}
int main(int argc,char **argv) {
    document("pri"); complete_refresh(); CHECK(suggestion_count>0); CHECK(!strcmp(suggestions[0],"printf"));
    CHECK(complete_key('\t')); CHECK(lens[0]==6 && !memcmp(lines[0],"printf",6));
    document("customSymbol cust"); complete_refresh(); CHECK(suggestion_count==1); CHECK(!strcmp(suggestions[0],"customSymbol"));
    CHECK(complete_key(27)); complete_refresh(); CHECK(suggestion_count==0);
    document("pri"); selected=true; complete_refresh(); CHECK(!suggestion_count);
    document("printf"); cur_c=3; complete_refresh(); CHECK(!suggestion_count);
    strcpy(path,"test.c"); document("/* comment"); int block=0; syntax_line(0,&block,false,0); CHECK(block==1);
    document("ends */ int value;"); syntax_line(0,&block,false,0); CHECK(block==0);
    document("\"/* not comment\" // /*"); syntax_line(0,&block,false,0); CHECK(block==0);
    document("/* this long comment crosses the right viewport boundary and then closes */ int x;");
    syntax_line(0,&block,true,0); CHECK(block==0);
    strcpy(path,"test.py"); document("# /*"); syntax_line(0,&block,false,0); CHECK(block==0);
    strcpy(path,"notes.txt"); document("/* plain text"); syntax_line(0,&block,false,0); CHECK(block==0);
    char header[]="// fakeApi()\n#define MY_FLAG 8\ntypedef unsigned int custom_type;\nint custom_api(int value);\nstruct event;\n";
    index_parse(header,"myos.h");CHECK(symbol_count==4);
    document("custom_a");complete_refresh();CHECK(suggestion_count==1);CHECK(suggestion_symbol[0]>=0);
    CHECK(!strcmp(symbols[suggestion_symbol[0]].header,"myos.h"));
    CHECK(strstr(symbols[suggestion_symbol[0]].signature,"int value"));
    completion_draw();
    for(int arg=1;arg<argc;arg++) {
        FILE *f=fopen(argv[arg],"rb");CHECK(f);fseek(f,0,SEEK_END);long n=ftell(f);rewind(f);
        char *s=malloc(n+1);CHECK(fread(s,1,n,f)==(size_t)n);s[n]=0;fclose(f);
        const char *base=strrchr(argv[arg],'/');index_parse(s,base?base+1:argv[arg]);free(s);
    }
    if(argc>1) {
        strcpy(path,"test.c");
        document("win_cre");complete_refresh();CHECK(suggestion_count>0);CHECK(!strcmp(suggestions[0],"win_create"));
        CHECK(suggestion_symbol[0]>=0);CHECK(!strcmp(symbols[suggestion_symbol[0]].header,"win.h"));
        document("#include <wi");complete_refresh();CHECK(suggestion_count>0);CHECK(!strcmp(suggestions[0],"win.h"));
        CHECK(complete_key('\t'));CHECK(lens[0]==16 && !memcmp(lines[0],"#include <win.h>",16));
        document("gfx_fill_r");complete_refresh();CHECK(suggestion_count>0);CHECK(!strcmp(suggestions[0],"gfx_fill_rect"));
        document("gfx_fill_rect(10,20,RGB(1,2,))");cur_c=lens[0]-2;CHECK(signature_refresh());CHECK(!strcmp(call_name,"RGB") && call_parameters==3 && !strcmp(call_header,"gfx.h"));
        document("clipboard_g");complete_refresh();CHECK(suggestion_count>0);CHECK(!strcmp(suggestions[0],"clipboard_get"));
    }
    strcpy(path,"colors.c");document("int main() { return 42; }");memset(painted,0,sizeof(painted));
    syntax_line(0,&block,true,0);CHECK(painted[0]==CODE_KEYWORD);CHECK(painted[4]==CODE_FUNCTION);CHECK(painted[20]==CODE_NUMBER);
    document("#include <win.h>");syntax_line(0,&block,true,0);CHECK(painted[0]==CODE_DIRECTIVE);CHECK(painted[9]==CODE_STRING);
    document("\"string\" // comment");syntax_line(0,&block,true,0);CHECK(painted[0]==CODE_STRING);CHECK(painted[9]==CODE_COMMENT);
    char calls_header[]="int outer(int a, int b); int inner(int a, int b); int callback(void (*fn)(int, int), int n); int variadic(const char *fmt, ...); int none(void);";
    index_parse(calls_header,"calls.h");
    document("outer(");CHECK(signature_refresh());CHECK(call_argument==0 && call_parameters==2);
    CHECK(!strncmp(call_signature+call_active_start,"int a",5));
    document("outer(1,");CHECK(signature_refresh());CHECK(call_argument==1);CHECK(!strncmp(call_signature+call_active_start,"int b",5));
    document("outer(inner(1, 2), ");CHECK(signature_refresh());CHECK(!strcmp(call_name,"outer") && call_argument==1);
    document("outer(inner(1, ");CHECK(signature_refresh());CHECK(!strcmp(call_name,"inner") && call_argument==1);
    document("outer(\"a,b\", ");CHECK(signature_refresh());CHECK(call_argument==1);
    document("outer(1 /* , ) */ , ");CHECK(signature_refresh());CHECK(call_argument==1);
    document("outer((1,2), ");CHECK(signature_refresh());CHECK(call_argument==1);
    document("outer((int[]){1,2}, ");CHECK(signature_refresh());CHECK(call_argument==1);
    document("outer(inner(1, 2))");cur_c=14;CHECK(signature_refresh());CHECK(!strcmp(call_header,"calls.h") && !strcmp(call_name,"inner"));
    document("outer(1, 2)");CHECK(!signature_refresh());
    document("// outer(1,");CHECK(!signature_refresh());
    document("callback(");CHECK(signature_refresh());CHECK(call_parameters==2);CHECK(strstr(call_signature+call_active_start,"(*fn)(int, int)"));
    document("variadic(\"x\", 1, 2, ");CHECK(signature_refresh());CHECK(call_active_start>=0 && !strncmp(call_signature+call_active_start,"...",3));
    document("none(");CHECK(signature_refresh());CHECK(!call_parameters);signature_draw();
    document("int own(int first, int second); own(1, ");CHECK(signature_refresh());CHECK(!strcmp(call_header,"current file") && call_argument==1);
    document("outer(1,");lines[1]=strdup("inner(2, 3), ");lens[1]=strlen(lines[1]);n_lines=2;cur_r=1;cur_c=lens[1];
    CHECK(signature_refresh());CHECK(!strcmp(call_name,"outer") && call_argument==2);free(lines[1]);lines[1]=NULL;cur_r=0;
    document("");CHECK(paired_key('('));CHECK(lens[0]==2 && cur_c==1 && !memcmp(lines[0],"()",2));
    CHECK(paired_key(')'));CHECK(cur_c==2 && lens[0]==2);
    document("");CHECK(paired_key('['));CHECK(paired_key(8));CHECK(!lens[0] && !cur_c);
    document("// comment");CHECK(!paired_key('('));
    document("\"text");CHECK(!paired_key('('));
    document("outer(\"inner");complete_refresh();CHECK(!suggestion_count);
    document("pri");complete_key(27);CHECK(signature_hidden);complete_key('n');CHECK(!signature_hidden);
    free(lines[0]); puts("Code feature tests passed"); return 0;
}
