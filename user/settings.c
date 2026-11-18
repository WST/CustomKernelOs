#include "win.h"
#include "img.h"
#include "wifi_ui.h"

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

static char dns[16], path[128], status[100], wifi_pass[64], wifi_ssid[33];
/* Wi-Fi не удлиняет окно: на панели ноутбука 780 уже выше экрана.
 * Три вкладки делят прежнюю страницу, поле DNS остаётся на FIELD_Y. */
static int net_tab;
static const char *net_tabs[] = {"Проводная", "Wi-Fi", "Общие"};
#define NET_BAR_Y 96
#define NET_BAR_H 32
#define WIFI_TOP 140
#define WIFI_BTN_Y (WIFI_TOP + 32)
#define WIFI_ROW_Y (WIFI_TOP + 80)
#define WIFI_ROW_H 24
#define WIFI_ROWS 6
#define WIFI_PASS_Y (WIFI_ROW_Y + WIFI_ROWS * WIFI_ROW_H + 30)
#define WIFI_ACT_Y (WIFI_PASS_Y + 46)
#define WIFI_BAR_W 14
/* Первая видимая строка. Выбор держится по имени сети, поэтому смена порядка его не теряет. */
static int wifi_top;
static int wifi_rows(void) { int n=(int)wifi_view->count; return n<WIFI_ROWS ? n : WIFI_ROWS; }
static int wifi_list_w(void) { return (int)wifi_view->count>WIFI_ROWS ? CW-WIFI_BAR_W-6 : CW; }
static void wifi_scroll(int delta) {
    int max=(int)wifi_view->count-WIFI_ROWS;
    wifi_top+=delta;
    if(wifi_top>max) wifi_top=max;
    if(wifi_top<0) wifi_top=0;
}
static int wifi_selected(void) {
    for(int i=0;i<(int)wifi_view->count;i++) if(wifi_ssid[0] && !strcmp(wifi_view->ap[i].ssid,wifi_ssid)) return i;
    return -1;
}
/* Стрелки двигают выбор по списку и прокручивают его так, чтобы выбранная строка была видна. */
static void wifi_step(int delta) {
    int n=(int)wifi_view->count;
    if(!n) return;
    int i=wifi_selected();
    i=i<0 ? (delta>0 ? 0 : n-1) : i+delta;
    if(i<0) i=0;
    if(i>=n) i=n-1;
    snprintf(wifi_ssid,sizeof(wifi_ssid),"%s",wifi_view->ap[i].ssid);
    if(i<wifi_top) wifi_top=i;
    if(i>=wifi_top+WIFI_ROWS) wifi_top=i-WIFI_ROWS+1;
}
static int wlan_present(void) {
    struct if_info inf;
    for (int i = 0; i < 4; i++) {
        if (ifctl(i, &inf) < 0) break;
        if (inf.kind == 2) return 1;
    }
    return 0;
}
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
static int hover_region(void) {
    for(int i=0;i<4;i++)if(i!=page && over(12,102+i*52,166,44))return i+1;
    if(page==0) {
        int tw=CW/3;
        for(int i=0;i<3;i++) if(i!=net_tab && over(CX+i*tw,NET_BAR_Y,tw,NET_BAR_H)) return 40+i;
        if(net_tab==1 && wlan_present() && wifi_link()) {
            if(over(CX,WIFI_BTN_Y,160,38)) return 11;
            if(over(CX+172,WIFI_BTN_Y,160,38)) return 12;
            for(int i=0;i<wifi_rows();i++) if(over(CX,WIFI_ROW_Y+i*WIFI_ROW_H,wifi_list_w(),WIFI_ROW_H)) return 15+wifi_top+i;
            if(over(CX,WIFI_ACT_Y,140,38)) return 13;
            if(over(CX+150,WIFI_ACT_Y,120,38)) return 14;
        }
    }
    if(((page==0 && net_tab==0) || page==1) && over(CX,ACTION_Y,180,38))return 10;
    if(page==1) {
        for(int i=0;i<6;i++)if(over(CX+i%3*180,100+i/3*120,162,108))return 20+i;
        if(over(CX+194,ACTION_Y,180,38))return 26;
    }
    if(page==2 && over(CX,290,CW,76))return 30;
    if(page==3)for(int i=0;i<3;i++)if(over(CX,210+i*88,CW,76))return 31+i;
    return 0;
}
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
static void apply_dns(void) {
    unsigned ip;
    if(!ip_parse(dns,&ip) || !ip) strcpy(status,"Введите корректный IPv4 адрес DNS");
    else strcpy(status,set_dns(ip)<0 ? "Ошибка смены DNS" : "DNS изменён до перезагрузки");
}
static void net_bar(void) {
    int tw=CW/3;
    gfx_round_frame(CX,NET_BAR_Y,CW,NET_BAR_H,8,BORDER,C_WHITE);
    for(int i=0;i<3;i++) {
        int x=CX+i*tw, on=i==net_tab;
        if(on) gfx_round_rect(x+2,NET_BAR_Y+2,tw-4,NET_BAR_H-4,6,RGB(251,236,228));
        else if(over(x,NET_BAR_Y,tw,NET_BAR_H)) gfx_round_rect(x+2,NET_BAR_Y+2,tw-4,NET_BAR_H-4,6,RGB(244,246,248));
        int width=text_width(FONT_UI_MEDIUM,13,net_tabs[i]);
        text_draw(on ? FONT_UI_MEDIUM : FONT_UI,13,x+(tw-width)/2,NET_BAR_Y+8,net_tabs[i],on ? UI_ACCENT : INK);
    }
}
static void draw_wired(char *line,char *a,char *b) {
    struct net_info n; memset(&n,0,sizeof(n)); netinfo(&n);
    card(CX,140,CW,68);
    gfx_round_rect(CX+16,152,44,44,10,RGB(235,247,239));
    icon(0,CX+27,164,RGB(55,137,88));
    text_draw(FONT_UI_MEDIUM,16,CX+76,150,n.up ? "Intel e1000 · Проводная сеть" : "Адаптер не найден",INK);
    snprintf(line,160,"eth0 · %02x:%02x:%02x:%02x:%02x:%02x",n.mac[0],n.mac[1],n.mac[2],n.mac[3],n.mac[4],n.mac[5]);
    label(12,CX+76,176,line,MUTED);
    gfx_fill_circle(CX+CW-113,173,4,n.up && n.ip ? RGB(61,156,100) : MUTED);
    label(12,CX+CW-102,166,n.up && n.ip ? "Подключено" : "Нет адреса",MUTED);
    card(CX,216,CW,116);
    detail(CX+20,228,"IPv4-адрес",ip_str(n.ip,a));
    detail(CX+280,228,"Маска подсети",ip_str(n.mask,b));
    detail(CX+20,276,"Шлюз",ip_str(n.gw,a));
    snprintf(line,160,"%s · RX %u / TX %u",n.dhcp ? "DHCP" : "Static",n.rx_packets,n.tx_packets);
    detail(CX+280,276,"Получение адреса / пакеты",line);
    snprintf(line,160,"DNS-сервер · сейчас %s",ip_str(n.dns,a));
    label(13,CX,348,line,MUTED);
    field(FIELD_Y,dns,focus==1);
    button(CX,ACTION_Y,180,"Применить DNS",true);
    label(12,CX+198,ACTION_Y+12,"Действует до перезагрузки",MUTED);
}
static void draw_wifi(char *line) {
    if(!(wlan_present() && wifi_link())) {
        card(CX,140,CW,76);
        text_draw(FONT_UI_MEDIUM,16,CX+20,156,wlan_present() ? "Служба Wi-Fi недоступна" : "Адаптер Wi-Fi не найден",INK);
        label(12,CX+20,184,wlan_present() ? "wifid не опубликовал состояние" : "В этой машине нет wlan0",MUTED);
        return;
    }
    label(13,CX,WIFI_TOP,wifi_view->blocked ? "Радио выключено" : "Wi-Fi",INK);
    snprintf(line,160,"%s",wifi_view->ssid[0] ? wifi_view->ssid : "не подключено");
    label(12,CX+180,WIFI_TOP,line,MUTED);
    button(CX,WIFI_BTN_Y,160,"Сканировать",false);
    button(CX+172,WIFI_BTN_Y,160,wifi_view->blocked ? "Включить" : "Выключить",false);
    wifi_scroll(0);
    int rows=wifi_rows(), lw=wifi_list_w(), count=(int)wifi_view->count;
    if(!rows) label(12,CX,WIFI_ROW_Y,"Список сетей пуст",MUTED);
    for(int i=0;i<rows;i++) {
        struct wifi_ap *ap=&wifi_view->ap[wifi_top+i];
        int ry=WIFI_ROW_Y+i*WIFI_ROW_H;
        int on=!strcmp(ap->ssid,wifi_ssid);
        if(on || over(CX,ry,lw,WIFI_ROW_H)) gfx_round_rect(CX,ry-2,lw,WIFI_ROW_H,6,on ? RGB(251,236,228) : RGB(244,246,248));
        gfx_set_clip(CX+8,ry-2,lw-120,WIFI_ROW_H);
        label(12,CX+8,ry,ap->ssid[0] ? ap->ssid : "(скрытая)",on ? UI_ACCENT : INK);
        gfx_no_clip();
        snprintf(line,160,"%s%s%d/4",ap->saved ? "сохр. " : "",ap->secured ? "замок " : "",ap->bars);
        label(12,CX+lw-8-text_width(FONT_UI,12,line),ry,line,MUTED);
    }
    if(count>WIFI_ROWS) {
        int tx=CX+CW-WIFI_BAR_W, th=WIFI_ROWS*WIFI_ROW_H;
        int thumb=th*WIFI_ROWS/count, ty=WIFI_ROW_Y-2+(th-thumb)*wifi_top/(count-WIFI_ROWS);
        gfx_round_rect(tx,WIFI_ROW_Y-2,WIFI_BAR_W,th,6,RGB(239,241,244));
        gfx_round_rect(tx+2,ty,WIFI_BAR_W-4,thumb,5,RGB(177,184,195));
    }
    int sel=wifi_selected();
    struct wifi_ap *pick=sel>=0 ? &wifi_view->ap[sel] : NULL;
    if(!pick) snprintf(line,160,"Выберите сеть в списке");
    else if(pick->secured) snprintf(line,160,"Пароль сети «%s»",pick->ssid);
    else snprintf(line,160,"«%s» — открытая сеть, пароль не нужен",pick->ssid);
    label(12,CX,WIFI_PASS_Y-20,line,MUTED);
    field(WIFI_PASS_Y,wifi_pass,focus==2);
    if(!wifi_pass[0] && focus!=2) label(14,CX+12,WIFI_PASS_Y+10,pick && !pick->secured ? "не требуется" : "пароль",MUTED);
    button(CX,WIFI_ACT_Y,140,"Подключить",true);
    button(CX+150,WIFI_ACT_Y,120,"Забыть",false);
    if(wifi_view->error[0]) label(12,CX+290,WIFI_ACT_Y+12,wifi_view->error,MUTED);
}
static void draw_general(char *line,char *a,char *b) {
    struct net_info n; memset(&n,0,sizeof(n)); netinfo(&n);
    card(CX,140,CW,148);
    text_draw(FONT_UI_MEDIUM,16,CX+20,156,"Маршрут по умолчанию",INK);
    detail(CX+20,188,"IPv4-адрес",ip_str(n.ip,a));
    detail(CX+280,188,"Шлюз",ip_str(n.gw,b));
    detail(CX+20,244,"DNS",ip_str(n.dns,a));
    label(12,CX,304,"Интерфейсы",MUTED);
    int y=328, shown=0;
    for(int i=0;i<4 && y+56<=470;i++) {
        struct if_info inf;
        if(ifctl(i,&inf)<0) break;
        card(CX,y,CW,56);
        snprintf(line,160,"%s · предпочтение %u",inf.name,inf.prefer);
        text_draw(FONT_UI_MEDIUM,15,CX+20,y+8,line,INK);
        snprintf(line,160,"%s · %s",ip_str(inf.ip,a),inf.up && inf.ip ? "подключено" : "нет адреса");
        label(12,CX+20,y+30,line,MUTED);
        y+=64; shown++;
    }
    if(!shown) label(12,CX,328,"Интерфейсов нет",MUTED);
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
        net_bar();
        if(net_tab==0) draw_wired(line,a,b);
        else if(net_tab==1) draw_wifi(line);
        else draw_general(line,a,b);
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
    } else if(page==1) label(12,CX,H-30,"Выбор сохраняется · Стрелки влево/вправо — сменить фон",MUTED);
    else if(page==0) label(12,CX,H-30,"Влево/вправо — вкладка сети",MUTED);
    else label(12,CX,H-30,"Alt+Shift или Win+Space — сменить язык",MUTED);
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
        if(e.type==EV_MOUSE_MOVE) {
            int before=hover_region();hover_x=e.x;hover_y=e.y;
            if(before==hover_region())continue;
        }
        if(e.type==EV_MOUSE_WHEEL && page==0 && net_tab==1 && wlan_present() && wifi_link() &&
           ui_inside(e.x,e.y,CX,WIFI_ROW_Y-2,CW,WIFI_ROWS*WIFI_ROW_H)) {
            int before=wifi_top;
            wifi_scroll(-e.wheel);
            if(before!=wifi_top) draw();
            continue;
        }
        if(e.type==EV_MOUSE_UP || e.type==EV_FOCUS || e.type==EV_MOUSE_WHEEL)continue;
        if(e.type==EV_TIMEOUT && page!=0 && page!=3)continue;
        if(e.type==EV_MOUSE_DOWN) {
            focus=0;
            for(int i=0;i<4;i++) if(ui_inside(e.x,e.y,12,100+i*52,166,44)) { page=i; status[0]=0; }
            if(page==0) {
                int tw=CW/3, switched=0;
                for(int i=0;i<3;i++) if(ui_inside(e.x,e.y,CX+i*tw,NET_BAR_Y,tw,NET_BAR_H)) { net_tab=i; status[0]=0; switched=1; }
                if(!switched && net_tab==0) {
                    if(ui_inside(e.x,e.y,CX,FIELD_Y,CW,38)) focus=1;
                    if(ui_inside(e.x,e.y,CX,ACTION_Y,180,38)) apply_dns();
                } else if(!switched && net_tab==1 && wlan_present() && wifi_link()) {
                    if(ui_inside(e.x,e.y,CX,WIFI_BTN_Y,160,38)) wifi_order(WIFI_SCAN, 0, 0, 0);
                    if(ui_inside(e.x,e.y,CX+172,WIFI_BTN_Y,160,38)) wifi_order(WIFI_RADIO, 0, 0, wifi_view->blocked ? 1u : 0u);
                    wifi_scroll(0);
                    for(int i=0;i<wifi_rows();i++) if(ui_inside(e.x,e.y,CX,WIFI_ROW_Y+i*WIFI_ROW_H,wifi_list_w(),WIFI_ROW_H)) {
                        struct wifi_ap *ap=&wifi_view->ap[wifi_top+i];
                        snprintf(wifi_ssid, sizeof(wifi_ssid), "%s", ap->ssid);
                        if(!ap->secured) wifi_order(WIFI_CONNECT, wifi_ssid, "", 0);
                        else focus=2;
                    }
                    if((int)wifi_view->count>WIFI_ROWS && ui_inside(e.x,e.y,CX+CW-WIFI_BAR_W,WIFI_ROW_Y-2,WIFI_BAR_W,WIFI_ROWS*WIFI_ROW_H))
                        wifi_scroll(e.y<WIFI_ROW_Y-2+WIFI_ROWS*WIFI_ROW_H/2 ? -WIFI_ROWS : WIFI_ROWS);
                    if(ui_inside(e.x,e.y,CX,WIFI_PASS_Y,CW,38)) focus=2;
                    if(ui_inside(e.x,e.y,CX,WIFI_ACT_Y,140,38) && wifi_ssid[0]) wifi_order(WIFI_CONNECT, wifi_ssid, wifi_pass, 0);
                    if(ui_inside(e.x,e.y,CX+150,WIFI_ACT_Y,120,38) && wifi_ssid[0]) wifi_order(WIFI_FORGET, wifi_ssid, 0, 0);
                }
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
            char *s=page==0 ? (focus==2 ? wifi_pass : dns) : path;
            int cap=page==0 ? (focus==2 ? (int)sizeof(wifi_pass) : (int)sizeof(dns)) : (int)sizeof(path);
            int len=strlen(s);
            if(e.key==21) s[0]=0;
            else if(e.key==8 || e.key==127) { if(len) { do { len--; } while(len>0 && ((unsigned char)s[len]&192)==128); s[len]=0; } }
            else if((e.key>=32 && e.key<127) || (page==1 && e.key>=160 && e.key<65536 && (e.key<KEY_UP || e.key>KEY_INS))) {
                char u[4]; int count=utf8_put(u,e.key);
                if(len+count<cap) { memcpy(s+len,u,count); s[len+count]=0; }
            }
        }
        if(e.type==EV_KEY && !focus && page==0 && (e.key==KEY_LEFT || e.key==KEY_RIGHT)) {
            net_tab=(net_tab+(e.key==KEY_RIGHT ? 1 : 2))%3; status[0]=0;
        }
        if(e.type==EV_KEY && !focus && page==1 && (e.key==KEY_LEFT || e.key==KEY_RIGHT)) {
            int selected=0;
            for(int i=0;i<6;i++) { char name[128]; wall_path(i,name,sizeof(name)); if(!strcmp(path,name)) selected=i; }
            selected=(selected+(e.key==KEY_RIGHT ? 1 : 5))%6;
            wall_path(selected,path,sizeof(path)); wallpaper_save(0);
        }
        if(e.type==EV_KEY && (e.key==KEY_DOWN || e.key==KEY_UP) && page==0 && net_tab==1 && wlan_present() && wifi_link())
            wifi_step(e.key==KEY_DOWN ? 1 : -1);
        else if(e.type==EV_KEY && !focus && (e.key==KEY_DOWN || e.key==KEY_UP)) { page=(page+(e.key==KEY_DOWN ? 1 : 3))%4; status[0]=0; }
        if(e.type==EV_KEY && e.key=='\t' && page<2) {
            if(page==0 && net_tab==1) focus=focus==2 ? 0 : 2;
            else if(page==0 && net_tab!=0) focus=0;
            else focus=!focus;
        }
        if(e.type==EV_KEY && (e.key=='\n' || e.key=='\r')) {
            if(page==0 && net_tab==0) apply_dns();
            else if(page==0 && net_tab==1 && wifi_ssid[0] && wlan_present() && wifi_link()) wifi_order(WIFI_CONNECT, wifi_ssid, wifi_pass, 0);
            if(page==1) wallpaper_save(0);
        }
        while(waitpid(-1,0,WNOHANG)>0) {}
        draw();
    }
    for(int i=0;i<6;i++) img_free(&thumbs[i]);
    win_destroy(win); return 0;
}
