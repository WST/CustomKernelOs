/* Exercise the actual editor loader on host files; unused GUI code is discarded. */
#define main code_application_main
#include "../user/code.c"
#undef main
#define CHECK(x) do { if(!(x)) { dprintf(STDERR,"document test failed at %d: %s\n",__LINE__,#x); dprintf(STDERR,"%s\n",status_msg); return 1; } } while(0)
int main(void) {
    char fixture[128];snprintf(fixture,sizeof(fixture),"/tmp/myos-code-document-%d",getpid());
    char *long_line=malloc(8192);CHECK(long_line);memset(long_line,'x',8192);
    int fd=open(fixture,O_WRITE,0600);CHECK(fd>=0);CHECK(write(fd,long_line,8192)==8192);close(fd);
    clear_text();CHECK(load(fixture));CHECK(n_lines==1 && lens[0]==8192);CHECK(!memcmp(lines[0],long_line,8192));
    fd=open(fixture,O_WRITE,0600);CHECK(fd>=0);CHECK(write(fd,"binary\0data",11)==11);close(fd);
    CHECK(!load(fixture));CHECK(n_lines==1 && lens[0]==8192);
    CHECK(!load("/definitely-no-such-myOS-code-file"));CHECK(lens[0]==8192);
    fd=open(fixture,O_WRITE,0600);CHECK(fd>=0);CHECK(write(fd,"a\r\n\tb\n",6)==6);close(fd);
    CHECK(load(fixture));CHECK(n_lines==3);CHECK(lens[0]==1 && lines[0][0]=='a');CHECK(lens[1]==5 && !memcmp(lines[1],"    b",5));
    fd=open(fixture,O_WRITE,0600);CHECK(fd>=0);
    for(int i=0;i<MAX_LINES;i++) CHECK(write(fd,"\n",1)==1);
    close(fd);CHECK(!load(fixture));CHECK(n_lines==3 && lens[1]==5);
    unlink(fixture);free(long_line);while(n_lines) delete_line(n_lines-1);
    dprintf(STDOUT,"Code document tests passed\n");return 0;
}
