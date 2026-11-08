/* Index public declarations from installed C headers, without a language server. */
#define INDEX_MAX 1024
struct code_symbol { char name[64], header[40], signature[384]; };
static struct code_symbol symbols[INDEX_MAX];
static int symbol_count;
static bool index_ident(int c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '_';
}
static void index_add(const char *s, int n, const char *header, const char *decl, int length) {
    if (n < 2 || n >= 64 || symbol_count == INDEX_MAX) return;
    for (int i = 0; i < symbol_count; i++)
        if ((int)strlen(symbols[i].name) == n && !memcmp(symbols[i].name,s,n)) return;
    struct code_symbol *sym = &symbols[symbol_count++];
    memcpy(sym->name,s,n); sym->name[n]=0;
    snprintf(sym->header,sizeof(sym->header),"%s",header);
    int out=0;
    for (int i=0;i<length && out<(int)sizeof(sym->signature)-1;i++) {
        char c=decl[i];
        if (c=='\n' || c=='\r' || c=='\t') c=' ';
        if (c==' ' && (!out || sym->signature[out-1]==' ')) continue;
        sym->signature[out++]=c;
    }
    sym->signature[out]=0;
}
static void index_parse(char *s, const char *header) {
    /* Remove comments/string contents first so examples never become symbols. */
    int len=strlen(s), block=0, line=0, quote=0;
    for (int i=0;i<len;i++) {
        int c=s[i], next=i+1<len?s[i+1]:0;
        if (block) { s[i]=' '; if(c=='*' && next=='/') {s[++i]=' ';block=0;} }
        else if (line) { if(c=='\n') line=0; else s[i]=' '; }
        else if (quote) {
            s[i]=' ';
            if(c=='\\' && next) s[++i]=' ';
            else if(c==quote) quote=0;
        } else if(c=='/' && next=='*') {s[i]=s[i+1]=' ';i++;block=1;}
        else if(c=='/' && next=='/') {s[i]=s[i+1]=' ';i++;line=1;}
        else if(c=='"' || c=='\'') {quote=c;s[i]=' ';}
    }
    int decl=0, is_typedef=0, last=-1, last_n=0;
    for(int b=0;b<len;) {
        if(s[b]==';') {
            if(is_typedef && last>=0) index_add(s+last,last_n,header,s+decl,b-decl+1);
            decl=++b;is_typedef=0;continue;
        }
        if(s[b]=='#') {
            int e=b;while(e<len && s[e]!='\n') e++;
            int p=b+1;while(p<e && (s[p]==' ' || s[p]=='\t')) p++;
            if(e-p>6 && !memcmp(s+p,"define",6) && !index_ident(s[p+6])) {
                p+=6;while(p<e && (s[p]==' ' || s[p]=='\t')) p++;
                int st=p;while(p<e && index_ident(s[p])) p++;
                index_add(s+st,p-st,header,s+b,e-b);
            }
            b=e;decl=e;continue;
        }
        if(!index_ident(s[b]) || (s[b]>='0' && s[b]<='9')) {b++;continue;}
        int st=b++;while(b<len && index_ident(s[b])) b++;
        int n=b-st, p=b;while(p<len && (s[p]==' ' || s[p]=='\n' || s[p]=='\t' || s[p]=='\r')) p++;
        if(n==7 && !memcmp(s+st,"typedef",7)) is_typedef=1;
        if(p<len && s[p]=='(' && !(n==6 && !memcmp(s+st,"sizeof",6)) &&
            !(n==13 && !memcmp(s+st,"__attribute__",13))) {
            int e=p;while(e<len && s[e]!=';' && s[e]!='{') e++;
            index_add(s+st,n,header,s+decl,e-decl);
        }
        if((n==6 && !memcmp(s+st,"struct",6)) || (n==4 && !memcmp(s+st,"enum",4))) {
            int e=p;while(e<len && index_ident(s[e])) e++;
            index_add(s+p,e-p,header,s+st,e-st);
        }
        last=st;last_n=n;
    }
}
