/* ls [path...] — содержимое каталогов; файлы показываются как есть */
#include "myos.h"

static void entry(const struct dirent_info *e, const char *shown) {
    if (e->type == 2) printf("   <DIR>  " ANSI_BLUE ANSI_BOLD "%s/" ANSI_RESET "\n", shown);
    else if(e->type==3) printf(" <BLOCK>  %s\n",shown);
    else if(e->type==4) printf("  <CHAR>  %s\n",shown);
    else              printf("%8u  %s\n", e->size, shown);
}

static int list_dir(const char *path, bool title) {
    struct dirent_info e;
    if (title) printf("%s:\n", path);
    for (int i = 0; readdir(path, i, &e) == 1; i++) entry(&e, e.name);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) return list_dir(".", false);
    int rc = 0, dirs = 0;
    struct dirent_info e;
    /* сначала файлы, потом каталоги — как в настоящем ls */
    for (int i = 1; i < argc; i++) {
        int r = stat(argv[i], &e);
        if (r < 0) { dprintf(STDERR, "ls: %s: %s\n", argv[i], strerror(-r)); rc = 1; continue; }
        if (e.type == 2) dirs++;
        else entry(&e, argv[i]);
    }
    for (int i = 1; i < argc; i++)
        if (stat(argv[i], &e) == 0 && e.type == 2) list_dir(argv[i], argc > 2);
    (void)dirs;
    return rc;
}
