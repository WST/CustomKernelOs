/* wget [-O file] [-q] http(s)://host[:port]/path — скачать файл по HTTP или HTTPS
 *   -O file   куда сохранить (-O - = на экран); по умолчанию — имя из адреса
 *   -q        молча (без строки прогресса)
 * HTTPS — через tls.c (TLS 1.3); сертификат сервера пока не проверяется. */
#include "myos.h"
#include "tls.h"

static char host[128], path[256];
static int port = 80;
static bool https;
static tls *conn;                       /* не NULL — читаем/пишем через TLS */

static int net_read(int fd, void *b, int n) { return conn ? tls_read(conn, b, n) : read(fd, b, n); }
static int net_write(int fd, const void *b, int n) { return conn ? tls_write(conn, b, n) : write(fd, b, n); }

static int parse_url(const char *url) {
    if (!strncmp(url, "http://", 7)) url += 7;
    else if (!strncmp(url, "https://", 8)) { url += 8; https = true; port = 443; }
    int i = 0;
    while (*url && *url != '/' && *url != ':' && i < (int)sizeof(host) - 1) host[i++] = *url++;
    host[i] = 0;
    if (!i) return -1;
    if (*url == ':') { url++; port = atoi(url); while (isdigit(*url)) url++; }
    if (port <= 0 || port > 65535) return -1;
    if (*url && *url != '/') return -1;
    snprintf(path, sizeof(path), "%s", *url ? url : "/");
    return 0;
}

/* читаем ответ до пустой строки (конец заголовков); остаток тела — в buf */
static char resp[4096];
static int resp_len, body_off;

static int read_headers(int fd) {
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

/* найти заголовок (без учёта регистра) -> значение до конца строки */
static bool header(const char *name, char *out, int max) {
    int nl = strlen(name);
    for (char *p = resp; p < resp + body_off; ) {
        char *e = p;
        while (e < resp + body_off && *e != '\r') e++;
        bool same = e - p > nl && p[nl] == ':';
        for (int i = 0; same && i < nl; i++) if (toupper(p[i]) != toupper(name[i])) same = false;
        if (same) {
            char *v = p + nl + 1;
            while (*v == ' ') v++;
            int l = e - v < max - 1 ? e - v : max - 1;
            memcpy(out, v, l); out[l] = 0;
            return true;
        }
        p = e + 2;
    }
    return false;
}

int main(int argc, char **argv) {
    const char *out = NULL, *url = NULL;
    bool quiet = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-O") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "-q")) quiet = true;
        else url = argv[i];
    }
    if (!url || parse_url(url) < 0) { dprintf(STDERR, "usage: wget [-q] [-O file|-] http[s]://host[:port]/path\n"); return 2; }

    char name[32];
    if (!out) {                                             /* имя файла — последний кусок пути */
        const char *s = strrchr(path, '/') + 1;
        int l = 0;
        while (s[l] && s[l] != '?' && l < (int)sizeof(name) - 1) { name[l] = s[l]; l++; }
        name[l] = 0;
        if (!l) strcpy(name, "index.html");
        out = name;
    }
    bool to_stdout = !strcmp(out, "-");
    if (to_stdout) quiet = true;

    if (!quiet) dprintf(STDERR, "Connecting to %s:%d... ", host, port);
    int fd = tcp_connect(host, port);
    if (fd < 0) {
        if (quiet) dprintf(STDERR, "wget: %s: ", host);
        dprintf(STDERR, "%s\n", fd == -ENOENT ? "host not found" : strerror(-fd));
        return 1;
    }
    if (!quiet) dprintf(STDERR, "connected.\n");
    if (https) {
        char err[96];
        if (!quiet) dprintf(STDERR, "TLS handshake... ");
        conn = tls_open(fd, host, err, sizeof(err));
        if (!conn) { dprintf(STDERR, "%swget: TLS: %s\n", quiet ? "" : "\n", err); close(fd); return 1; }
        if (!quiet) dprintf(STDERR, "%s (certificate not verified)\n", tls_info(conn));
    }

    char req[512];
    int rl = snprintf(req, sizeof(req),
        "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: MyOS-wget/0.13\r\nAccept: */*\r\nConnection: close\r\n\r\n",
        path, host);
    if (net_write(fd, req, rl) != rl) { dprintf(STDERR, "wget: send failed\n"); close(fd); return 1; }

    int r = read_headers(fd);
    if (r < 0) { dprintf(STDERR, "wget: bad response: %s\n", strerror(-r)); close(fd); return 1; }
    char *eol = strchr(resp, '\r');
    *eol = 0;
    int status = 0;
    char *sp = strchr(resp, ' ');
    if (sp) status = atoi(sp + 1);
    if (!quiet) dprintf(STDERR, "%s\n", resp);
    *eol = '\r';
    char loc[200], lenh[16];
    if (status >= 300 && status < 400 && header("Location", loc, sizeof(loc))) {
        dprintf(STDERR, "wget: redirected to %s\n", loc);
        close(fd);
        return 1;
    }
    unsigned total = header("Content-Length", lenh, sizeof(lenh)) ? (unsigned)atoi(lenh) : 0;

    int ofd = to_stdout ? STDOUT : open(out, O_WRITE);
    if (ofd < 0) { dprintf(STDERR, "wget: %s: %s\n", out, strerror(-ofd)); close(fd); return 1; }

    unsigned got = 0, t0 = ticks(), last = 0;
    if (resp_len > body_off) { write(ofd, resp + body_off, resp_len - body_off); got = resp_len - body_off; }
    static char buf[4096];
    int n;
    bool failed = false;
    while ((n = net_read(fd, buf, sizeof(buf))) > 0) {
        int w = write(ofd, buf, n);
        if (w != n) { dprintf(STDERR, "\nwget: write to %s: %s\n", out, strerror(w < 0 ? -w : ENOSPC)); failed = true; n = 0; break; }
        got += n;
        if (!quiet && ticks() - last > 200) {
            last = ticks();
            if (total) dprintf(STDERR, "\r%u / %u bytes (%u%%)", got, total, total < 40000000 ? got * 100 / total : got / (total / 100));
            else dprintf(STDERR, "\r%u bytes", got);
        }
    }
    if (conn) tls_close(conn);
    close(fd);
    if (!to_stdout) close(ofd);
    if (n < 0) { dprintf(STDERR, "\nwget: %s\n", conn ? "TLS error" : strerror(-n)); return 1; }
    if (failed) return 1;
    unsigned ms = ticks() - t0;
    if (!quiet) {
        dprintf(STDERR, "\r'%s' saved: %u bytes in %u.%02u s", out, got, ms / 1000, ms % 1000 / 10);
        if (got >= 10240) dprintf(STDERR, " (%u KB/s)", got / (ms ? ms : 1));
        dprintf(STDERR, "\n");
    }
    return status >= 200 && status < 300 ? 0 : 1;
}
