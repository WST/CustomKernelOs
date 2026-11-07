#include "myos.h"
int clipboard_editor_test(void);
int clipboard_terminal_test(void);
static int fail(const char *s) { printf("CLIPCHECK FAIL: %s\n", s); return 1; }
int main(void) {
    if (clipboard_set(NULL, 0) || clipboard_get(NULL, 0)) return fail("empty");
    char out[32];
    if (clipboard_set("hello", 5) != 5 || clipboard_get(NULL, 0) != 5) return fail("set/query");
    memset(out, 0x55, sizeof(out));
    if (clipboard_get(out, 4) != -EFBIG || out[0] != 0x55) return fail("small buffer must not truncate");
    if (clipboard_get(out, sizeof(out)) != 5 || memcmp(out, "hello", 5) || out[5] != 0x55) return fail("exact bytes");
    if (clipboard_set((void *)0x1000, 1) != -EFAULT || clipboard_get((void *)0x1000, 5) != -EFAULT) return fail("kernel pointer");
    if (clipboard_set("x", CLIPBOARD_MAX + 1) != -EFBIG) return fail("limit");
    int pid = fork(), status = -1;
    if (pid < 0) return fail("fork");
    if (!pid) { int n = clipboard_set("child", 5); exit(n == 5 ? 0 : 1); }
    if (waitpid(pid, &status, 0) < 0 || status || clipboard_get(out, sizeof(out)) != 5 || memcmp(out, "child", 5)) return fail("cross-process persistence");
    char *large = malloc(CLIPBOARD_MAX);
    if (!large) return fail("allocation");
    memset(large, 'a', CLIPBOARD_MAX);
    if (clipboard_set(large, CLIPBOARD_MAX) != CLIPBOARD_MAX) return fail("maximum size set");
    memset(large, 0, CLIPBOARD_MAX);
    if (clipboard_get(large, CLIPBOARD_MAX) != CLIPBOARD_MAX || large[0] != 'a' || large[CLIPBOARD_MAX - 1] != 'a') return fail("maximum size get");
    free(large);
    int result = clipboard_editor_test();
    if (result) { printf("CLIPCHECK FAIL: editor case %d\n", result); return 1; }
    puts("CLIPCHECK EDITOR PASS");
    result = clipboard_terminal_test();
    if (result) { printf("CLIPCHECK FAIL: terminal case %d\n", result); return 1; }
    puts("CLIPCHECK TERMINAL PASS");
    ttymode(TTY_RAW);
    puts("CLIPCHECK KEYS READY");
    int expected[] = {KEY_RIGHT | KEY_SHIFT, KEY_COPY, KEY_PASTE, KEY_INS | KEY_SHIFT};
    for (int i = 0; i < 4; i++) {
        int k = readkey();
        if (k != expected[i]) { printf("CLIPCHECK FAIL: key %d got %d expected %d\n", i, k, expected[i]); ttymode(0); return 1; }
    }
    ttymode(0);
    clipboard_set(NULL, 0);
    puts("CLIPCHECK PASS");
    return 0;
}
