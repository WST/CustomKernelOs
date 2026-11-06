/* Имена файлов, по одному на строку. Без -l: для этого нужны время и права. */
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static int list(const char *path) {
    DIR *d = opendir(path);
    struct dirent *ent;
    if (!d) {
        fprintf(stderr, "ls: %s: %s\n", path, strerror(errno));
        return 1;
    }
    while ((ent = readdir(d)))
        printf("%s\n", ent->d_name);
    if (closedir(d) < 0) {
        fprintf(stderr, "ls: %s: %s\n", path, strerror(errno));
        return 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    int i, rc = 0;
    if (argc < 2) return list(".");
    for (i = 1; i < argc; i++) {
        struct stat st;
        if (stat(argv[i], &st) < 0) {
            fprintf(stderr, "ls: %s: %s\n", argv[i], strerror(errno));
            rc = 1;
            continue;
        }
        if (!S_ISDIR(st.st_mode)) {
            printf("%s\n", argv[i]);
            continue;
        }
        if (argc > 2) printf("%s:\n", argv[i]);
        if (list(argv[i])) rc = 1;
    }
    return rc;
}
