/* Exercise the actual close syscall; unused syscall sections are discarded. */
#define HOST_TEST
#include "../kernel/syscall.c"
int printf(const char *, ...);int puts(const char *);void exit(int);
#define CHECK(c) do { if (!(c)) { printf("CLOSE FAIL line %d: %s\n",__LINE__,#c);exit(1); } } while(0)
static struct task process,thread;
static struct file handle;
static int flush_result,flushes,closes;
struct task *task_current(void) { return &thread; }
struct task *task_process(struct task *t) { CHECK(t==&thread);return &process; }
int block_flush(void) { flushes++;CHECK(process.fds[3]==NULL);return flush_result; }
void file_close(struct file *f) { CHECK(f==&handle);CHECK(flushes==1);closes++; }
static void prepare(int result) { process.fds[3]=&handle;process.cloexec=1u<<3;flush_result=result;flushes=closes=0; }
int main(void) {
    prepare(-1);CHECK(sys_close(3)==-EIO);CHECK(closes==1 && !process.fds[3] && !(process.cloexec&(1u<<3)));
    CHECK(sys_close(3)==-EBADF && flushes==1 && closes==1);
    prepare(0);CHECK(sys_close(3)==0 && closes==1 && flushes==1);
    CHECK(sys_close(MAX_FDS)==-EBADF && flushes==1 && closes==1);
    puts("CLOSE FLUSH PASS: EIO reaches caller; descriptor and file reference released");return 0;
}
