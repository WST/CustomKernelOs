/* Exercise the actual Notepad editing implementation in a native MyOS ELF. */
#define main notepad_application_main
#define win_set_title clipboard_test_title
#include "../user/notepad.c"
#undef main

void win_set_title(window *w, const char *s) { (void)w; (void)s; }

int clipboard_editor_test(void) {
    clear_text();
    const char text[] = "Hello\n\320\237\321\200\320\270\320\262\320\265\321\202";
    if (clipboard_set(text, sizeof(text) - 1) < 0) return 1;
    text_key(CTRL('v'));
    if (n_lines != 2 || lens[0] != 5 || lens[1] != 12 || cur_c != 12) return 2;
    text_key(KEY_LEFT | KEY_SHIFT);
    if (anchor_c != 12 || cur_c != 10) return 3;
    text_key(CTRL('x'));
    char s[64];
    if (clipboard_get(s, sizeof(s)) != 2 || memcmp(s, "\321\202", 2) || lens[1] != 10) return 4;
    text_key(CTRL('v'));
    if (lens[1] != 12 || cur_c != 12 || has_selection()) return 5;
    text_key(CTRL('a')); text_key(CTRL('c'));
    if (clipboard_get(s, sizeof(s)) != (int)sizeof(text) - 1 || memcmp(s, text, sizeof(text) - 1)) return 6;
    text_key('X');
    if (n_lines != 1 || lens[0] != 1 || lines[0][0] != 'X' || has_selection()) return 7;
    text_key(CTRL('a')); text_key(KEY_DEL);
    if (n_lines != 1 || lens[0] || cur_r || cur_c || has_selection()) return 8;
    clipboard_set("a\r\nb\rc\td", 8); text_key(CTRL('v'));
    if (n_lines != 3 || lens[0] != 1 || lens[1] != 1 || lens[2] != 6) return 9;
    text_key(CTRL('a'));
    clipboard_set("\xf0\x9f\x98\x80", 4); text_key(CTRL('v'));
    if (!has_selection() || n_lines != 3) return 10;
    /* Multiline deletion with a tail on the last selected line. */
    anchor_r = 0; anchor_c = 0; cur_r = 2; cur_c = 1;
    text_key(8);
    if (n_lines != 1 || lens[0] != 5 || memcmp(lines[0], "    d", 5)) return 11;
    clear_text();
    return 0;
}
