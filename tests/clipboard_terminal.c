#define main terminal_application_main
#include "../user/terminal.c"
#undef main
int clipboard_terminal_test(void) {
    cols = rows = 0;
    set_grid(10, 3);
    at(0, 0)->ch = 'A'; at(0, 1)->ch = 0x42F;
    at(1, 0)->ch = 'B';
    sel_anchor = 11; sel_end = 0; /* reversed, multiline, trim trailing blanks */
    on_key(KEY_COPY);
    char out[32];
    if (clipboard_get(out, sizeof(out)) != 5 || memcmp(out, "A\320\257\nB", 5)) return 1;
    int fds[2];
    if (pipe(fds) < 0) return 2;
    master = fds[1];
    const char s[] = "hi\r\n\320\257\033\003\177\t";
    clipboard_set(s, sizeof(s) - 1);
    on_key(KEY_PASTE);
    paste_step();
    if (read(fds[0], out, 32) != 6 || memcmp(out, "hi\r\320\257\t", 6)) return 3;
    on_key(CTRL('c'));
    if (read(fds[0], out, 1) != 1 || out[0] != 3) return 4;
    close(fds[0]); close(fds[1]); master = -1;
    free(grid); free(row_dirty); grid = NULL; row_dirty = NULL; rows = cols = 0;
    return 0;
}
