/* Compact application chrome: menus, command centre, active tab and breadcrumbs. */
static int menu_open=-1;
static const char *menus[]={"File","Edit","View","Run"};
static const char *menu_items[4][4]={
    {"New File          Ctrl+N","Open File         Ctrl+O","Save              Ctrl+S","Open Folder..."},
    {"Cut               Ctrl+X","Copy              Ctrl+C","Paste             Ctrl+V","Select All        Ctrl+A"},
    {"Open Folder...","Refresh Explorer",NULL,NULL},
    {"Build C File      Ctrl+B",NULL,NULL,NULL}
};
static void chrome_text(int x,int y,const char *s,int columns,unsigned color) {
    char buf[160];int n=strlen(s);
    if(columns<1) return;
    if(columns>150) columns=150;
    if(n>columns) {n=columns;while(n>0 && cont(s[n])) n--;}
    memcpy(buf,s,n);buf[n]=0;gfx_text(x,y,buf,color,TRANSPARENT);
}
static const char *file_label(void) {
    const char *base=strrchr(path,'/');return path[0]?(base?base+1:path):"Untitled-1";
}
static int tab_width(void) {
    int width=80+u8_chars(file_label())*8;
    if(width<160) width=160;
    if(width>300) width=300;
    return width;
}
static int command_width(void) {int w=W-500;return w>300?300:w;}
static int command_x(void) {return (W-command_width())/2;}
static void chrome_draw(void) {
    unsigned muted=RGB(155,155,155);
    gfx_fill_rect(0,0,W,34,RGB(24,24,24));
    gfx_text(12,9,"<> ",CODE_KEYWORD,TRANSPARENT);
    for(int i=0;i<4;i++) {
        if(menu_open==i) gfx_fill_rect(42+i*48,3,46,28,RGB(55,55,55));
        gfx_text(48+i*48,9,menus[i],CODE_FG,TRANSPARENT);
    }
    int cx=command_x();
    gfx_fill_rect(cx,5,command_width(),24,RGB(51,51,51));
    const char *folder=strrchr(workspace,'/');folder=folder && folder[1]?folder+1:workspace;
    char command[96];snprintf(command,sizeof(command),">  %s",folder);
    chrome_text(cx+10,9,command,(command_width()-20)/8,CODE_FG);
    gfx_text(W-142,9,"Save",CODE_FG,TRANSPARENT);
    gfx_text(W-82,9,"> Build",CODE_TYPE,TRANSPARENT);
    gfx_fill_rect(SIDE_W,34,W-SIDE_W,34,RGB(24,24,24));
    gfx_fill_rect(SIDE_W,34,tab_width(),34,CODE_BG);
    gfx_fill_rect(SIDE_W,34,tab_width(),2,RGB(0,122,204));
    gfx_text(SIDE_W+14,44,language()==2?"Py":language()==1?"C":"{}",CODE_KEYWORD,TRANSPARENT);
    chrome_text(SIDE_W+36,44,file_label(),(tab_width()-72)/8,CODE_FG);
    gfx_text(SIDE_W+tab_width()-22,44,modified?"*":"x",CODE_FG,TRANSPARENT);
    gfx_fill_rect(SIDE_W,68,W-SIDE_W,22,CODE_BG);
    char crumb[240];snprintf(crumb,sizeof(crumb),"%s  >  %s",workspace,file_label());
    chrome_text(SIDE_W+16,71,crumb,(W-SIDE_W-32)/8,muted);
    gfx_fill_rect(SIDE_W,89,W-SIDE_W,1,RGB(43,43,43));
}
static void chrome_overlay(void) {
    if(menu_open>=0) {
        int x=42+menu_open*48;
        gfx_fill_rect(x+3,37,258,124,RGB(15,15,15));
        gfx_fill_rect(x,34,258,124,RGB(37,37,38));
        for(int i=0;i<4 && menu_items[menu_open][i];i++)
            gfx_text(x+12,43+i*28,menu_items[menu_open][i],CODE_FG,TRANSPARENT);
    }
    if(editing_name || editing_folder) {
        int width=W-80;if(width>640) width=640;
        int x=(W-width)/2;
        gfx_fill_rect(x+4,42,width,92,RGB(15,15,15));
        gfx_fill_rect(x,38,width,92,RGB(37,37,38));
        gfx_text(x+12,46,editing_folder?"Open Folder":"File path",CODE_FG,TRANSPARENT);
        gfx_fill_rect(x+10,68,width-20,30,RGB(0,122,204));
        gfx_fill_rect(x+11,69,width-22,28,RGB(60,60,60));
        const char *value=editing_folder?folder_input:path;
        int columns=(width-40)/8,chars=u8_chars(value),skip=chars>columns?chars-columns:0;
        const char *visible=value;
        while(skip-- && *visible) {visible++;while(*visible && cont(*visible)) visible++;}
        chrome_text(x+18,75,visible,columns,CODE_FG);
        gfx_fill_rect(x+18+u8_chars(visible)*8,75,1,16,CODE_FG);
        gfx_text(x+12,105,"Enter to confirm    Esc to cancel",RGB(155,155,155),TRANSPARENT);
    }
}
static bool chrome_click(int x,int y) {
    if(editing_name || editing_folder) return true;
    if(menu_open>=0 && y>=34 && y<158) {
        int mx=42+menu_open*48;
        if(x>=mx && x<mx+258) {
            int row=(y-34)/28,menu=menu_open;menu_open=-1;
            if(row>=4 || !menu_items[menu][row]) return true;
            if(menu==0) {if(row<3) text_key(row==0?CTRL('n'):row==1?CTRL('o'):CTRL('s'));else folder_begin();}
            if(menu==1) text_key(row==0?CTRL('x'):row==1?CTRL('c'):row==2?CTRL('v'):CTRL('a'));
            if(menu==2) {if(!row) folder_begin();else {char p[128];strcpy(p,workspace);workspace_open(p);}}
            if(menu==3) build_start();
            return true;
        }
    }
    if(y<34) {
        if(x>=42 && x<234) {int i=(x-42)/48;menu_open=menu_open==i?-1:i;return true;}
        menu_open=-1;
        if(x>=W-90) build_start();
        else if(x>=W-150) {save();set_title();}
        else if(x>=command_x() && x<command_x()+command_width()) folder_begin();
        return true;
    }
    menu_open=-1;
    if(x>=SIDE_W && y<68) {
        if(x>=SIDE_W+tab_width()-30 && x<SIDE_W+tab_width()) text_key(CTRL('n'));
        return true;
    }
    if(x>=SIDE_W && y<90) {text_key(CTRL('o'));return true;}
    return false;
}
