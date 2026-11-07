/* tests/tls_test.c — проверка криптографии из user/lib/tls.c на известных ответах
 * (RFC 4231, 7748, 8439, 8448, тест-вектор GCM из NIST). Запускается на хосте: make test */
#include <unistd.h>
#include <errno.h>
static ssize_t tls_test_read(int fd, void *buf, size_t length);
#define read tls_test_read
#define TLS_HOST
#include "../user/lib/tls.c"
#include "../user/lib/x509.c"
#undef read
static int read_mode, read_calls, idle_calls;
static ssize_t tls_test_read(int fd, void *buf, size_t length) {
    if (fd != 12345) return read(fd, buf, length);
    read_calls++;
    if (read_mode == 2) { errno = EIO; return -1; }
    if (read_mode == 1 || read_calls == 1) { errno = ETIMEDOUT; return -1; }
    memset(buf, 0x5a, length); return (ssize_t)length;
}
static int test_idle(void) { idle_calls++; return read_mode == 0; }
static int timeout_checks(void) {
    unsigned char out = 0;
    tls_idle = test_idle;
    read_mode = read_calls = idle_calls = 0;
    int failed = read_all(12345, &out, 1) != 1 || out != 0x5a || idle_calls != 1;
    read_mode = 1; read_calls = idle_calls = 0;
    failed |= read_all(12345, &out, 1) != -1 || idle_calls != 1;
    read_mode = 2; read_calls = idle_calls = 0;
    failed |= read_all(12345, &out, 1) != -1 || idle_calls != 0;
    tls_idle = NULL;
    printf("TLS POSIX timeout handling: %s\n", failed ? "FAIL" : "ok");
    return failed;
}

static int fails;
static void unhex(const char *s, u8 *o) { for (; s[0] && s[1]; s += 2) { unsigned v; sscanf(s, "%2x", &v); *o++ = v; } }
static void check(const char *name, const u8 *got, const char *hex) {
    int n = strlen(hex) / 2;
    u8 *want = malloc(n);
    unhex(hex, want);
    int ok = !memcmp(got, want, n);
    printf("%-28s %s\n", name, ok ? "ok" : "FAIL");
    if (!ok) fails++;
    free(want);
}

int main(void) {
    fails += timeout_checks();
    u8 o[256], a[64], b[64], c[64];
    sha256("abc", 3, o);
    check("sha256(abc)", o, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    hmac_sha256("Jefe", 4, "what do ya want for nothing?", 28, o);
    check("hmac-sha256 (RFC 4231 #2)", o, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
    unhex("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4", a);
    unhex("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c", b);
    x25519(o, a, b);
    check("x25519 (RFC 7748)", o, "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552");
    unhex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a", a);
    memset(b, 0, 32); b[0] = 9;
    x25519(o, a, b);
    check("x25519 public (RFC 7748)", o, "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a");

    const char *pt = "Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, sunscreen would be it.";
    int n = strlen(pt);
    for (int i = 0; i < 32; i++) a[i] = 0x80 + i;
    unhex("070000004041424344454647", b);
    unhex("50515253c0c1c2c3c4c5c6c7", c);
    memcpy(o, pt, n);
    chacha_xor(a, 1, b, o, n);
    chapoly_tag(a, b, c, 12, o, n, o + n);
    check("chacha20-poly1305 (RFC 8439)", o, "d31a8d34648e60db7b86afbc53ef7ec2a4aded51296e08fea9e2b5a736ee62d63dbea45e8ca9671282fafb69da92728b1a71de0a9e060b2905d6a5b67ecd3b3692ddbd7f2d778b8c9803aee328091b58fab324e4fad675945585808b4831d7bc3ff4def08e4b7a9de576d26586cec64b61161ae10b594f09e26a7e902ecbd0600691");

    aesgcm g;
    unhex("feffe9928665731c6d6a8f9467308308", a);
    unhex("cafebabefacedbaddecaf888", b);
    unhex("feedfacedeadbeeffeedfacedeadbeefabaddad2", c);
    unhex("d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a721c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b39", o);
    gcm_init(&g, a);
    gcm_ctr(&g, b, o, 60);
    gcm_tag(&g, b, c, 20, o, 60, o + 60);
    check("aes-128-gcm (NIST #4)", o, "42831ec2217774244b7221b784d0d49ce3aa212f2c02a4e035c17e2329aca12e21d514b25466931c7d8f6a5aac84aa051ba30b396a0aac973d58e0915bc94fbc3221a5db94fae95ae7121a47");

    u8 zero[32], early[32], e[32];
    memset(zero, 0, 32);
    hmac_sha256(zero, 32, zero, 32, early);
    check("tls13 early secret (8448)", early, "33ad0a1c607ec03b09e6cd9893680ce210adf300aa1f2660e1b22e10f170f92a");
    sha256("", 0, e);
    hkdf_label(early, "derived", e, 32, o, 32);
    check("tls13 derived (RFC 8448)", o, "6f2615a108c702c5678f54fc9dbab69716c076189c48250cebeac3576c3611ba");

    printf(fails ? "TLS: %d test(s) FAILED\n" : "TLS: all tests passed\n", fails);
    return fails != 0;
}
