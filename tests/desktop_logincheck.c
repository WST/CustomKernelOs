#define account observed_account
#define main desktop_main
#include "../user/desktop.c"
#undef main
#undef account
int account(int op, const char *name, const char *password);
int observed_account(int op, const char *name, const char *password) {
    int result=account(op,name,password);
    if (op==1 && !result) printf("GUI SIGNED IN %s\n",name);
    if (op==6 && !result) puts("GUI LOGGED OUT");
    return result;
}
static int failures;
#define CHECK(x) do { if (!(x)) { printf("LOGIN FAIL %d: %s\n",__LINE__,#x); failures++; } } while(0)
int main(int argc, char **argv) {
    char name[24], path[128]; struct dirent_info st;
    if (account(0,NULL,NULL)>0) CHECK(account(3,"root","review123")==0);
    CHECK(account(1,"root","review123")==0);
    CHECK(getcwd(path,sizeof(path))==0 && !strcmp(path,"/root"));
    CHECK(dirstat("/root",&st)==0 && st.type==2);
    if (account_list(name,1)==-ENOENT) CHECK(account(2,"alice","alice123")==0);
    CHECK(dirstat("/home/alice",&st)==0 && st.type==2);
    if (argc>1 && !strcmp(argv[1],"desktop")) {
        CHECK(account(6,NULL,NULL)==0);
        puts("DESKTOP READY");
        return desktop_main();
    }
    CHECK(desktop_login()); /* Already authenticated: no graphics or input required. */
    CHECK(account(6,NULL,NULL)==0);
    CHECK(account_list(name,0)==0 && !strcmp(name,"root"));
    CHECK(account_list(name,1)==0 && !strcmp(name,"alice"));
    CHECK(account_list(name,2)==-ENOENT);
    CHECK(account_list(name,~0u)==-ENOENT);
    CHECK(account_list((char *)0xFFFFFFFF,0)==-EFAULT);
    CHECK(gfx_open()==0);
    SW=gfx_w; SH=gfx_h; back=gfx_buf;
    make_wallpaper(); dock_build();
    ttymode(TTY_RAW|TTY_NONBLOCK|TTY_NOSIG);
    puts("LOGIN READY");
    bool ok=desktop_login();
    CHECK(dirstat("/home/alice",&st)==0 && st.type==2);
    if (argc>1 && !strcmp(argv[1],"cancel")) {
        CHECK(!ok && account(4,NULL,NULL)==-EPERM);
    } else {
        CHECK(ok);
        CHECK(getcwd(path,sizeof(path))==0 && !strcmp(path,"/home/alice"));
        CHECK(account(4,NULL,NULL)==1);
        CHECK(account(5,name,NULL)==0 && !strcmp(name,"alice"));
        CHECK(account(2,"intruder","test")==-EPERM);
        int pid=fork();
        if (!pid) exit(account(4,NULL,NULL)==1 ? 0 : 1);
        int status=-1; CHECK(pid>0); CHECK(waitpid(pid,&status,0)==pid && status==0);
    }
    gfx_close();
    puts(failures ? "LOGIN FAILED" : "LOGIN PASS");
    return failures!=0;
}
