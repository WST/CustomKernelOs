/* opendir/readdir, POSIX stat, rename, basename и dirname. */
#include <myos.h>
#include <dirent.h>
#include <libgen.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static int fails;

#define CHECK(cond) do { \
    if (!(cond)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

int main(void) {
    int fd;
    struct stat st;
    DIR *d;
    struct dirent *ent;
    int saw = 0;
    char base[] = "/ext/bin/ls";
    char dir[] = "/ext/bin/ls";
    char one[] = "ls";
    char root[] = "/";

    unlink("build/cct_dir/a.txt");
    unlink("build/cct_dir/b.txt");
    unlink("build/cct_dir");
    CHECK(mkdir("build/cct_dir") == 0);

    fd = open("build/cct_dir/a.txt", O_WRITE);
    CHECK(fd >= 0);
    if (fd >= 0) {
        CHECK(write(fd, "hi", 2) == 2);
        close(fd);
    }

    CHECK(stat("build/cct_dir/a.txt", &st) == 0);
    CHECK(S_ISREG(st.st_mode) && st.st_size == 2);
    CHECK(stat("build/cct_dir", &st) == 0 && S_ISDIR(st.st_mode));
    CHECK(stat("build/cct_dir/missing", &st) < 0 && errno == ENOENT);

    d = opendir("build/cct_dir/a.txt");
    CHECK(d == NULL && errno == ENOTDIR);
    d = opendir("build/cct_dir");
    CHECK(d != NULL);
    if (d) {
        while ((ent = readdir(d)))
            if (!strcmp(ent->d_name, "a.txt") && ent->d_type == DT_REG) saw = 1;
        CHECK(closedir(d) == 0);
    }
    CHECK(saw);

    CHECK(rename("build/cct_dir/a.txt", "build/cct_dir/b.txt") == 0);
    CHECK(stat("build/cct_dir/a.txt", &st) < 0);
    CHECK(stat("build/cct_dir/b.txt", &st) == 0 && st.st_size == 2);
    CHECK(rename("build/cct_dir/missing", "build/cct_dir/c.txt") < 0 && errno == ENOENT);
    CHECK(remove("build/cct_dir/b.txt") == 0);
    CHECK(remove("build/cct_dir") == 0);

    CHECK(!strcmp(basename(base), "ls"));
    CHECK(!strcmp(dirname(dir), "/ext/bin"));
    CHECK(!strcmp(basename(one), "ls") && !strcmp(dirname(one), "."));
    CHECK(!strcmp(basename(root), "/") && !strcmp(dirname(root), "/"));

    return fails != 0;
}
