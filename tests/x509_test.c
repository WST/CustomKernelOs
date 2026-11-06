/* tests/x509_test.c — проверка сертификатов (user/lib/x509.c) на Linux: make x509test.
 *   build/x509-test КАТАЛОГ   (тестовые цепочки делает tests/x509/make.py)
 * Плюс известные ответы: SHA-384/512 (FIPS 180), ECDH P-256 (RFC 5903), корни Mozilla читаются. */
#define TLS_HOST
#include "../user/lib/tls.c"
#include "../user/lib/x509.c"

static int fails;
static void ok(const char *what, int good) { printf("%-46s %s\n", what, good ? "ok" : "FAIL"); if (!good) fails++; }
static void unhex2(const char *s, u8 *o) { for (; s[0] && s[1]; s += 2) { unsigned v; sscanf(s, "%2x", &v); *o++ = v; } }

static u8 *load(const char *dir, const char *f, int *n) {
    char p[512];
    snprintf(p, sizeof(p), "%s/%s", dir, f);
    FILE *fp = fopen(p, "rb");
    if (!fp) { *n = 0; return NULL; }
    fseek(fp, 0, SEEK_END); *n = ftell(fp); fseek(fp, 0, SEEK_SET);
    u8 *b = malloc(*n + 1);
    *n = fread(b, 1, *n, fp);
    fclose(fp);
    return b;
}

static int chain(const char *dir, const char *f, const char *host, char *err, char *iss) {
    int n;
    u8 *d = load(dir, f, &n);
    const u8 *cs[10]; int cl[10], k = 0;
    for (int p = 0; d && p + 4 <= n && k < 10;) {
        int l = d[p] << 24 | d[p + 1] << 16 | d[p + 2] << 8 | d[p + 3];
        cs[k] = d + p + 4; cl[k] = l; k++;
        p += 4 + l;
    }
    int r = x509_verify_chain(cs, cl, k, host, err, 128, iss, 64);
    free(d);
    return r;
}

int main(int argc, char **argv) {
    u8 h[64], w[64];
    sha384("abc", 3, h);
    unhex2("cb00753f45a35e8bb5a03d699ac65007272c32ab0eded1631a8b605a43ff5bed8086072ba1e7cc2358baeca134c825a7", w);
    ok("sha384(abc)", !memcmp(h, w, 48));
    sha512("abc", 3, h);
    unhex2("ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f", w);
    ok("sha512(abc)", !memcmp(h, w, 64));
    /* RFC 5903, 8.1: ECDH P-256 */
    u8 pa[32], pb[32], pubA[65], pubB[65], za[32], zb[32], zw[32];
    unhex2("c88f01f510d9ac3f70a292daa2316de544e9aab8afe84049c62a9c57862d1433", pa);
    unhex2("c6ef9c5d78ae012a011164acb397ce2088685d8f06bf9be0b283ab46476bee53", pb);
    p256_keygen(pa, pubA); p256_keygen(pb, pubB);
    unhex2("dad0b65394221cf9b051e1feca5787d098dfe637fc90b9ef945d0c3772581180", w);
    ok("P-256 public key (RFC 5903)", !memcmp(pubA + 1, w, 32));
    p256_shared(pa, pubB, za); p256_shared(pb, pubA, zb);
    unhex2("d6840f6b42f6edafd13116e0e12565202fef8e9ece7dce03812464d04b9442de", zw);
    ok("P-256 ECDH shared secret (RFC 5903)", !memcmp(za, zw, 32) && !memcmp(zb, zw, 32));
    /* корни Mozilla */
    x509_roots_file = "certs/roots.der";
    load_roots();
    int rsa = 0, ecn = 0;
    for (int i = 0; i < n_roots; i++) { if (rc[i].key == K_RSA) rsa++; else if (rc[i].key) ecn++; }
    printf("    roots: %d (RSA %d, EC %d)\n", n_roots, rsa, ecn);
    ok("Mozilla roots parsed", n_roots > 100 && rsa > 60 && ecn > 30);
    if (argc > 1) {                                       /* тестовые цепочки */
        const char *dir = argv[1];
        char p[512], err[128], iss[64];
        snprintf(p, sizeof(p), "%s/roots.der", dir);
        static char rf[512];
        snprintf(rf, sizeof(rf), "%s", p);
        x509_roots_file = rf;
        free(roots); free(rc); roots = NULL; rc = NULL; n_roots = 0; roots_len = -1;
        int r = chain(dir, "chainA.chain", "example.com", err, iss);
        printf("    chainA: %s / %s\n", r ? err : "ok", iss);
        ok("EC P-256 <- EC P-384 <- RSA 4096 root", r == 0);
        r = chain(dir, "chainB.chain", "www.example.com", err, iss);
        printf("    chainB: %s / %s\n", r ? err : "ok", iss);
        ok("RSA 2048 <- RSA <- EC P-384 root, *.example.com", r == 0);
        ok("wrong host is refused", chain(dir, "chainA.chain", "evil.com", err, iss) != 0);
        printf("    %s\n", err);
        ok("wildcard does not match two labels", chain(dir, "chainA.chain", "a.b.example.com", err, iss) != 0);
        ok("expired is refused", chain(dir, "expired.chain", "example.com", err, iss) != 0);
        printf("    %s\n", err);
        ok("self-signed is refused", chain(dir, "self.chain", "example.com", err, iss) != 0);
        printf("    %s\n", err);
        ok("missing intermediate is refused", chain(dir, "nointer.chain", "example.com", err, iss) != 0);
        ok("wrong intermediate is refused", chain(dir, "swapped.chain", "example.com", err, iss) != 0);
        int mn;
        u8 *msg = load(dir, "msg.bin", &mn);
        for (int i = 0; i < 5; i++) {
            static const int sch[5] = { 0x0403, 0x0804, 0x0805, 0x0401, 0x0503 };
            char f1[16], f2[16];
            snprintf(f1, sizeof(f1), "sig%d.bin", i); snprintf(f2, sizeof(f2), "sig%d.der", i);
            int sl, cl;
            u8 *s = load(dir, f1, &sl), *c = load(dir, f2, &cl);
            char what[64];
            snprintf(what, sizeof(what), "CertificateVerify scheme %04x", sch[i]);
            ok(what, x509_verify_tls(c, cl, sch[i], s, sl, msg, mn) == 0);
            s[sl / 2] ^= 1;
            snprintf(what, sizeof(what), "  ...damaged signature refused");
            ok(what, x509_verify_tls(c, cl, sch[i], s, sl, msg, mn) != 0);
            free(s); free(c);
        }
        free(msg);
    }
    printf(fails ? "%d FAILED\n" : "ALL X509 TESTS PASSED\n", fails);
    return fails != 0;
}
