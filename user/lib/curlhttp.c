#include "myos.h"
#include "strings.h"
#include "curlhttp.h"

static int started;

static int der_span(const unsigned char *p, int n, int *span) {
    int nbytes, len, i;
    if (n < 2 || p[0] != 0x30) return -1;
    if (p[1] < 0x80) *span = 2 + p[1];
    else {
        nbytes = p[1] & 0x7f;
        if (nbytes < 1 || nbytes > 3 || n < 2 + nbytes) return -1;
        len = 0;
        for (i = 0; i < nbytes; i++) len = (len << 8) | p[2 + i];
        if (len < 0) return -1;
        *span = 2 + nbytes + len;
    }
    if (*span < 2 || *span > n) return -1;
    return 0;
}

static int putmem(char *dst, int cap, int o, const void *s, int n) {
    if (n < 0 || o < 0 || n > cap - o) return -1;
    memcpy(dst + o, s, (size_t)n);
    return o + n;
}

static int b64(char *dst, int cap, int o, const unsigned char *src, int n) {
    static const char tab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int i = 0, col = 0;
    while (i + 2 < n) {
        unsigned v = ((unsigned)src[i] << 16) | ((unsigned)src[i + 1] << 8) | src[i + 2];
        char chunk[4];
        chunk[0] = tab[(v >> 18) & 63];
        chunk[1] = tab[(v >> 12) & 63];
        chunk[2] = tab[(v >> 6) & 63];
        chunk[3] = tab[v & 63];
        o = putmem(dst, cap, o, chunk, 4);
        if (o < 0) return -1;
        i += 3;
        col += 4;
        if (col == 64) {
            o = putmem(dst, cap, o, "\n", 1);
            if (o < 0) return -1;
            col = 0;
        }
    }
    if (i < n) {
        unsigned v = (unsigned)src[i] << 16;
        char chunk[4];
        if (i + 1 < n) v |= (unsigned)src[i + 1] << 8;
        chunk[0] = tab[(v >> 18) & 63];
        chunk[1] = tab[(v >> 12) & 63];
        chunk[2] = (i + 1 < n) ? tab[(v >> 6) & 63] : '=';
        chunk[3] = '=';
        o = putmem(dst, cap, o, chunk, 4);
        if (o < 0) return -1;
        o = putmem(dst, cap, o, "\n", 1);
    } else if (col) o = putmem(dst, cap, o, "\n", 1);
    return o;
}

/* Сертификаты DER подряд -> PEM, который понимает OpenSSL. */
static char *der_to_pem(const unsigned char *der, int n, int *outlen) {
    int i = 0, certs = 0, cap, o = 0;
    char *pem;
    while (i < n) {
        int span;
        if (der_span(der + i, n - i, &span) < 0) return NULL;
        i += span;
        certs++;
    }
    if (!certs) return NULL;
    cap = n * 2 + certs * 80;
    pem = malloc((size_t)cap + 1);
    if (!pem) return NULL;
    i = 0;
    while (i < n) {
        int span;
        der_span(der + i, n - i, &span);
        o = putmem(pem, cap, o, "-----BEGIN CERTIFICATE-----\n", 28);
        if (o >= 0) o = b64(pem, cap, o, der + i, span);
        if (o >= 0) o = putmem(pem, cap, o, "-----END CERTIFICATE-----\n", 26);
        if (o < 0) { free(pem); return NULL; }
        i += span;
    }
    pem[o] = 0;
    *outlen = o;
    return pem;
}

static unsigned char *read_file(const char *path, int *n) {
    int fd = open(path, O_READ), cap = 4096, len = 0;
    unsigned char *b;
    if (fd < 0) return NULL;
    b = malloc((size_t)cap);
    if (!b) { close(fd); return NULL; }
    for (;;) {
        int r;
        if (len == cap) {
            unsigned char *g;
            cap *= 2;
            g = realloc(b, (size_t)cap);
            if (!g) { free(b); close(fd); return NULL; }
            b = g;
        }
        r = read(fd, b + len, (size_t)(cap - len));
        if (r < 0) { free(b); close(fd); return NULL; }
        if (r == 0) break;
        len += r;
    }
    close(fd);
    *n = len;
    return b;
}

static int load_roots(CURL *c) {
    int n = 0, pem_len = 0;
    unsigned char *der = read_file("/etc/certs.der", &n);
    char *pem;
    struct curl_blob blob;
    CURLcode rc;
    if (!der) return -1;
    pem = der_to_pem(der, n, &pem_len);
    free(der);
    if (!pem) return -1;
    blob.data = pem;
    blob.len = (size_t)pem_len;
    blob.flags = CURL_BLOB_COPY;
    rc = curl_easy_setopt(c, CURLOPT_CAINFO_BLOB, &blob);
    free(pem);
    return rc == CURLE_OK ? 0 : -2;
}

int curlhttp_prepare(CURL *c, const char *url, const char *useragent, int insecure) {
    int https = !strncmp(url, "https://", 8);
    if (!started) {
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != 0) return -2;
        started = 1;
    }
    if (curl_easy_setopt(c, CURLOPT_URL, url) != CURLE_OK) return -2;
    if (curl_easy_setopt(c, CURLOPT_USERAGENT, useragent) != CURLE_OK) return -2;
    if (curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L) != CURLE_OK) return -2;
    if (curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "http,https") != CURLE_OK) return -2;
    if (insecure) {
        if (curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L) != CURLE_OK) return -2;
        if (curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L) != CURLE_OK) return -2;
    } else if (https) {
        int r = load_roots(c);
        if (r < 0) return r;
    }
    return 0;
}

size_t curlhttp_on_header(char *ptr, size_t size, size_t nmemb, void *ud) {
    struct curlhttp_hdr *h = ud;
    size_t n = size * nmemb, i = 0;
    if (n >= 5 && !memcmp(ptr, "HTTP/", 5)) {
        while (i < n && i + 1 < sizeof h->line && ptr[i] != '\r' && ptr[i] != '\n') {
            h->line[i] = ptr[i];
            i++;
        }
        h->line[i] = 0;
        {
            char *sp = strchr(h->line, ' ');
            h->status = sp ? atoi(sp + 1) : 0;
        }
        h->location[0] = 0;
    } else if (n > 9 && !strncasecmp(ptr, "Location:", 9)) {
        const char *v = ptr + 9;
        size_t left = n - 9;
        while (left && (*v == ' ' || *v == '\t')) { v++; left--; }
        i = 0;
        while (i < left && i + 1 < sizeof h->location && v[i] != '\r' && v[i] != '\n') {
            h->location[i] = v[i];
            i++;
        }
        h->location[i] = 0;
    }
    return n;
}
