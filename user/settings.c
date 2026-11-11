#include "win.h"
#include "img.h"

#define W 760
#define H 520
#define CX 214
#define CW 522
#define FIELD_Y 378
#define ACTION_Y 432
#define INK RGB(39,44,52)
#define MUTED RGB(118,125,136)
#define BORDER RGB(225,228,233)
#define CANVAS RGB(247,248,250)
static int hover_x=-1, hover_y=-1;
static bool over(int x,int y,int w,int h) { return ui_inside(hover_x,hover_y,x,y,w,h); }
static void label(int size,int x,int y,const char *s,color_t c) { text_draw(FONT_UI,size,x,y,s,c); }
static void card(int x,int y,int w,int h) {
    gfx_round_rect(x,y+2,w,h,12,RGB(236,238,242));
    gfx_round_frame(x,y,w,h,12,BORDER,C_WHITE);
}
static void icon(int kind,int x,int y,color_t c) {
    if(kind==0) {
        gfx_round_frame(x+5,y,12,8,2,c,C_WHITE);
        gfx_line(x+11,y+8,x+11,y+13,c); gfx_line(x+2,y+13,x+20,y+13,c);
        gfx_round_frame(x,y+14,7,6,1,c,C_WHITE); gfx_round_frame(x+15,y+14,7,6,1,c,C_WHITE);
    } else if(kind==1) {
        gfx_round_frame(x,y,23,20,3,c,C_WHITE);
        gfx_fill_circle(x+16,y+6,2,c);
        gfx_line(x+3,y+16,x+9,y+9,c); gfx_line(x+9,y+9,x+15,y+16,c); gfx_line(x+15,y+16,x+20,y+12,c);
    } else if(kind==2) {
        gfx_round_frame(x,y,24,16,3,c,C_WHITE);
        gfx_line(x+12,y+17,x+12,y+21,c); gfx_line(x+6,y+22,x+18,y+22,c);
    } else {
        gfx_circle(x+11,y+10,10,c); gfx_fill_circle(x+11,y+5,1,c);
        gfx_line(x+11,y+9,x+11,y+15,c);
    }
}
static void button(int x,int y,int w,const char *name,bool primary) {
    color_t bg=primary ? UI_ACCENT : C_WHITE;
    if(over(x,y,w,38)) bg=primary ? RGB(212,71,24) : RGB(239,241,244);
    gfx_round_frame(x,y,w,38,8,primary ? bg : BORDER,bg);
    text_draw(FONT_UI_MEDIUM,14,x+(w-text_width(FONT_UI_MEDIUM,14,name))/2,y+10,name,primary ? C_WHITE : INK);
}
static void row(int y,int kind,const char *title,const char *description) {
    card(CX,y,CW,76);
    if(over(CX,y,CW,76)) gfx_round_rect(CX+1,y+1,CW-2,74,12,RGB(252,247,244));
    gfx_round_rect(CX+16,y+16,44,44,10,RGB(251,236,228));
    icon(kind,CX+26,y+27,UI_ACCENT);
    text_draw(FONT_UI_MEDIUM,15,CX+76,y+15,title,INK);
    label(12,CX+76,y+40,description,MUTED);
    gfx_line(CX+CW-28,y+32,CX+CW-22,y+38,MUTED);
    gfx_line(CX+CW-22,y+38,CX+CW-28,y+44,MUTED);
}

static char dns[16], path[128], status[100];
static const char *wall_names[] = {"Луна", "Багряные горы", "Млечный Путь", "Комета", "Синие горы", "Город на закате"};
static const char *wall_files[] = {"moon", "crimson-mountains", "milky-way", "comet", "blue-mountains", "sunset-street"};
static img thumbs[6];
static window *win;

static void wall_path(int i, char *out, int cap) {
    snprintf(out,cap,"/usr/share/wallpapers/%s.jpg",wall_files[i]);
}
static void load_thumbs(void) {
    for(int i=0;i<6;i++) {
        char name[128]; wall_path(i,name,sizeof(name));
        int fd=open(name,O_READ); if(fd<0) continue;
        int len=lseek(fd,0,SEEK_END); lseek(fd,0,SEEK_SET);
        if(len>0 && len<=8*1024*1024) {
            unsigned char *data=malloc(len);
            if(data && read(fd,data,len)==len) img_decode(data,len,&thumbs[i],160);
            free(data);
        }
        close(fd);
    }
}
static void gallery(void) {
    for(int i=0;i<6;i++) {
        int x=CX+(i%3)*180, y=100+(i/3)*120;
        char name[128]; wall_path(i,name,sizeof(name));
        bool selected=!strcmp(path,name);
        color_t edge=selected ? UI_ACCENT : over(x,y,162,108) ? RGB(177,184,195) : BORDER;
        gfx_round_rect(x,y+2,162,108,10,RGB(234,237,242));
        gfx_round_frame(x,y,162,108,10,edge,C_WHITE);
        if(selected) gfx_round_frame(x+1,y+1,160,106,9,UI_ACCENT,C_WHITE);
        img *im=&thumbs[i];
        int iw=152,ih=74;
        gfx_round_rect(x+5,y+5,iw,ih,6,RGB(40,30,40));
        if(im->px) {
            int cw=im->w,ch=im->h;
            if(im->w*ih>im->h*iw) cw=im->h*iw/ih; else ch=im->w*ih/iw;
            if(cw<1) cw=1;
            if(ch<1) ch=1;
            for(int yy=0;yy<ih;yy++) for(int xx=0;xx<iw;xx++) {
                int dx=xx<6 ? 6-xx : xx>=iw-6 ? xx-(iw-7) : 0;
                int dy=yy<6 ? 6-yy : yy>=ih-6 ? yy-(ih-7) : 0;
                if(dx*dx+dy*dy>36) continue;
                gfx_pixel(x+5+xx,y+5+yy,im->px[((im->h-ch)/2+yy*ch/ih)*im->w+(im->w-cw)/2+xx*cw/iw]);
            }
        }
        label(13,x+12,y+86,wall_names[i],INK);
        if(selected) {
            gfx_fill_circle(x+143,y+18,9,UI_ACCENT);
            gfx_thick_line(x+139,y+18,x+142,y+21,1,C_WHITE);
            gfx_thick_line(x+142,y+21,x+148,y+15,1,C_WHITE);
        }
    }
}

static int page, focus;
static const char *tabs[] = {"Сеть", "Рабочий стол", "Экран", "Система"};

static void launch(const char *name) {
    char *args[] = {(char *)name, NULL};
    spawn(name, args, -1);
}
static void field(int y, const char *s, int selected) {
    gfx_round_frame(CX,y,CW,38,8,selected ? UI_ACCENT : BORDER,C_WHITE);
    gfx_set_clip(CX+12,y+5,CW-24,28);
    int width=text_width(FONT_UI,14,s);
    int x=CX+12;
    if(selected && width>CW-30) x=CX+CW-18-width;
    label(14,x,y+10,s,INK);
    if(selected) gfx_line(x+width+2,y+10,x+width+2,y+27,UI_ACCENT);
    gfx_no_clip();
}
static void detail(int x,int y,const char *name,const char *value) {
    label(12,x,y,name,MUTED);
    text_draw(FONT_UI_MEDIUM,15,x,y+20,value,INK);
}
static void draw(void) {
    char line[160], a[32], b[32];
    static const char *subtitles[]={"Подключение, адреса и DNS", "Выберите фон, который вам нравится", "Изображение, разрешение и курсор", "Информация и полезные инструменты"};
    gfx_clear(CANVAS);
    gfx_fill_rect(0,0,190,H,RGB(239,241,245));
    gfx_line(189,0,189,H,BORDER);
    gfx_round_rect(20,24,38,38,11,UI_ACCENT);
    icon(3,28,32,C_WHITE);
    text_draw(FONT_UI_BOLD,18,70,26,"MyOS",INK);
    label(12,70,50,"Настройки",MUTED);
    for(int i=0;i<4;i++) {
        int y=100+i*52;
        if(i==page) {
            gfx_round_rect(12,y+2,166,44,9,RGB(227,230,235));
            gfx_round_rect(12,y,166,44,9,C_WHITE);
            gfx_round_rect(12,y+11,3,22,1,UI_ACCENT);
        } else if(over(12,y,166,44)) gfx_round_rect(12,y,166,44,9,RGB(229,232,237));
        icon(i,26,y+11,i==page ? UI_ACCENT : MUTED);
        text_draw(i==page ? FONT_UI_MEDIUM : FONT_UI,14,64,y+14,tabs[i],i==page ? INK : MUTED);
    }
    gfx_line(20,H-70,170,H-70,BORDER);
    label(12,24,H-50,"Системные настройки",MUTED);
    text_draw(FONT_UI_BOLD,26,CX,24,tabs[page],INK);
    label(13,CX,62,subtitles[page],MUTED);
    if(page==0) {
        struct net_info n; memset(&n,0,sizeof(n)); netinfo(&n);
        card(CX,100,CW,76);
        gfx_round_rect(CX+16,116,44,44,10,RGB(235,247,239));
        icon(0,CX+27,128,RGB(55,137,88));
        text_draw(FONT_UI_MEDIUM,16,CX+76,116,n.up ? "Intel e1000 · Проводная сеть" : "Адаптер не найден",INK);
        snprintf(line,sizeof(line),"eth0 · %02x:%02x:%02x:%02x:%02x:%02x",n.mac[0],n.mac[1],n.mac[2],n.mac[3],n.mac[4],n.mac[5]);
        label(12,CX+76,143,line,MUTED);
        gfx_fill_circle(CX+CW-113,137,4,n.up && n.ip ? RGB(61,156,100) : MUTED);
        label(12,CX+CW-102,130,n.up && n.ip ? "Подключено" : "Нет адреса",MUTED);
        card(CX,192,CW,136);
        detail(CX+20,208,"IPv4-адрес",ip_str(n.ip,a));
        detail(CX+280,208,"Маска подсети",ip_str(n.mask,b));
        detail(CX+20,266,"Шлюз",ip_str(n.gw,a));
        snprintf(line,sizeof(line),"%s · RX %u / TX %u",n.dhcp ? "DHCP" : "Static",n.rx_packets,n.tx_packets);
        detail(CX+280,266,"Получение адреса / пакеты",line);
        snprintf(line,sizeof(line),"DNS-сервер · сейчас %s",ip_str(n.dns,a));
        label(13,CX,352,line,MUTED);
        field(FIELD_Y,dns,focus);
        button(CX,ACTION_Y,180,"Применить DNS",true);
        label(12,CX+198,ACTION_Y+12,"Действует до перезагрузки",MUTED);
    } else if(page==1) {
        gallery();
        label(13,CX,352,"Своё изображение",INK);
        label(12,CX+343,353,"PNG · JPEG · GIF · BMP",MUTED);
        field(FIELD_Y,path,focus);
        button(CX,ACTION_Y,180,"Установить обои",true);
        button(CX+194,ACTION_Y,180,"По умолчанию",false);
    } else if(page==2) {
        struct gfx_info2 gi; gfx_info2(&gi);
        card(CX,100,CW,166);
        gfx_round_rect(CX+24,124,122,96,10,RGB(251,236,228));
        gfx_round_frame(CX+44,144,82,52,5,UI_ACCENT,C_WHITE);
        gfx_line(CX+85,197,CX+85,208,UI_ACCENT); gfx_line(CX+64,209,CX+106,209,UI_ACCENT);
        snprintf(line,sizeof(line),"%u × %u",gi.width,gi.height);
        text_draw(FONT_UI_BOLD,30,CX+174,130,line,INK);
        snprintf(line,sizeof(line),"%s · %u bpp",gi.driver,gi.bpp);
        label(14,CX+174,174,line,MUTED);
        label(12,CX+174,208,gi.hw_cursor ? "Аппаратный курсор включён" : "Программный курсор",MUTED);
        row(290,2,"Параметры экрана","Изменить разрешение и режим курсора");
        label(12,CX,390,"Настройки откроются в приложении Display.",MUTED);
    } else {
        unsigned t=ticks()/1000;
        card(CX,100,CW,94);
        gfx_round_rect(CX+18,118,56,56,13,UI_ACCENT);
        text_draw(FONT_UI_BOLD,22,CX+28,132,"M",C_WHITE);
        text_draw(FONT_UI_BOLD,22,CX+92,116,"MyOS",INK);
        snprintf(line,sizeof(line),"Время работы %u:%02u:%02u",t/3600,t/60%60,t%60);
        label(13,CX+92,151,line,MUTED);
        row(210,0,"Диспетчер задач","Процессы и использование ресурсов");
        row(298,2,"Терминал","Командная строка и системные утилиты");
        row(386,3,"О системе","Сведения о вашей операционной системе");
    }
    if(status[0]) {
        gfx_fill_circle(CX+4,H-22,3,UI_ACCENT);
        gfx_set_clip(CX+14,H-34,CW-14,28);
        label(12,CX+14,H-30,status,MUTED); gfx_no_clip();
    } else label(12,CX,H-30,page==1 ? "Выбор сохраняется · Стрелки влево/вправо — сменить фон" : "Alt+Shift или Win+Space — сменить язык",MUTED);
    win_update(win);
}
static void wallpaper_save(int reset) {
    if (reset) wall_path(0,path,sizeof(path));
    {
        if(path[0]!='/') { strcpy(status,"Укажите абсолютный путь, начиная с /"); return; }
        int fd=open(path,O_READ); if(fd<0) { strcpy(status,"Не удалось открыть изображение"); return; }
        int len=lseek(fd,0,SEEK_END); lseek(fd,0,SEEK_SET);
        if(len<=0 || len>8*1024*1024) { close(fd); strcpy(status,"Размер файла должен быть до 8 МБ"); return; }
        unsigned char *data=malloc(len); img im;
        int ok=data && read(fd,data,len)==len && img_decode(data,len,&im,2048)==0;
        close(fd); free(data);
        if(!ok) { strcpy(status,"Не удалось декодировать изображение"); return; } img_free(&im);
    }
    mkdir("/etc");
    int fd=open("/etc/wallpaper",O_WRITE);
    const char *value=path;
    int len=strlen(value);
    if(fd<0) { strcpy(status,"Не удалось сохранить настройки"); return; }
    int ok=write(fd,value,len)==len; close(fd);
    if(!ok) { strcpy(status,"Ошибка записи настроек"); return; }
    struct msg m; memset(&m,0,sizeof(m)); m.type=WM_WALLPAPER;
    if(msg_send(win_desktop_pid(),&m)<0) strcpy(status,"Сохранено; откройте рабочий стол заново");
    else strcpy(status,"Обои сохранены и применяются");

}
int main(void) {
    win=win_create("Панель управления",W,H); if(!win) return 1;
    struct net_info n; if(netinfo(&n)==0) ip_str(n.dns,dns);
    int fd=open("/etc/wallpaper",O_READ); if(fd>=0) { int k=read(fd,path,sizeof(path)-1); if(k>0) path[k]=0; close(fd); }
    if(!path[0]) wall_path(0,path,sizeof(path));
    draw(); /* Show the window before decoding gallery images. */
    load_thumbs();
    draw(); struct event e;
    while(win_event(&e,1000)) {
        if(e.type==EV_CLOSE || e.type==EV_QUIT || (e.type==EV_KEY && e.key==27)) break;
        if(e.type==EV_MOUSE_MOVE) { hover_x=e.x; hover_y=e.y; }
        if(e.type==EV_MOUSE_DOWN) {
            focus=0;
            for(int i=0;i<4;i++) if(ui_inside(e.x,e.y,12,100+i*52,166,44)) { page=i; status[0]=0; }
            if(page==0) {
                if(ui_inside(e.x,e.y,CX,FIELD_Y,CW,38)) focus=1;
                if(ui_inside(e.x,e.y,CX,ACTION_Y,180,38)) { unsigned ip; if(!ip_parse(dns,&ip) || !ip) strcpy(status,"Введите корректный IPv4 адрес DNS"); else strcpy(status,set_dns(ip)<0 ? "Ошибка смены DNS" : "DNS изменён до перезагрузки"); }
            } else if(page==1) {
                for(int i=0;i<6;i++) if(ui_inside(e.x,e.y,CX+(i%3)*180,100+(i/3)*120,162,108)) {
                    wall_path(i,path,sizeof(path)); wallpaper_save(0);
                }
                if(ui_inside(e.x,e.y,CX,FIELD_Y,CW,38)) focus=1;
                if(ui_inside(e.x,e.y,CX,ACTION_Y,180,38)) wallpaper_save(0);
                if(ui_inside(e.x,e.y,CX+194,ACTION_Y,180,38)) wallpaper_save(1);
            } else if(page==2 && ui_inside(e.x,e.y,CX,290,CW,76)) launch("/bin/display");
            else if(page==3) {
                if(ui_inside(e.x,e.y,CX,210,CW,76)) launch("/bin/taskmgr");
                if(ui_inside(e.x,e.y,CX,298,CW,76)) launch("/bin/terminal");
                if(ui_inside(e.x,e.y,CX,386,CW,76)) launch("/bin/about");
            }
        }
        if(e.type==EV_KEY && focus && page<2) {
            char *s=page==0 ? dns : path; int cap=page==0 ? sizeof(dns) : sizeof(path); int len=strlen(s);
            if(e.key==21) s[0]=0;
            else if(e.key==8 || e.key==127) { if(len) { do { len--; } while(len>0 && ((unsigned char)s[len]&192)==128); s[len]=0; } }
            else if((e.key>=32 && e.key<127) || (page==1 && e.key>=160 && e.key<65536 && (e.key<KEY_UP || e.key>KEY_INS))) {
                char u[4]; int count=utf8_put(u,e.key);
                if(len+count<cap) { memcpy(s+len,u,count); s[len+count]=0; }
            }
        }
        if(e.type==EV_KEY && !focus && page==1 && (e.key==KEY_LEFT || e.key==KEY_RIGHT)) {
            int selected=0;
            for(int i=0;i<6;i++) { char name[128]; wall_path(i,name,sizeof(name)); if(!strcmp(path,name)) selected=i; }
            selected=(selected+(e.key==KEY_RIGHT ? 1 : 5))%6;
            wall_path(selected,path,sizeof(path)); wallpaper_save(0);
        }
        if(e.type==EV_KEY && !focus && (e.key==KEY_DOWN || e.key==KEY_UP)) { page=(page+(e.key==KEY_DOWN ? 1 : 3))%4; status[0]=0; }
        if(e.type==EV_KEY && e.key=='\t' && page<2) focus=!focus;
        if(e.type==EV_KEY && (e.key=='\n' || e.key=='\r')) {
            if(page==0) { unsigned ip; if(!ip_parse(dns,&ip) || !ip) strcpy(status,"Введите корректный IPv4 адрес DNS"); else strcpy(status,set_dns(ip)<0 ? "Ошибка смены DNS" : "DNS изменён до перезагрузки"); }
            if(page==1) wallpaper_save(0);
        }
        while(waitpid(-1,0,WNOHANG)>0) {}
        draw();
    }
    for(int i=0;i<6;i++) img_free(&thumbs[i]);
    win_destroy(win); return 0;
}
