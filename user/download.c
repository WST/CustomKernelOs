/* download [-r N] [-o file] [-k] http(s)://host[:port]/path
 * Родная загрузка одного файла. Не заменяет wget.
 *   -r N    сколько раз повторить после неудачи (по умолчанию 0)
 *   -o file куда сохранить; иначе имя берётся из адреса
 *   -k      не проверять сертификат HTTPS
 * До пяти перенаправлений за одну попытку. */
#include "myos.h"
#include "unistd.h"
#include "tls.h"

static char host[128];
static char reqpath[512];
static int port, https;
static tls *conn;

static int net_read(int fd, void *b, int n) { return conn ? tls_read(conn, b, n) : read(fd, b, n); }
static int net_write(int fd, const void *b, int n) { return conn ? tls_write(conn, b, n) : write(fd, b, n); }

static int parse_url(const char *url) {
    https = 0;
    port = 80;
    host[0] = 0;
    if (!strncmp(url, "http://", 7)) url += 7;
    else if (!strncmp(url, "https://", 8)) { url += 8; https = 1; port = 443; }
    else return -1;
    int i = 0;
    while (*url && *url != '/' && *url != ':' && i < (int)sizeof(host) - 1) host[i++] = *url++;
    host[i] = 0;
    if (!i) return -1;
    if (*url == ':') {
        url++;
        port = atoi(url);
        while (isdigit((unsigned char)*url)) url++;
    }
    if (port <= 0 || port > 65535) return -1;
    if (*url && *url != '/') return -1;
    snprintf(reqpath, sizeof(reqpath), "%s", *url ? url : "/");
    return 0;
}

static char resp[4096];
static int resp_len, body_off;

static int read_headers(int fd) {
    resp_len = 0;
    body_off = 0;
    while (resp_len < (int)sizeof(resp) - 1) {
        int n = net_read(fd, resp + resp_len, sizeof(resp) - 1 - resp_len);
        if (n <= 0) return n < 0 ? n : -EIO;
        resp_len += n;
        resp[resp_len] = 0;
        for (int i = 0; i + 3 < resp_len; i++)
            if (resp[i] == '\r' && resp[i + 1] == '\n' && resp[i + 2] == '\r' && resp[i + 3] == '\n') {
                body_off = i + 4;
                return 0;
            }
    }
    return -EMSGSIZE;
}

static int header(const char *name, char *out, int max) {
    int nl = (int)strlen(name);
    for (char *p = resp; p < resp + body_off; ) {
        char *e = p;
        int same;
        while (e < resp + body_off && *e != '\r') e++;
        same = e - p > nl && p[nl] == ':';
        for (int i = 0; same && i < nl; i++) if (toupper((unsigned char)p[i]) != toupper((unsigned char)name[i])) same = 0;
        if (same) {
            char *v = p + nl + 1;
            int l;
            while (*v == ' ') v++;
            l = e - v < max - 1 ? (int)(e - v) : max - 1;
            memcpy(out, v, (size_t)l);
            out[l] = 0;
            return 1;
        }
        p = e + 2;
    }
    return 0;
}

/* Новый адрес пишется в cur. 0 — получилось. */
static int resolve_loc(char *cur, int cap, const char *loc) {
    char base[512];
    char *slash;
    if (!strncmp(loc, "http://", 7) || !strncmp(loc, "https://", 8)) {
        snprintf(cur, (size_t)cap, "%s", loc);
        return 0;
    }
    if (loc[0] == '/') {
        snprintf(cur, (size_t)cap, "%s://%s:%d%s", https ? "https" : "http", host, port, loc);
        return 0;
    }
    snprintf(base, sizeof base, "%s", reqpath);
    slash = strrchr(base, '/');
    if (slash) slash[1] = 0;
    else snprintf(base, sizeof base, "/");
    snprintf(cur, (size_t)cap, "%s://%s:%d%s%s", https ? "https" : "http", host, port, base, loc);
    return 0;
}

static void close_net(int fd) {
    if (conn) { tls_close(conn); conn = NULL; }
    if (fd >= 0) close(fd);
}

/* 0 — файл записан, 2 — в cur новый адрес, -2 — сертификат, -1 — прочая ошибка. */
static int once(char *cur, const char *out, int insecure) {
    int fd, ofd, status, n, r;
    char req[640], loc[640];
    char *eol, *sp;
    unsigned got = 0;
    if (parse_url(cur) < 0) { dprintf(STDERR, "download: bad url\n"); return -1; }
    fd = tcp_connect(host, port);
    if (fd < 0) {
        dprintf(STDERR, "download: %s: %s\n", host, fd == -ENOENT ? "host not found" : strerror(-fd));
        return -1;
    }
    if (https) {
        char err[96];
        conn = tls_open(fd, host, err, sizeof err);
        if (!conn) { dprintf(STDERR, "download: TLS: %s\n", err); close(fd); return -1; }
        if (!tls_cert_ok(conn) && !insecure) {
            dprintf(STDERR, "download: certificate problem: %s\n(use -k to download anyway)\n", tls_cert_error(conn));
            close_net(fd);
            return -2;
        }
    }
    n = snprintf(req, sizeof req,
        "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: MyOS-download/0.1\r\nAccept: */*\r\nConnection: close\r\n\r\n",
        reqpath, host);
    if (n < 0 || n >= (int)sizeof req || net_write(fd, req, n) != n) {
        dprintf(STDERR, "download: send failed\n");
        close_net(fd);
        return -1;
    }
    r = read_headers(fd);
    if (r < 0) { dprintf(STDERR, "download: bad response: %s\n", strerror(-r)); close_net(fd); return -1; }
    eol = strchr(resp, '\r');
    if (!eol) { close_net(fd); return -1; }
    *eol = 0;
    status = 0;
    sp = strchr(resp, ' ');
    if (sp) status = atoi(sp + 1);
    dprintf(STDERR, "download: %s\n", resp);
    *eol = '\r';
    if (status >= 300 && status < 400 && header("Location", loc, sizeof loc)) {
        close_net(fd);
        resolve_loc(cur, 768, loc);
        return 2;
    }
    if (status < 200 || status >= 300) { close_net(fd); return -1; }

    ofd = open(out, O_WRITE);
    if (ofd < 0) { dprintf(STDERR, "download: %s: %s\n", out, strerror(errno)); close_net(fd); return -1; }
    if (resp_len > body_off) {
        int w = write(ofd, resp + body_off, (size_t)(resp_len - body_off));
        if (w != resp_len - body_off) { dprintf(STDERR, "download: write failed\n"); close(ofd); close_net(fd); return -1; }
        got = (unsigned)(resp_len - body_off);
    }
    while ((n = net_read(fd, req, 512)) > 0) {
        int w = write(ofd, req, (size_t)n);
        if (w != n) { dprintf(STDERR, "download: write failed\n"); close(ofd); close_net(fd); return -1; }
        got += (unsigned)n;
    }
    close(ofd);
    close_net(fd);
    if (n < 0) { dprintf(STDERR, "download: %s\n", strerror(-n)); return -1; }
    dprintf(STDERR, "download: saved %s (%u bytes)\n", out, got);
    return 0;
}

static int default_name(const char *url, char *name, int max) {
    const char *s, *slash;
    int i = 0;
    if (parse_url(url) < 0) return -1;
    slash = strrchr(reqpath, '/');
    s = slash ? slash + 1 : reqpath;
    while (s[i] && s[i] != '?' && s[i] != '#') i++;
    if (i == 0) { snprintf(name, (size_t)max, "index.html"); return 0; }
    if (i >= max) { dprintf(STDERR, "download: name is longer than %d, use -o\n", max - 1); return -1; }
    memcpy(name, s, (size_t)i);
    name[i] = 0;
    return 0;
}

static void usage(void) {
    dprintf(STDERR, "usage: download [-r N] [-o file] [-k] http[s]://host[:port]/path\n");
}

int main(int argc, char **argv) {
    int retries = 0, insecure = 0, opt, attempt;
    const char *out = NULL, *url = NULL;
    char name[28];
    char cur[768];
    opterr = 1;
    while ((opt = getopt(argc, argv, "r:o:k")) != -1) {
        if (opt == 'r') {
            const char *p = optarg;
            retries = 0;
            if (!p || !isdigit((unsigned char)*p)) { usage(); return 2; }
            while (isdigit((unsigned char)*p)) { retries = retries * 10 + (*p - '0'); p++; }
            if (*p || retries > 20) { usage(); return 2; }
        } else if (opt == 'o') out = optarg;
        else if (opt == 'k') insecure = 1;
        else { usage(); return 2; }
    }
    if (optind + 1 != argc) { usage(); return 2; }
    url = argv[optind];
    if (!out) {
        if (default_name(url, name, (int)sizeof name) < 0) return 2;
        out = name;
    }
    for (attempt = 0; attempt <= retries; attempt++) {
        int red;
        snprintf(cur, sizeof cur, "%s", url);
        for (red = 0; red < 5; red++) {
            int r = once(cur, out, insecure);
            if (r == 0) return 0;
            if (r == 2) continue;
            if (r == -2) return 1;
            break;
        }
        unlink(out);
        if (attempt < retries) {
            dprintf(STDERR, "download: retry %d/%d\n", attempt + 1, retries);
            sleep(1);
        }
    }
    return 1;
}
