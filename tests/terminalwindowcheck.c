#include "win.h"
static int child;
static color_t *pixels;
static int receive(int type) {
    unsigned start=ticks();
    while (ticks()-start<6000) {
        struct msg m;
        if (msg_recv(&m,100)<0) continue;
        if (m.from!=child) continue;
        if (m.type==WM_CREATE) {
            pixels=shm_map(m.a);
            struct msg reply={0}; reply.type=WM_CREATED; reply.a=0;
            msg_send(child,&reply);
        }
        if (m.type==WM_SETBUF) {
            color_t *next=shm_map(m.b);
            if (!next) return -2;
            if (pixels) shm_unmap(pixels);
            pixels=next;
        }
        if (m.type==type) return 0;
    }
    struct proc_info p[32]; int n=ps(p,32);
    for(int i=0;i<n;i++) if(p[i].pid==(unsigned)child)
        { int status=-1; waitpid(child,&status,WNOHANG); printf("WINDOW FAIL state=%u status=%d step=%d\n",p[i].state,status,type); }
    return -1;
}
static int check_terminal(void) {
    if (service_register("desktop")<0) return 1;
    child=fork();
    if (!child) { char *args[]={"terminal",NULL}; exec("/bin/terminal",args); return 2; }
    if (receive(WM_RESIZABLE)<0 || receive(WM_UPDATE)<0) return 3;
    int sizes[][2]={{1000,700},{1848,1120},{648,408},{1200,760},{1848,1120},{648,408}};
    for(int i=0;i<6;i++) {
        if (i==3) {
            const char *login="\rhtop123\r";
            for (int j=0;login[j];j++) {
                struct msg key={0};key.type=WM_KEY;key.a=0;key.b=login[j];
                msg_send(child,&key);sleep_ms(30);
            }
            sleep_ms(300);
        }
        printf("WINDOW resize=%dx%d\n",sizes[i][0],sizes[i][1]);
        struct msg m={0};m.type=WM_RESIZE;m.a=0;m.b=sizes[i][0];m.c=sizes[i][1];
        msg_send(child,&m);
        if(receive(WM_SETBUF)<0 || receive(WM_UPDATE)<0) return 4;
        sleep_ms(100);
    }
    /* Verify login and command execution still work after all resizes. */
    const char *command="echo resizealive > /tmp/resize-alive\r";
    unlink("/tmp/resize-alive");
    for (int j=0;command[j];j++) {
        struct msg key={0};key.type=WM_KEY;key.a=0;key.b=command[j];
        msg_send(child,&key);sleep_ms(20);
        struct msg update; while(msg_recv(&update,0)==0) {}
    }
    int fd=-1,got=-1;char output[32]={0};
    /* Redirection creates the file before echo writes it. Wait for the data,
     * rather than treating the first successful open as command completion. */
    for(int j=0;j<30;j++) {
        sleep_ms(100);fd=open("/tmp/resize-alive",O_READ);
        if(fd<0)continue;
        memset(output,0,sizeof(output));got=read(fd,output,sizeof(output)-1);close(fd);
        if(got==12 && !memcmp(output,"resizealive\n",12))break;
    }
    if(got!=12 || memcmp(output,"resizealive\n",12)){printf("WINDOW command output bytes=%d text=[%s]\n",got,output);return 6;}
    unlink("/tmp/resize-alive");
    struct msg m={0};m.type=WM_CLOSE;m.a=0;msg_send(child,&m);
    int status=-1;
    if(waitpid(child,&status,0)!=child || status) return 5;
    puts("TERMINAL WINDOW PASS");return 0;
}

int main(void){int status=check_terminal();if(status)printf("TERMINAL WINDOW FAIL code=%d\n",status);return status;}
