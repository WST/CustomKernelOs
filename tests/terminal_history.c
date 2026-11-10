/* Exercise the real terminal history, including ring wrap and resize. */
#define main terminal_main
#include "../user/terminal.c"
#undef main
#define CHECK(x) do { if (!(x)) return __LINE__ % 250 + 1; } while (0)
int ttyctl(int fd, int op, int arg) { (void)fd; (void)op; (void)arg; return 0; }
int main(void) {
    set_grid(10, 3);
    CHECK(grid && history && row_dirty);
    cur_fg = DEF_FG; cur_bg = DEF_BG;
    at(0, 0)->ch = 'A'; at(1, 0)->ch = 'B'; at(2, 0)->ch = 'C';
    scroll_region(0, 2, 1);
    CHECK(history_count == 1 && at(0, 0)->ch == 'B');
    view_back = 1;
    CHECK(view_at(0, 0)->ch == 'A' && view_at(1, 0)->ch == 'B');
    scroll_region(0, 2, 1);
    CHECK(view_back == 2 && view_at(0, 0)->ch == 'A');
    int count = history_count;
    scroll_region(1, 2, 1);
    CHECK(history_count == count); /* partial scroll regions are not shell history */
    view_back = 0;
    for (int i = 0; i < 1100; i++) {
        at(0, 0)->ch = i;
        scroll_region(0, 2, 1);
    }
    CHECK(history_count == HISTORY_ROWS);
    view_back = HISTORY_ROWS;
    CHECK(view_at(0, 0)->ch == 100);
    view_back = 1;
    CHECK(view_at(0, 0)->ch == 1099);
    set_grid(20, 5);
    CHECK(history_count == 0 && view_back == 0 && cols == 20);
    free(history); free(grid); free(row_dirty);
    return 0;
}
