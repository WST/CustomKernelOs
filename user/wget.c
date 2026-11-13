/* wget [-O file] [-q] [-k] http(s)://host[:port]/path — скачать файл по HTTP или HTTPS.
 *   -O file   куда сохранить (-O - = на экран); по умолчанию — имя из адреса
 *   -q        молча (без строки прогресса)
 *   -k        не проверять сертификат HTTPS (как --no-check-certificate)
 * Передача — libcurl. Корни сертификатов те же, /etc/certs.der. */
#include "myos.h"
#include "curlhttp.h"

struct got {
    struct curlhttp_hdr hdr;
    const char *out;
    int fd, opened, to_stdout, quiet, err, said;
    unsigned bytes, t0, last;
};

static int discarding(struct got *g) {
    return g->hdr.status >= 300 && g->hdr.status < 400 && g->hdr.location[0];
}

static int ensure_open(struct got *g) {
    if (g->opened) return 0;
    if (g->to_stdout) { g->fd = STDOUT; g->opened = 1; return 0; }
    g->fd = open(g->out, O_WRITE);
    if (g->fd < 0) {
        dprintf(STDERR, "\nwget: %s: %s\n", g->out, strerror(errno));
        g->err = 1;
        return -1;
    }
    g->opened = 1;
    return 0;
}

static size_t on_body(char *ptr, size_t size, size_t nmemb, void *ud) {
    struct got *g = ud;
    size_t n = size * nmemb, off = 0;
    if (discarding(g)) return n;
    if (ensure_open(g) < 0) return 0;
    while (off < n) {
        size_t chunk = n - off;
        int w;
        if (chunk > (1u << 20)) chunk = 1u << 20;
        w = write(g->fd, ptr + off, chunk);
        if (w <= 0) {
            dprintf(STDERR, "\nwget: write to %s: %s\n", g->out, strerror(w < 0 ? errno : ENOSPC));
            g->err = 1;
            return 0;
        }
        off += (size_t)w;
    }
    g->bytes += (unsigned)n;
    return n;
}

static size_t on_hdr(char *ptr, size_t size, size_t nmemb, void *ud) {
    struct got *g = ud;
    size_t n = curlhttp_on_header(ptr, size, nmemb, &g->hdr);
    if (!g->said && n >= 5 && !memcmp(ptr, "HTTP/", 5)) {
        g->said = 1;
        if (!g->quiet) dprintf(STDERR, "connected.\n%s\n", g->hdr.line);
    }
    return n;
}

static int on_progress(void *ud, curl_off_t dltotal, curl_off_t dlnow,
                       curl_off_t ultotal, curl_off_t ulnow) {
    struct got *g = ud;
    unsigned now, got, total;
    (void)ultotal; (void)ulnow;
    if (g->quiet || dlnow <= 0) return 0;
    now = ticks();
    if (dltotal != dlnow && now - g->last < 200) return 0;
    g->last = now;
    got = dlnow > 0xffffffffLL ? 0xffffffffu : (unsigned)dlnow;
    if (dltotal > 0) {
        total = dltotal > 0xffffffffLL ? 0xffffffffu : (unsigned)dltotal;
        dprintf(STDERR, "\r%u / %u bytes (%u%%)", got, total,
                total < 40000000u ? got * 100u / total : got / (total / 100u));
    } else dprintf(STDERR, "\r%u bytes", got);
    return 0;
}

static int https_url(const char *url) { return !strncmp(url, "https://", 8); }
static int http_url(const char *url) { return https_url(url) || !strncmp(url, "http://", 7); }

static void where(const char *url, char *host, int max, int *port) {
    const char *p = url;
    int i = 0;
    if (https_url(p)) p += 8;
    else if (!strncmp(p, "http://", 7)) p += 7;
    while (*p && *p != '/' && *p != ':' && *p != '?' && i < max - 1) host[i++] = *p++;
    host[i] = 0;
    *port = https_url(url) ? 443 : 80;
    if (*p == ':') {
        int v = atoi(p + 1);
        if (v > 0 && v <= 65535) *port = v;
    }
}

static void file_name(const char *url, char *name) {
    const char *p = strstr(url, "://"), *path, *end, *slash, *s;
    int l = 0;
    p = p ? p + 3 : url;
    path = strchr(p, '/');
    if (!path) { strcpy(name, "index.html"); return; }
    end = path;
    while (*end && *end != '?' && *end != '#') end++;
    slash = path;
    for (s = path; s < end; s++) if (*s == '/') slash = s;
    s = slash + 1;
    if (s >= end) { strcpy(name, "index.html"); return; }
    while (s < end && l < 31) name[l++] = *s++;
    name[l] = 0;
    if (!l) strcpy(name, "index.html");
}

static int cert_fail(CURLcode rc) {
    return rc == CURLE_PEER_FAILED_VERIFICATION || rc == CURLE_SSL_CACERT_BADFILE;
}

static const char *why(CURLcode rc, const char *err) {
    if (err && err[0]) return err;
    if (rc == CURLE_COULDNT_RESOLVE_HOST) return "host not found";
    return curl_easy_strerror(rc);
}

int main(int argc, char **argv) {
    const char *out = NULL, *url = NULL;
    int quiet = 0, insecure = 0;
    char name[32], host[128], err[256];
    int port = 80;
    struct got g;
    CURL *c;
    CURLcode rc;
    int prepared;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-O") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "-q")) quiet = 1;
        else if (!strcmp(argv[i], "-k")) insecure = 1;
        else url = argv[i];
    }
    if (!url || !http_url(url)) {
        dprintf(STDERR, "usage: wget [-q] [-k] [-O file|-] http[s]://host[:port]/path\n");
        return 2;
    }
    if (!out) { file_name(url, name); out = name; }
    if (!strcmp(out, "-")) { quiet = 1; }

    memset(&g, 0, sizeof g);
    g.out = out;
    g.to_stdout = !strcmp(out, "-");
    g.quiet = quiet;
    g.t0 = ticks();
    where(url, host, (int)sizeof host, &port);

    c = curl_easy_init();
    if (!c) { dprintf(STDERR, "wget: out of memory\n"); return 1; }
    err[0] = 0;
    curl_easy_setopt(c, CURLOPT_ERRORBUFFER, err);
    prepared = curlhttp_prepare(c, url, "MyOS-wget/0.14", insecure);
    if (prepared < 0) {
        curl_easy_cleanup(c);
        if (prepared == -1)
            dprintf(STDERR, "wget: certificate problem: cannot read /etc/certs.der\n(use -k to download anyway)\n");
        else dprintf(STDERR, "wget: out of memory\n");
        return 1;
    }
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_body);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &g);
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, on_hdr);
    curl_easy_setopt(c, CURLOPT_HEADERDATA, &g);
    if (!quiet) {
        curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, on_progress);
        curl_easy_setopt(c, CURLOPT_XFERINFODATA, &g);
        dprintf(STDERR, "Connecting to %s:%d... ", host, port);
    }
    rc = curl_easy_perform(c);
    curl_easy_cleanup(c);

    if (cert_fail(rc)) {
        if (!quiet && !g.said) dprintf(STDERR, "\n");
        dprintf(STDERR, "wget: certificate problem: %s\n(use -k to download anyway)\n", why(rc, err));
        if (g.opened && !g.to_stdout) close(g.fd);
        return 1;
    }
    if (g.err) {
        if (g.opened && !g.to_stdout) close(g.fd);
        return 1;
    }
    if (rc != CURLE_OK) {
        if (!g.said) dprintf(STDERR, "%s%s\n", quiet ? "wget: " : "", why(rc, err));
        else dprintf(STDERR, "\nwget: %s\n", why(rc, err));
        if (g.opened && !g.to_stdout) close(g.fd);
        return 1;
    }
    if (discarding(&g)) {
        dprintf(STDERR, "wget: redirected to %s\n", g.hdr.location);
        if (g.opened && !g.to_stdout) close(g.fd);
        return 1;
    }
    if (g.hdr.status >= 200 && ensure_open(&g) < 0) return 1;
    if (g.opened && !g.to_stdout) close(g.fd);
    if (!quiet && g.hdr.status >= 200) {
        unsigned ms = ticks() - g.t0;
        dprintf(STDERR, "\r'%s' saved: %u bytes in %u.%02u s", out, g.bytes, ms / 1000, ms % 1000 / 10);
        if (g.bytes >= 10240) dprintf(STDERR, " (%u KB/s)", g.bytes / (ms ? ms : 1));
        dprintf(STDERR, "\n");
    }
    return g.hdr.status >= 200 && g.hdr.status < 300 ? 0 : 1;
}
