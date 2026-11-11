/* Real win.c producer and desktop.c consumer, with deliberately delayed IPC. */
#define main desktop_program_main
#include "../user/desktop.c"
#undef main

#define SAMPLE 800
#define NEXT 801
#define RED RGB(170,40,80)
#define GREEN RGB(20,190,70)
#define BLUE RGB(30,80,210)

static void require(int ok,const char *what) {
    if(!ok) { printf("FRAMECHECK FAIL %s\n",what); exit(1); }
}
static void stage(int owner,int number) {
    struct msg m={0}; m.type=SAMPLE; m.a=number;
    require(msg_send(owner,&m)==0,"sample send");
    unsigned start=ticks();
    while(ticks()-start<10000) {
        if(msg_recv(&m,100)==0 && m.from==owner && m.type==NEXT) return;
    }
    require(false,"sample acknowledgement");
}
static int producer(int owner) {
    window *w=win_create("Atomic window frames",400,240);
    require(w && w->frame && w->pixels!=w->shared,"buffer negotiation");
    gfx_clear(RED); win_update(w);
    /* The next repaint starts while the completed update is still queued. */
    gfx_clear(0);
    require(w->shared[0]==RED,"private draw does not clear publication");
    stage(owner,1);

    gfx_clear(RED); gfx_fill_rect(23,17,16,8,GREEN);
    win_update_rect(w,23,17,16,8);
    gfx_clear(0);
    stage(owner,2);

    /* Publish twice without waiting in the app: the second publication must
     * wait for the desktop's shared-memory acknowledgement of the first. */
    gfx_clear(RED); win_update(w);
    struct msg m={0}; m.type=SAMPLE; m.a=3;
    require(msg_send(owner,&m)==0,"in-flight sample");
    gfx_clear(BLUE); win_update(w);
    gfx_clear(0);
    stage(owner,4);

    struct event e;
    do { require(win_event(&e,2000),"resize event"); } while(e.type!=EV_RESIZE);
    require(w->w==520 && w->h==300,"resize dimensions");
    gfx_clear(GREEN); win_update(w); gfx_clear(0);
    stage(owner,5);

    gfx_clear(BLUE); win_update(w); gfx_clear(0);
    stage(owner,6);
    /* A slow/dead consumer must never let the next update overwrite an
     * in-flight publication, nor lose damage when that consumer recovers. */
    gfx_clear(RED); win_update(w);
    gfx_clear(BLUE); win_update_rect(w,10,10,1,1);
    require(w->full_update && w->shared[0]==RED,"timeout preserves publication");
    stage(owner,7);
    win_update_rect(w,0,0,1,1);
    require(!w->full_update,"timeout recovery publication");
    gfx_clear(0); stage(owner,8);
    win_destroy(w);
    return 0;
}
static void verify_pixels(int stage_number) {
    struct win *w=&wins[0];
    color_t *actual=malloc(SW*SH*4);
    require(actual && gfx_readback(actual)==0,"GPU readback");
    int different=0;
    for(int y=0;y<w->bh;y++) for(int x=0;x<w->bw;x++) {
        color_t expected=stage_number>=5 ? (stage_number==5 ? GREEN:stage_number==7 ? RED:BLUE) :
            stage_number==4 ? BLUE : stage_number==2 && x>=23 && x<39 && y>=17 && y<25 ? GREEN:RED;
        if((actual[(w->y+HEAD_H+y)*SW+w->x+x]&0xffffff)!=expected) different++;
    }
    free(actual);
    printf("FRAMECHECK stage %d differences %d\n",stage_number,different);
    require(!different,"unfinished repaint became visible");
}
int main(int argc,char **argv) {
    if(argc>2 && !strcmp(argv[1],"producer")) return producer(atoi(argv[2]));
    require(gfx_open()==0,"open");
    require(gfx_set_mode(argc>1 ? 2560:1024,argc>1 ? 1600:768)==0,"mode"); gfx_check_mode();
    SW=gfx_w; SH=gfx_h; back=gfx_buf;
    gpu_reset(); require(gpu_scene,"GPU compositor");
    wallpaper=malloc(SW*SH*4); require(wallpaper!=NULL,"wallpaper");
    for(int i=0;i<SW*SH;i++) wallpaper[i]=RGB(35,28,52);
    load_apps(); dock_build(); update_clock(); mouse_x=900; mouse_y=700;
    require(service_register("desktop")==0,"desktop registration");
    int owner=getpid(); char owner_text[16]; snprintf(owner_text,sizeof(owner_text),"%d",owner);
    char *args[]={"windowframecheck","producer",owner_text,NULL};
    int child=spawn("/bin/windowframecheck",args,-1); require(child>=0,"spawn");
    struct msg pending_update={0};
    bool have_update=false,done=false;
    unsigned last=ticks();
    while(!done && ticks()-last<10000) {
        struct msg m;
        if(msg_recv(&m,100)<0) continue;
        if(m.from!=child) continue;
        last=ticks();
        if(m.type==WM_UPDATE) { require(!have_update,"one in-flight publication"); pending_update=m; have_update=true; continue; }
        if(m.type==SAMPLE) {
            require(have_update,"completed update queued");
            require(wins[0].frame->submitted!=wins[0].frame->consumed,"publication held until capture");
            /* A minimized window must release its publication too. */
            if(m.a==6) wins[0].minimized=true;
            handle_msg(&pending_update); have_update=false;
            require(wins[0].frame->submitted==wins[0].frame->consumed,"capture acknowledgement");
            if(m.a==6) { wins[0].minimized=false; dirty_win(&wins[0]); }
            flush(); verify_pixels(m.a);
            if(m.a==3) continue; /* producer is waiting to publish the next frame */
            struct msg reply={0}; reply.type=NEXT;
            require(msg_send(child,&reply)==0,"next sample");
            if(m.a==4) {
                dirty_win(&wins[0]); wins[0].w=520; wins[0].h=300; dirty_win(&wins[0]);
                struct msg resize={0}; resize.type=WM_RESIZE; resize.a=0; resize.b=520; resize.c=300;
                require(msg_send(child,&resize)==0,"resize request");
            }
        } else {
            if(m.type==WM_DESTROY) done=true;
            handle_msg(&m);
        }
    }
    require(done,"producer completed");
    int status=-1; require(waitpid(child,&status,0)==child && status==0,"producer exit");
    require(!wins[0].used,"window released");
    free(wallpaper); gfx_close(); printf("FRAMECHECK PASS\n");
    return 0;
}
