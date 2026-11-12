#include "myos.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"
#define MAP_FAILED ((void*)-1)
static int failures;
#define CHECK(c) do{if(!(c)){printf("ISOLATION FAIL %d %s errno=%d\n",__LINE__,#c,errno);failures++;}}while(0)
static void put(int fd,int v){CHECK(write(fd,&v,4)==4);}
static int take(int fd){int v=-999;CHECK(read(fd,&v,4)==4);return v;}
static void waited(int pid,int expected){int status=-1;CHECK(pid>0 && waitpid(pid,&status,0)==pid && status==expected);}
static void drop(int uid){CHECK(setgid(uid)==0);CHECK(setuid(uid)==0);}
static volatile unsigned stop;
static void idle(void *arg){(void)arg;while(!stop)yield();}
static void smash(void *arg){*(volatile unsigned*)arg=42;}
static void memory_checks(void){
 const unsigned bottom=0xbff00000u,guard=bottom-4096;
 CHECK(mmap((void*)bottom,4096,3,MAP_PRIVATE|MAP_ANONYMOUS,-1,0)==MAP_FAILED && errno==EINVAL);
 CHECK(mmap((void*)guard,4096,3,MAP_PRIVATE|MAP_ANONYMOUS,-1,0)==MAP_FAILED && errno==EINVAL);
 CHECK(munmap((void*)bottom,4096)==-EINVAL);CHECK(mprotect((void*)bottom,4096,3)==-EINVAL);
 CHECK(mprotect((void*)guard,4096,3)==-EINVAL);
 void *edge=mmap((void*)(guard-4096),4096,3,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(edge==(void*)(guard-4096));if(edge!=MAP_FAILED)CHECK(munmap(edge,4096)==0);
 int child=fork();if(!child){*(volatile unsigned*)guard=1;exit(99);}waited(child,139);
 void *stack=mmap(NULL,65536,3,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(stack!=MAP_FAILED);if(stack==MAP_FAILED)return;
 stop=0;int tid=thread_create(idle,NULL,stack,65536);CHECK(tid>0);
 CHECK(mprotect(stack,4096,3)==-EBUSY);CHECK(munmap(stack,65536)==-EBUSY);
 CHECK(mmap(stack,4096,3,MAP_PRIVATE|MAP_ANONYMOUS,-1,0)==MAP_FAILED);
 CHECK(thread_create(idle,NULL,(char*)stack+4096,32768)<0);
 int fds[2];CHECK(pipe(fds)==0);put(fds[1],3);CHECK(read(fds[0],stack,4)<0 && errno==EFAULT);close(fds[0]);close(fds[1]);
 stop=1;int status=-1;CHECK(thread_join(tid,&status)==0 && !status);*(unsigned*)stack=7;
 CHECK(mprotect(stack,4096,3)==0 && munmap(stack,65536)==0);
 child=fork();if(!child){void *p=malloc(65536);unsigned page=((unsigned)p+4095)&~4095u;CHECK(thread_create(smash,(void*)page,p,65536)>0);for(;;)yield();}waited(child,139);
 /* Heap shrink/regrow must not bypass a live stack's guard. */
 void *heap=malloc(65536),*heap_end=sbrk(0);stop=0;
 tid=thread_create(idle,NULL,heap,65536);CHECK(tid>0);
 CHECK(sbrk((char*)heap-(char*)heap_end)==(void*)-1 && sbrk(0)==heap_end);
 stop=1;CHECK(thread_join(tid,&status)==0);free(heap);
 /* Caller-provided heap pages are restored before join returns/free reuses them. */
 for(int i=0;i<12;i++){void *p=malloc(65536);stop=1;tid=thread_create(idle,NULL,p,65536);CHECK(tid>0 && thread_join(tid,&status)==0);memset(p,0x5a,65536);free(p);}
 puts("Stack guards, mmap boundary and restore after join: PASS");
}
static void ipc_checks(void){
 int id=shm_alloc(4096);unsigned *root=shm_map(id);CHECK(root!=NULL);if(!root)return;*root=0xabc123;
 CHECK(service_register("isolation")==0);
 int ready[2],command[2];CHECK(pipe(ready)==0 && pipe(command)==0);int parent=getpid();
 int child=fork();if(!child){
  CHECK(shm_map(id)==NULL && errno==EACCES);drop(1001);CHECK(kill(parent)==-EPERM);CHECK(kill(1)==-EPERM);
  struct msg m={0};m.from=123456;CHECK(msg_send(parent,&m)==-EPERM);CHECK(service_find("isolation")==0);CHECK(service_register("isolation")==0 && service_find("isolation")==getpid());
  CHECK(shm_map(id)==NULL && errno==EACCES);CHECK(shm_grant(id,parent,3)==-EPERM);
  int own=shm_alloc(4096);unsigned *p=shm_map(own);CHECK(p!=NULL);*p=99;put(ready[1],own);
  CHECK(take(command[0])==1);CHECK(shm_grant(own,parent,SHM_READ)==0);CHECK(msg_allow(parent,1)==0);put(ready[1],1);
  CHECK(take(command[0])==2);unsigned *shared=shm_map(id);CHECK(shared && *shared==0xabc123);
  CHECK(msg_send(parent,&m)==0);CHECK(msg_recv(&m,2000)==0 && m.from==parent);put(ready[1],2);
  CHECK(take(command[0])==3);CHECK(shm_map(id)==shared);CHECK(shm_unmap(shared)==0);CHECK(shm_map(id)==NULL && errno==EACCES);CHECK(msg_send(parent,&m)==-EPERM);put(ready[1],3);
  CHECK(take(command[0])==4);char value[16],receiver[16];snprintf(value,sizeof(value),"%d",id);snprintf(receiver,sizeof(receiver),"%d",parent);
  char *args[]={"isolation-check","epoch",value,receiver,NULL};exec("/bin/isolation-check",args);exit(99);
 }
 int own=take(ready[0]);CHECK(shm_map(own)==NULL && errno==EACCES);CHECK(msg_allow(child,1)==0);CHECK(shm_grant(id,child,SHM_READ)==0);
 put(command[1],1);CHECK(take(ready[0])==1);unsigned *foreign=shm_map(own);CHECK(foreign && *foreign==99);if(foreign)CHECK(shm_unmap(foreign)==0);
 put(command[1],2);struct msg m={0};m.from=987;CHECK(msg_send(child,&m)==0);CHECK(msg_recv(&m,2000)==0 && m.from==child);CHECK(take(ready[0])==2);
 CHECK(shm_grant(id,child,0)==0 && msg_allow(child,0)==0);put(command[1],3);CHECK(take(ready[0])==3);
 CHECK(shm_grant(id,child,SHM_READ)==0 && msg_allow(child,1)==0);put(command[1],4);waited(child,0);
 CHECK(service_find("isolation")==parent);
 close(ready[0]);close(ready[1]);close(command[0]);close(command[1]);
 /* Read-only grants must be enforced by page tables, not just metadata. */
 CHECK(pipe(ready)==0);child=fork();if(!child){CHECK(take(ready[0])==1);unsigned *p=shm_map(id);CHECK(p && *p==0xabc123);*p=0;exit(99);}CHECK(shm_grant(id,child,SHM_READ)==0);put(ready[1],1);waited(child,139);CHECK(*root==0xabc123);close(ready[0]);close(ready[1]);
 /* A foreign user's thread ID must not bypass process ownership checks. */
 CHECK(pipe(ready)==0);int victim=fork();if(!victim){drop(1002);stop=0;void *s=malloc(65536);int thread=thread_create(idle,0,s,65536);put(ready[1],thread);for(;;)yield();}
 int victim_tid=take(ready[0]);child=fork();if(!child){drop(1001);CHECK(kill(victim)==-EPERM && kill(victim_tid)==-EPERM);
  int same=fork();if(!same){for(;;)yield();}CHECK(kill(same)==0);waited(same,137);exit(failures?1:0);
 }waited(child,0);CHECK(kill(victim_tid)==0);waited(victim,137);close(ready[0]);close(ready[1]);
 child=fork();if(!child){CHECK(account(6,NULL,NULL)==0);CHECK(kill(parent)==-EPERM && msg_send(parent,&m)==-EPERM && service_find("isolation")==0);CHECK(service_register("anonymous")==-EPERM);for(int i=0;i<16;i++)CHECK(shm_alloc(4096)>0);exit(failures?1:0);}waited(child,0);
 CHECK(shm_unmap(root)==0);int slots[64];for(int i=0;i<64;i++){slots[i]=shm_alloc(4096);CHECK(slots[i]>0);}CHECK(shm_alloc(4096)<0);for(int i=0;i<64;i++)CHECK(shm_discard(slots[i])==0);
 int old=shm_alloc(4096);CHECK(shm_discard(old)==0);int fresh=shm_alloc(4096);CHECK(fresh!=old && shm_map(old)==NULL && errno==EINVAL);CHECK(shm_discard(fresh)==0);
 puts("Kill ownership, IPC grants/epochs, namespaces and cleanup: PASS");
}
int main(int argc,char **argv){
 if(argc>1 && !strcmp(argv[1],"epoch")){int id=atoi(argv[2]),parent=atoi(argv[3]);struct msg m={0};CHECK(shm_map(id)==NULL && errno==EACCES);CHECK(msg_send(parent,&m)==-EPERM);puts(failures?"EPOCH FAIL":"Exec invalidates old IPC grants: PASS");return failures?1:0;}
 CHECK(geteuid()==0);memory_checks();ipc_checks();puts(failures?"ISOLATION FAILED":"ISOLATION PASS");return failures?1:0;
}
