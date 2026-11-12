#include "myos.h"
#include "stdio.h"
static int master,child,original_mode,original_output;static char output[65536];static unsigned length;
static int raw(int n,int a,int b,int c){int r;__asm__ volatile("int $0x80":"=a"(r):"a"(n),"b"(a),"c"(b),"d"(c):"memory");return r;}
static void pump(void){struct {int fd;short events,revents;} poll={master,1,0};
    for(int i=0;i<64;i++){poll.revents=0;int n=raw(SYS_SSHCTL,1,1,(int)&poll);if(n<1 || !(poll.revents&1))break;char buf[1024];n=read(master,buf,sizeof(buf));if(n<=0)break;if(length+n>=sizeof(output)){length=0;}memcpy(output+length,buf,n);length+=n;output[length]=0;}}
static int wait_text(const char *marker){unsigned start=ticks();while(ticks()-start<10000){pump();if(strstr(output,marker))return 0;sleep_ms(10);}printf("NANO CHECK missing %s: %.4000s\n",marker,output);return 1;}
static void clear(void){pump();length=0;output[0]=0;}
static int send(const char *s){clear();return write(master,s,strlen(s))!=(int)strlen(s);}
static void pause_for(unsigned ms){unsigned start=ticks();while(ticks()-start<ms){pump();sleep_ms(10);}}
static int start(const char *path){int fds[2];if(pty_open(fds)<0)return 1;master=fds[0];original_mode=ttyctl(master,TTYCTL_GETMODE,0);original_output=ttyctl(master,TTYCTL_GETOFLAGS,0);ttyctl(master,TTYCTL_SETSIZE,(80<<16)|24);child=fork();if(child<0)return 1;if(!child){dup2(fds[1],0);dup2(fds[1],1);dup2(fds[1],2);close(master);close(fds[1]);char *args[]={"nano",(char*)path,NULL};exec("/ext/bin/nano",args);exit(127);}close(fds[1]);length=0;return wait_text("GNU nano");}
static int finish(void){if(send("\x18"))return 1;unsigned began=ticks();int status=0;while(ticks()-began<10000){pump();if(waitpid(child,&status,WNOHANG)==child){if(ttyctl(master,TTYCTL_GETMODE,0)!=original_mode || ttyctl(master,TTYCTL_GETOFLAGS,0)!=original_output){puts("NANO CHECK terminal modes were not restored");status=1;}close(master);return status;}sleep_ms(10);}kill(child);close(master);return 1;}
static int save(void){if(send("\x0f")||wait_text("Write to File"))return 1;if(send("\r")||wait_text("Wrote"))return 1;return 0;}
static int file_is(const char *path,const char *expected){char bytes[256]={0};int fd=open(path,O_READ);if(fd<0)return 0;int got=read(fd,bytes,sizeof(bytes)-1);close(fd);if(got!=(int)strlen(expected)||memcmp(bytes,expected,got)){printf("NANO CHECK file mismatch bytes=%d [%s]\n",got,bytes);return 0;}return 1;}
int main(void){const char *path="/tmp/nano-edit-check.txt";const char *utf="alpha\n\xd0\x9f\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82\n";unlink(path);
 if(start(path)||send("alpha\r\xd0\x9f\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82"))return 1;
 pause_for(200);if(!strstr(output,"\033[?25h")){puts("NANO CHECK cursor hidden while waiting for input");return 6;}clear();ttyctl(master,TTYCTL_SETSIZE,(100<<16)|30);if(wait_text("GNU nano")||save()||finish()||!file_is(path,utf))return 2;
 if(start(path)||send("X"))return 3;
 pause_for(100);send("\x1b" "u");pause_for(100);if(send("\x17")||wait_text("Search"))return 4;send("alpha\r");pause_for(150);send("\x01" "found ");pause_for(100);send("\x0b");pause_for(100);send("\x15");pause_for(100);
 if(save()||finish()||!file_is(path,"found alpha\n\xd0\x9f\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82\n"))return 5;
 puts("NANO EDIT PASS: UTF-8, save/reopen, undo, search, cut/paste and PTY resize");return 0;}
