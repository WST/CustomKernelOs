/* Lexical call tracking: commas count only at the call's own nesting level. */
struct call_frame { char name[64]; int argument; char opener; };
static struct call_frame call_stack[128];
static int call_depth, call_lex; /* 0 code, 1 block comment, 2 line comment, or quote */
static char call_signature[384], call_header[40], call_name[64];
static int call_argument, call_parameters, call_active_start, call_active_end;
static bool signature_hidden;

static void call_scan(void) {
    call_depth=call_lex=0;
    char word[64];int word_n=0;bool escaped=false,overflow=false;
    for(int r=0;r<=cur_r;r++) {
        int limit=r==cur_r?cur_c:lens[r];
        for(int b=0;b<limit;b++) {
            int c=(unsigned char)lines[r][b],next=b+1<limit?lines[r][b+1]:0;
            if(call_lex==1) {if(c=='*' && next=='/') {call_lex=0;b++;}continue;}
            if(call_lex==2) break;
            if(call_lex) {
                if(escaped) escaped=false;
                else if(c=='\\') escaped=true;
                else if(c==call_lex) call_lex=0;
                continue;
            }
            if(c=='/' && next=='*') {call_lex=1;b++;word_n=0;continue;}
            if((c=='/' && next=='/') || (language()==2 && c=='#')) {call_lex=2;word_n=0;break;}
            if(c=='"' || c=='\'') {call_lex=c;word_n=0;continue;}
            if(ident(c)) {
                int start=b;while(b+1<limit && ident((unsigned char)lines[r][b+1])) b++;
                word_n=b-start+1;
                if(word_n>=64) word_n=0;
                if(word_n) memcpy(word,lines[r]+start,word_n);
                word[word_n]=0;continue;
            }
            if(c==' ' || c=='\t' || c=='\r') continue;
            if(c=='(' || c=='[' || c=='{') {
                if(call_depth==128) {overflow=true;break;}
                struct call_frame *f=&call_stack[call_depth++];
                f->opener=c;f->argument=0;f->name[0]=0;
                if(c=='(' && word_n && strcmp(word,"if") && strcmp(word,"for") &&
                    strcmp(word,"while") && strcmp(word,"switch") && strcmp(word,"sizeof")) strcpy(f->name,word);
            } else if(c==')' || c==']' || c=='}') {
                if(call_depth && call_stack[call_depth-1].opener==(c==')'?'(':c==']'?'[':'{')) call_depth--;
                else call_depth=0;
            } else if(c==',' && call_depth && call_stack[call_depth-1].opener=='(') call_stack[call_depth-1].argument++;
            word_n=0;
        }
        if(overflow) {call_depth=0;break;}
        if(r<cur_r && call_lex==2) call_lex=0;
        if(r<cur_r && call_lex && call_lex!=1 && !escaped) call_lex=0;
        escaped=false;
    }
}
static int signature_open(const char *s,const char *name) {
    int n=strlen(name);
    for(int b=0;s[b];b++) if((int)strlen(s+b)>=n && !memcmp(s+b,name,n) &&
        (!b || !ident(s[b-1])) && !ident(s[b+n])) {
        int p=b+n;while(s[p]==' ' || s[p]=='\t') p++;
        if(s[p]=='(') return p;
    }
    return -1;
}
static bool signature_arguments(const char *s,int open,int argument) {
    int nesting=0,start=open+1,parameter=0,active_start=-1,active_end=-1;
    for(int b=start;s[b];b++) {
        char c=s[b];
        if(c=='(' || c=='[' || c=='{') nesting++;
        if(c==')' && !nesting) {
            int a=start,e=b;while(a<e && s[a]==' ') a++;while(e>a && s[e-1]==' ') e--;
            if(a==e || (!parameter && e-a==4 && !memcmp(s+a,"void",4))) {
                call_parameters=0;call_active_start=call_active_end=-1;return true;
            }
            if(parameter==argument || (e-a==3 && !memcmp(s+a,"...",3) && argument>=parameter)) {active_start=a;active_end=e;}
            call_parameters=parameter+1;call_active_start=active_start;call_active_end=active_end;return true;
        }
        if(c==')' || c==']' || c=='}') {if(nesting) nesting--;}
        if(c==',' && !nesting) {
            int a=start,e=b;while(a<e && s[a]==' ') a++;while(e>a && s[e-1]==' ') e--;
            if(parameter==argument) {active_start=a;active_end=e;}
            parameter++;start=b+1;
        }
    }
    return false; /* A truncated declaration must never advertise wrong parameters. */
}
static bool local_signature(const char *name) {
    for(int r=0;r<n_lines;r++) {
        char candidate[384];int out=0;
        for(int rr=r;rr<n_lines && rr<r+6 && out<380;rr++) {
            for(int b=0;b<lens[rr] && out<380;b++) candidate[out++]=lines[rr][b];
            candidate[out++]=' ';candidate[out]=0;
            int open=signature_open(candidate,name);if(open<0) break;
            int pos=open-(int)strlen(name);while(pos>0 && candidate[pos-1]==' ') pos--;
            bool declaration=false;
            for(int b=0;b<pos;b++) {
                if(ident(candidate[b])) declaration=true;
                if(!ident(candidate[b]) && candidate[b]!=' ' && candidate[b]!='\t' && candidate[b]!='*') {declaration=false;break;}
            }
            if(!declaration || strstr(candidate,"return ")) break;
            if(signature_arguments(candidate,open,call_argument)) {
                int end=open,depth=0;
                do {if(candidate[end]=='(') depth++;else if(candidate[end]==')') depth--;end++;} while(candidate[end] && depth);
                candidate[end]=0;strcpy(call_signature,candidate);strcpy(call_header,"current file");return true;
            }
        }
    }
    return false;
}
static bool signature_refresh(void) {
    call_signature[0]=0;call_scan();
    if(!language() || editing_name || signature_hidden || call_lex==1 || call_lex==2 || has_selection()) return false;
    for(int d=call_depth-1;d>=0;d--) {
        if(!call_stack[d].name[0]) continue;
        strcpy(call_name,call_stack[d].name);call_argument=call_stack[d].argument;
        if(local_signature(call_name)) return true;
        for(int i=0;i<symbol_count;i++) if(!strcmp(call_name,symbols[i].name)) {
            int open=signature_open(symbols[i].signature,call_name);
            if(open>=0 && signature_arguments(symbols[i].signature,open,call_argument)) {
                strcpy(call_signature,symbols[i].signature);strcpy(call_header,symbols[i].header);return true;
            }
        }
        return false;
    }
    return false;
}
static void signature_draw(void) {
    if(!signature_refresh()) return;
    int width=W-TEXT_X-16;if(width>664) width=664;
    int columns=(width-16)/8;if(columns<8) return;
    int count=strlen(call_signature),rows=(count+columns-1)/columns;
    int height=rows*18+48;
    int x=TEXT_X,y=TEXT_Y+(cur_r-top)*16-height-4;
    if(y<BAR_H) {
        y=TEXT_Y+(cur_r-top+1)*16+4;
        if(suggestion_count) y+=suggestion_count*18+46;
    }
    if(y+height>H-STATUS_H) y=H-STATUS_H-height;
    if(y<BAR_H) y=BAR_H;
    gfx_fill_rect(x,y,width,height,RGB(45,45,48));
    gfx_fill_rect(x,y,width,2,RGB(0,122,204));
    char text[144];
    if(call_parameters) snprintf(text,sizeof(text),"%s | argument %d (%d declared) | %s",call_name,call_argument+1,call_parameters,call_header);
    else snprintf(text,sizeof(text),"%s | no parameters | %s",call_name,call_header);
    text[columns<143?columns:143]=0;gfx_text(x+8,y+7,text,CODE_FG,TRANSPARENT);
    for(int b=0;b<count;b++) {
        int px=x+8+(b%columns)*8,py=y+29+(b/columns)*18;
        bool active=b>=call_active_start && b<call_active_end;
        if(active) gfx_fill_rect(px,py,8,18,RGB(9,71,113));
        char glyph[2]={call_signature[b],0};gfx_text(px,py,glyph,active?CODE_TYPE:CODE_FG,TRANSPARENT);
    }
}
static bool paired_key(int k) {
    if(!language() || has_selection()) return false;
    call_scan();
    bool code=call_lex==0;
    char close=k=='('?')':k=='['?']':k=='{'?'}':k=='"'?'"':k=='\''?'\'':0;
    if(cur_c<lens[cur_r] && lines[cur_r][cur_c]==k &&
        ((code && (k==')' || k==']' || k=='}')) || ((k=='"' || k=='\'') && call_lex==k &&
        (!cur_c || lines[cur_r][cur_c-1]!='\\')))) {cur_c++;drop_selection();return true;}
    if((k==8 || k==127) && cur_c && cur_c<lens[cur_r]) {
        char a=lines[cur_r][cur_c-1],b=lines[cur_r][cur_c];
        if((code && ((a=='(' && b==')') || (a=='[' && b==']') || (a=='{' && b=='}'))) ||
            ((a=='"' || a=='\'') && a==b && call_lex==a && (cur_c<2 || lines[cur_r][cur_c-2]!='\\'))) {
            memmove(lines[cur_r]+cur_c-1,lines[cur_r]+cur_c+1,lens[cur_r]-cur_c-1);
            lens[cur_r]-=2;cur_c--;changed();drop_selection();return true;
        }
    }
    if(!close || !code) return false;
    if(cur_c<lens[cur_r] && ident((unsigned char)lines[cur_r][cur_c])) return false;
    if((k=='"' || k=='\'') && cur_c && ident((unsigned char)lines[cur_r][cur_c-1])) return false;
    ensure_cap(cur_r,lens[cur_r]+3);
    memmove(lines[cur_r]+cur_c+2,lines[cur_r]+cur_c,lens[cur_r]-cur_c);
    lines[cur_r][cur_c++]=k;lines[cur_r][cur_c]=close;lens[cur_r]+=2;
    changed();drop_selection();return true;
}
