#define main terminal_application_main
#include "../user/terminal.c"
#undef main
struct report { int key, w, h; };
int main(int argc, char **argv) {
    if (argc>1) return term_cols()==200 && term_rows()==60 ? 0 : 20;
    int fds[2], ready[2], results[2];
    if (pty_open(fds)<0 || pipe(ready)<0 || pipe(results)<0) return 1;
    master=fds[0]; set_grid(80,25);
    at(0,0)->ch='A';
    int pid=fork();
    if (pid<0) return 2;
    if (!pid) {
        close(fds[0]); dup2(fds[1],STDIN); close(fds[1]);
        close(ready[0]); close(results[0]);
        ttymode(TTY_RAW|TTY_NONBLOCK);
        readkey(); /* Establish initial dimensions without waiting for input. */
        ttymode(TTY_RAW);
        for (int i=0;i<4;i++) {
            write(ready[1],"r",1);
            int key;
            do { key=readkey(); } while(key<0);
            struct report result={key,term_cols(),term_rows()};
            write(results[1],&result,sizeof(result));
        }
        ttymode(0);
        char *args[]={"terminalresizecheck","size",NULL};
        exec("/bin/terminalresizecheck",args); return 3;
    }
    close(ready[1]); close(results[1]);
    const int widths[]={160,80,80,200}, heights[]={50,25,25,60};
    for (int i=0;i<4;i++) {
        char ready_byte; if (read(ready[0],&ready_byte,1)!=1) return 4;
        /* Allow child to enter blocking read: resize must wake it. */
        sleep_ms(30);
        set_grid(widths[i],heights[i]);
        if (i==2) write(master,"x",1); /* Same-size updates must not emit resize. */
        struct report result;
        if (read(results[0],&result,sizeof(result))!=sizeof(result)) return 5;
        if (result.key!=(i==2 ? 'x' : KEY_RESIZE) || result.w!=widths[i] || result.h!=heights[i]) {
            printf("RESIZE FAIL: step %d key %d size %dx%d\n",i,result.key,result.w,result.h); return 6;
        }
        if (at(0,0)->ch!='A' || cols!=widths[i] || rows!=heights[i] || bot_m!=rows-1) return 7;
    }
    int status=-1;
    if (waitpid(pid,&status,0)!=pid || status) return 8;
    close(fds[1]); close(master); master=-1;
    close(ready[0]); close(results[0]);
    puts("TERMINAL RESIZE PASS: expand, restore, unchanged size, raw wakeup, exec geometry");
    return 0;
}
