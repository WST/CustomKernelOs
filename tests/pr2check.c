#include "myos.h"
#include "stdio.h"
static int failures;
#define CHECK(c, label) do { if (!(c)) { printf("PR2 FAIL: %s (errno=%d)\n", label, errno); failures++; } } while (0)
static void put_keep(void) {
    int fd = open("/tmp/dl-keep", O_WRITE);
    CHECK(fd >= 0, "create download destination");
    if (fd >= 0) { CHECK(write(fd, "KEEP", 4) == 4, "write sentinel"); close(fd); }
}
static int matches(const char *path, const char *value) {
    char out[16]; int fd = open(path, O_READ);
    if (fd < 0) return 0;
    int n = read(fd, out, sizeof out); close(fd);
    return n == (int)strlen(value) && !memcmp(out, value, n);
}
static int download(const char *url, const char *target, int retry) {
    int pid = fork(), status = -1;
    if (pid < 0) return -1;
    if (!pid) {
        char *args[] = {"download", "-r", retry ? "1" : "0", "-o", (char *)target, (char *)url, NULL};
        exec("/bin/download", args); exit(99);
    }
    return waitpid(pid, &status, 0) < 0 ? -1 : status;
}
int main(int argc, char **argv) {
    CHECK(open("/etc/accounts", O_READ) == -1 && errno == EPERM, "database stays private");
    CHECK(rename("/etc/accounts", "/tmp/exposed") == -1 && errno == EPERM, "deny database rename");
    CHECK(rename("/etc/../etc/./accounts", "/tmp/exposed") == -1 && errno == EPERM, "deny database alias");
    CHECK(rename("/etc", "/tmp/moved-etc") == -1 && errno == EPERM, "deny database parent rename");
    CHECK(chdir("/etc") == 0, "relative setup");
    CHECK(rename("accounts", "/tmp/exposed") == -1 && errno == EPERM, "deny relative database rename");
    chdir("/");
    put_keep();
    CHECK(rename("/tmp/dl-keep", "/etc/accounts") == -1 && errno == EPERM, "deny replacing database");
    CHECK(matches("/tmp/dl-keep", "KEEP"), "replacement source preserved");
    CHECK(rename("/tmp/dl-keep", "/tmp/renamed") == 0, "ordinary rename works");
    CHECK(rename("/tmp/renamed", "/tmp/dl-keep") == 0, "ordinary rename back");
    CHECK(download("invalid-url", "/tmp/dl-keep", 0) != 0, "invalid URL rejected");
    CHECK(matches("/tmp/dl-keep", "KEEP"), "invalid URL preserves existing file");
    CHECK(download("invalid-url", "/tmp/dl-new", 0) != 0, "invalid new URL rejected");
    struct dirent_info info;
    CHECK(dirstat("/tmp/dl-new", &info) == -ENOENT, "no partial final file");
    if (argc > 1) {
        char url[128];
        snprintf(url, sizeof url, "%s/short", argv[1]);
        CHECK(download(url, "/tmp/dl-keep", 1) != 0, "truncated HTTP rejected after retry");
        CHECK(matches("/tmp/dl-keep", "KEEP"), "truncated HTTP preserves destination");
        snprintf(url, sizeof url, "%s/missing", argv[1]);
        CHECK(download(url, "/tmp/dl-keep", 0) != 0, "HTTP error rejected");
        CHECK(matches("/tmp/dl-keep", "KEEP"), "HTTP error preserves destination");
        snprintf(url, sizeof url, "%s/ok", argv[1]);
        CHECK(download(url, "/tmp/dl-keep", 0) == 0, "completed download replaces existing file");
        CHECK(matches("/tmp/dl-keep", "NEW"), "replacement contents");
        CHECK(download(url, "/tmp/dl-new", 0) == 0, "completed download creates new file");
        CHECK(matches("/tmp/dl-new", "NEW"), "new contents");
        mkdir("/tmp/dl-dir");
        CHECK(download(url, "/tmp/dl-dir", 0) != 0, "directory destination rejected");
        CHECK(dirstat("/tmp/dl-dir", &info) == 0 && info.type == 2, "directory preserved");
    } else CHECK(0, "HTTP fixture URL provided");
    for (int i = 0; dirread("/tmp", i, &info) == 1; i++)
        CHECK(strncmp(info.name, ".dl", 3) != 0, "temporary and backup files removed");
    printf(failures ? "PR2 FAILURES: %d\n" : "PR2 NATIVE PASS\n", failures);
    return failures != 0;
}
