#include "myos.h"
#include "sys/stat.h"
#include "unistd.h"
static int failures;
#define CHECK(x) do { if (!(x)) { printf("SERVICE FAIL line %d: %s\n", __LINE__, #x); failures++; } } while (0)
static void put(const char *path, const char *text) {
    int fd = open(path, O_WRITE); CHECK(fd >= 0);
    if (fd >= 0) { CHECK(write(fd, text, strlen(text)) == (int)strlen(text)); close(fd); }
}
int main(int argc, char **argv) {
    struct unit_info u, v;
    if(argc>1 && !strcmp(argv[1],"root")) {
        CHECK(account(4,NULL,NULL)==0 && geteuid()==0 && getuid()==0 && getgid()==0);
        char name[24];CHECK(account(5,name,NULL)==0 && !strcmp(name,"root"));
        puts(failures?"ROOT SERVICE FAILED":"ROOT SERVICE PASS");return failures!=0;
    }
    if(argc>1 && !strcmp(argv[1],"anonymous")) {
        CHECK(account(4,NULL,NULL)==-EPERM && geteuid()==65534);
        puts(failures?"DEFAULT SERVICE FAILED":"DEFAULT SERVICE PASS");return failures!=0;
    }
    if (argc > 1 && !strcmp(argv[1], "desktop")) {
        /* An unauthenticated greeter is intentionally hidden from other IPC identities. */
        CHECK(unitctl(UNIT_STATUS, "desktop.service", &u, 0) == 0 && u.state == UNIT_ACTIVE);
        sleep_ms(5000); /* Host captures the graphical screen during this interval. */
        CHECK(account(1, "root", "review123") == 0);
        CHECK(unitctl(UNIT_STOP, "desktop.service", &u, 0) == 0 && !u.pid);
        puts(failures ? "SERVICE DESKTOP FAILED" : "SERVICE DESKTOP PASS"); return failures != 0;
    }
    if (argc > 1 && !strcmp(argv[1], "boot")) {
        CHECK(unitctl(UNIT_STATUS, "test.service", &u, 0) == 0);
        CHECK(u.enabled && u.state == UNIT_ACTIVE && u.pid > 0);
        CHECK(unitctl(UNIT_DISABLE, "test.service", &u, 0) == 0);
        CHECK(unitctl(UNIT_STOP, "test.service", &u, 0) == 0);
        puts(failures ? "SERVICE BOOT FAILED" : "SERVICE BOOT PASS"); return failures != 0;
    }
    CHECK(account(6, NULL, NULL) == 0);
    CHECK(unitctl(UNIT_START, "httpd.service", &u, 0) == -EPERM);
    CHECK(unitctl(UNIT_LIST, NULL, &u, 0) == 0);
    CHECK(account(1, "root", "review123") == 0);
    put("/etc/services/root.service","[Service]\nUser=root\nExecStart=/bin/servicecheck root\n");
    put("/etc/services/anonymous.service","[Service]\nExecStart=/bin/servicecheck anonymous\n");
    put("/etc/services/baduser.service","[Service]\nUser=alice\nExecStart=/bin/sleep 1\n");
    CHECK(unitctl(UNIT_RELOAD,NULL,NULL,0)==0);
    CHECK(unitctl(UNIT_START,"baduser.service",NULL,0)==-EINVAL);
    CHECK(chmod("/etc/services/root.service",0666)==0);
    CHECK(unitctl(UNIT_START,"root.service",NULL,0)==-EACCES);
    CHECK(chmod("/etc/services/root.service",0644)==0);
    CHECK(chown("/etc/services/root.service",1001,1001)==0);
    CHECK(unitctl(UNIT_START,"root.service",NULL,0)==-EACCES);
    CHECK(chown("/etc/services/root.service",0,0)==0);
    CHECK(chmod("/etc/services",0777)==0);
    CHECK(unitctl(UNIT_START,"root.service",NULL,0)==-EACCES);
    CHECK(chmod("/etc/services",0755)==0);
    CHECK(unitctl(UNIT_START,"root.service",NULL,0)==0);
    CHECK(unitctl(UNIT_START,"anonymous.service",NULL,0)==0);
    sleep_ms(300);
    CHECK(unitctl(UNIT_ENABLE,"root.service",NULL,0)==0);
    CHECK(unitctl(UNIT_ENABLE,"anonymous.service",NULL,0)==0);
    put("/etc/services/test.service", "[Service]\nExecStart=/bin/sleep 60\nRestart=on-failure\n");
    put("/etc/services/bad.service", "[Service]\nExecStart=relative\n");
    put("/etc/services/quote.service", "[Service]\nExecStart=/bin/echo \"hello world\" 'two words' escaped\\ space\n");
    CHECK(unitctl(UNIT_RELOAD, NULL, NULL, 0) == 0);
    CHECK(unitctl(UNIT_START, "bad.service", &u, 0) == -EINVAL);
    CHECK(unlink("/etc/services/bad.service") == 0);
    CHECK(unitctl(UNIT_RELOAD, NULL, NULL, 0) == 0);
    CHECK(unitctl(UNIT_STATUS, "bad.service", &u, 0) == -ENOENT);
    CHECK(unitctl(UNIT_START, "quote.service", &u, 0) == 0);
    sleep_ms(300);
    CHECK(unitctl(UNIT_STATUS, "quote.service", &u, 0) == 0 && u.state == UNIT_INACTIVE);
    char log[100] = {0}; int fd = open("/var/log/quote.service", O_READ);
    CHECK(fd >= 0); if (fd >= 0) { int n=read(fd, log, sizeof(log)-1); CHECK(n>0); close(fd); }
    CHECK(!strcmp(log, "hello world two words escaped space\n"));
    CHECK(unitctl(UNIT_START, "test.service", &u, 0) == 0 && u.pid);
    CHECK(unitctl(UNIT_START, "test.service", &v, 0) == 0 && v.pid == u.pid);
    CHECK(kill(u.pid) == 0);
    sleep_ms(1600);
    CHECK(unitctl(UNIT_STATUS, "test.service", &v, 0) == 0);
    CHECK(v.state == UNIT_ACTIVE && v.pid != u.pid && v.restarts == 1);
    CHECK(unitctl(UNIT_STOP, "test.service", &u, 0) == 0 && !u.pid);
    sleep_ms(1200);
    CHECK(unitctl(UNIT_STATUS, "test.service", &u, 0) == 0 && u.state == UNIT_INACTIVE);
    CHECK(unitctl(UNIT_RESTART, "test.service", &u, 0) == 0 && u.pid);
    CHECK(unitctl(UNIT_ENABLE, "test.service", &u, 0) == 0 && u.enabled);
    CHECK(unitctl(UNIT_ENABLE, "test.service", &u, 0) == 0);
    CHECK(unitctl(UNIT_STOP, "test.service", &u, 0) == 0 && u.enabled);
    puts(failures ? "SERVICE NATIVE FAILED" : "SERVICE NATIVE PASS");
    return failures != 0;
}
