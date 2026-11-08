#include "win.h"
#include "img.h"

#define W 600
#define H 400
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
        int x=190+(i%3)*130, y=62+(i/3)*92;
        char name[128]; wall_path(i,name,sizeof(name));
        gfx_fill_rect(x-2,y-2,124,86,!strcmp(path,name) ? UI_ACCENT : UI_SHADOW);
        gfx_fill_rect(x,y,120,64,RGB(40,30,40));
        img *im=&thumbs[i];
        if(im->px) {
            int cw=im->w,ch=im->h;
            if(im->w*64>im->h*120) cw=im->h*120/64;
            else ch=im->w*64/120;
            if(cw<1) cw=1;
            if(ch<1) ch=1;
            for(int yy=0;yy<64;yy++) for(int xx=0;xx<120;xx++)
                gfx_pixel(x+xx,y+yy,im->px[((im->h-ch)/2+yy*ch/64)*im->w+(im->w-cw)/2+xx*cw/120]);
        }
        ui_text_center(x,y+64,120,18,wall_names[i],!strcmp(path,name) ? C_WHITE : UI_TEXT);
    }
}

static int page, focus;
static const char *tabs[] = {"Сеть", "Рабочий стол", "Экран", "Система"};

static void launch(const char *name) {
    if (fork() == 0) { char *args[] = {(char *)name, 0}; exec(name, args); exit(1); }
}
static void field(int y, const char *s, int selected) {
    ui_sunken(190, y, 386, 30, C_WHITE);
    gfx_set_clip(196, y + 2, 374, 26);
    ui_text(198, y + 6, s, selected ? UI_ACCENT : UI_TEXT);
    gfx_no_clip();
}
static void draw(void) {
    char line[160], a[16], b[16];
    gfx_clear(UI_FACE);
    gfx_fill_rect(0, 0, 168, H, RGB(238,235,232));
    ui_text(16, 18, "Настройки", UI_TEXT);
    for (int i=0;i<4;i++) {
        if (i==page) gfx_fill_rect(8, 56+i*46, 152, 38, UI_SELECT);
        ui_text(18, 66+i*46, tabs[i], i==page ? UI_ACCENT : UI_TEXT);
    }
    text_draw(FONT_UI_BOLD, 22, 190, 18, tabs[page], UI_TEXT);
    if (page==0) {
        struct net_info n; memset(&n,0,sizeof(n)); netinfo(&n);
        ui_text(190,62,n.up ? "eth0 · Intel e1000" : "Сетевой адаптер не найден",UI_TEXT);
        snprintf(line,sizeof(line),"MAC %02x:%02x:%02x:%02x:%02x:%02x",n.mac[0],n.mac[1],n.mac[2],n.mac[3],n.mac[4],n.mac[5]); ui_text(190,90,line,UI_DARK);
        snprintf(line,sizeof(line),"IP %s  (%s)",ip_str(n.ip,a),n.dhcp ? "DHCP" : "static"); ui_text(190,118,line,UI_TEXT);
        snprintf(line,sizeof(line),"Маска %s",ip_str(n.mask,a)); ui_text(190,144,line,UI_TEXT);
        snprintf(line,sizeof(line),"Шлюз %s",ip_str(n.gw,a)); ui_text(190,170,line,UI_TEXT);
        snprintf(line,sizeof(line),"RX %u / TX %u пакетов",n.rx_packets,n.tx_packets); ui_text(190,196,line,UI_DARK);
        snprintf(line,sizeof(line),"DNS сейчас: %s",ip_str(n.dns,b)); ui_text(190,222,line,UI_TEXT);
        field(250,dns,focus);
        ui_button_accent(190,294,180,32,"Применить DNS",false);
    } else if (page==1) {
        gallery();
        ui_text(190,246,"Или свой файл: PNG, JPEG, GIF, BMP",UI_DARK);
        field(270,path,focus);
        ui_button_accent(190,312,180,34,"Установить",false);
        ui_button(382,312,194,34,"По умолчанию",false);
    } else if (page==2) {
        struct gfx_info2 gi; gfx_info2(&gi);
        snprintf(line,sizeof(line),"%s · %ux%u · %u bpp",gi.driver,gi.width,gi.height,gi.bpp); ui_text(190,70,line,UI_TEXT);
        ui_text(190,110,gi.hw_cursor ? "Аппаратный курсор включён" : "Аппаратный курсор выключен",UI_TEXT);
        ui_text(190,152,"Разрешение и курсор настраиваются в Display.",UI_DARK);
        ui_button_accent(190,208,240,34,"Настроить экран",false);
    } else {
        unsigned s=ticks()/1000;
        snprintf(line,sizeof(line),"Время работы: %u:%02u:%02u",s/3600,s/60%60,s%60); ui_text(190,70,line,UI_TEXT);
        ui_text(190,110,"Alt+Shift / Win+Space — смена языка",UI_TEXT);
        ui_text(190,144,"Настройки и утилиты системы",UI_DARK);
        ui_button(190,190,240,34,"Диспетчер задач",false);
        ui_button(190,238,240,34,"Терминал",false);
        ui_button(190,286,240,34,"О системе",false);
    }
    gfx_set_clip(182,354,410,40); ui_text(190,366,status,UI_DARK); gfx_no_clip();
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
    load_thumbs();
    draw(); struct event e;
    while(win_event(&e,1000)) {
        if(e.type==EV_CLOSE || e.type==EV_QUIT || (e.type==EV_KEY && e.key==27)) break;
        if(e.type==EV_MOUSE_DOWN) {
            focus=0;
            for(int i=0;i<4;i++) if(ui_inside(e.x,e.y,8,56+i*46,152,38)) { page=i; status[0]=0; }
            if(page==0) {
                if(ui_inside(e.x,e.y,190,250,386,30)) focus=1;
                if(ui_inside(e.x,e.y,190,294,180,32)) { unsigned ip; if(!ip_parse(dns,&ip) || !ip) strcpy(status,"Введите корректный IPv4 адрес DNS"); else strcpy(status,set_dns(ip)<0 ? "Ошибка смены DNS" : "DNS изменён до перезагрузки"); }
            } else if(page==1) {
                for(int i=0;i<6;i++) if(ui_inside(e.x,e.y,190+(i%3)*130,62+(i/3)*92,120,84)) {
                    wall_path(i,path,sizeof(path)); wallpaper_save(0);
                }
                if(ui_inside(e.x,e.y,190,270,386,30)) focus=1;
                if(ui_inside(e.x,e.y,190,312,180,34)) wallpaper_save(0);
                if(ui_inside(e.x,e.y,382,312,194,34)) wallpaper_save(1);
            } else if(page==2 && ui_inside(e.x,e.y,190,208,240,34)) launch("/bin/display");
            else if(page==3) {
                if(ui_inside(e.x,e.y,190,190,240,34)) launch("/bin/taskmgr");
                if(ui_inside(e.x,e.y,190,238,240,34)) launch("/bin/terminal");
                if(ui_inside(e.x,e.y,190,286,240,34)) launch("/bin/about");
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
