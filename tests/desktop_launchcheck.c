#define spawn observed_spawn
#define msg_recv observed_recv
#define gfx_flip_rect observed_flip
#define main desktop_main
#include "../user/desktop.c"
#undef main
#undef spawn
#undef msg_recv
#undef gfx_flip_rect
int spawn(const char *,char **,int);
int msg_recv(struct msg *,int);
void gfx_flip_rect(int,int,int,int);
static int pids[3],created[3],count;
static bool requested,reported;
int observed_spawn(const char *path,char **argv,int input) {
    unsigned start=ticks();int pid=spawn(path,argv,input);
    printf("GUI SPAWN %s pid=%d ms=%u\n",path,pid,ticks()-start);
    for(int i=0;i<3;i++) if(!pids[i]) {pids[i]=pid;break;}
    return pid;
}
int observed_recv(struct msg *m,int timeout) {
    if(!requested) {
        requested=true;
        launch(app_by_prog("calc"));launch(app_by_prog("notepad"));launch(app_by_prog("settings"));
    }
    int result=msg_recv(m,timeout);
    if(result==0 && m->type==WM_CREATE)
        for(int i=0;i<3;i++) if(m->from==pids[i] && !created[i]) {created[i]=1;count++;}
    return result;
}
void observed_flip(int x,int y,int w,int h) {
    gfx_flip_rect(x,y,w,h);
    if(count==3 && !reported) {reported=true;puts("GUI LAUNCH PASS");}
}
int main(void) {
    if(account(0,NULL,NULL)>0 && account(3,"root","launch123")<0) return 1;
    if(account(1,"root","launch123")<0) return 2;
    /* Real compositor plus 64 MiB heap: launch must not copy these pages. */
    volatile char *extra=malloc(64u*1024*1024);
    if(!extra) return 3;
    for(unsigned i=0;i<64u*1024*1024;i+=4096) extra[i]=1;
    return desktop_main();
}
