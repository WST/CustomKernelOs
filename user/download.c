/* download [-r N] [-o file] [-k] http(s)://host[:port]/path
 * Родная загрузка одного файла. Не заменяет wget.
 *   -r N    сколько раз повторить после неудачи (по умолчанию 0)
 *   -o file куда сохранить; иначе имя берётся из адреса
 *   -k      не проверять сертификат HTTPS
 * До пяти перенаправлений за одну попытку. Готовый файл публикуется переименованием,
 * поэтому оборванная загрузка не затирает уже лежащий файл.
 * Передача — libcurl. Корни сертификатов те же, /etc/certs.der. */
#include "myos.h"
#include "unistd.h"
#include "stdio.h"
#include "curlhttp.h"

struct sink {
    int fd, err;
};

static size_t on_body(char *ptr, size_t size, size_t nmemb, void *ud) {
    struct sink *s = ud;
    size_t n = size * nmemb, off = 0;
    while (off < n) {
        size_t chunk = n - off;
        int w;
        if (chunk > (1u << 20)) chunk = 1u << 20;
        w = write(s->fd, ptr + off, chunk);
        if (w <= 0) { s->err = w < 0 ? errno : ENOSPC; return 0; }
        off += (size_t)w;
    }
    return n;
}

static size_t on_hdr(char *ptr, size_t size, size_t nmemb, void *ud) {
    struct curlhttp_hdr *h = ud;
    size_t n = curlhttp_on_header(ptr, size, nmemb, h);
    if (n >= 5 && !memcmp(ptr, "HTTP/", 5)) dprintf(STDERR, "download: %s\n", h->line);
    return n;
}

static int http_url(const char *url) {
    return !strncmp(url, "http://", 7) || !strncmp(url, "https://", 8);
}

static int cert_fail(CURLcode rc) {
    return rc == CURLE_PEER_FAILED_VERIFICATION || rc == CURLE_SSL_CACERT_BADFILE;
}

/* Reserve a unique sibling, without truncating any existing path. */
static int reserve_sibling(const char *out, char temp[128]) {
    static unsigned serial;
    const char *slash = strrchr(out, '/');
    int prefix = slash ? (int)(slash - out) + 1 : 0;
    if (prefix + 28 >= 128) { errno = ENAMETOOLONG; return -1; }
    memcpy(temp, out, (size_t)prefix);
    for (int i = 0; i < 32; i++) {
        snprintf(temp + prefix, 128 - prefix, ".dl%u-%u", (unsigned)getpid(), ++serial);
        int fd = open(temp, O_WRONLY | O_CREAT | O_EXCL);
        if (fd >= 0 || errno != EEXIST) return fd;
    }
    errno = EEXIST; return -1;
}

/* This MyFS rename does not replace an existing file. Keep a recoverable
 * backup until the completed download has reached its final name. */
static int publish_download(const char *temp, const char *out) {
    struct dirent_info st;
    int r = dirstat(out, &st);
    if (r == -ENOENT) return rename(temp, out);
    if (r < 0) { errno = -r; return -1; }
    if (st.type != 1) { errno = EISDIR; return -1; }
    char backup[128];
    int fd = reserve_sibling(out, backup);
    if (fd < 0) return -1;
    if (close(fd) < 0) { unlink(backup); return -1; }
    if (unlink(backup) < 0) return -1;
    if (rename(out, backup) < 0) return -1;
    if (rename(temp, out) < 0) {
        int saved = errno;
        if (rename(backup, out) < 0)
            dprintf(STDERR, "download: original file retained at %s\n", backup);
        errno = saved; return -1;
    }
    if (unlink(backup) < 0)
        dprintf(STDERR, "download: old file retained at %s\n", backup);
    return 0;
}

/* 0 — файл записан, -2 — сертификат, -1 — прочая ошибка. */
static int once(const char *url, const char *temp, int insecure) {
    CURL *c;
    CURLcode rc;
    char err[256];
    long code = 0;
    struct sink sink;
    struct curlhttp_hdr hdr;
    int fd, prepared;
    if (!http_url(url)) { dprintf(STDERR, "download: bad url\n"); return -1; }
    fd = open(temp, O_WRONLY | O_TRUNC);
    if (fd < 0) { dprintf(STDERR, "download: %s: %s\n", temp, strerror(errno)); return -1; }
    c = curl_easy_init();
    if (!c) { close(fd); dprintf(STDERR, "download: out of memory\n"); return -1; }
    memset(&sink, 0, sizeof sink);
    memset(&hdr, 0, sizeof hdr);
    sink.fd = fd;
    err[0] = 0;
    curl_easy_setopt(c, CURLOPT_ERRORBUFFER, err);
    prepared = curlhttp_prepare(c, url, "MyOS-download/0.1", insecure);
    if (prepared < 0) {
        curl_easy_cleanup(c);
        close(fd);
        if (prepared == -1)
            dprintf(STDERR, "download: certificate problem: cannot read /etc/certs.der\n(use -k to download anyway)\n");
        else dprintf(STDERR, "download: out of memory\n");
        return prepared == -1 ? -2 : -1;
    }
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_body);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &sink);
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, on_hdr);
    curl_easy_setopt(c, CURLOPT_HEADERDATA, &hdr);
    rc = curl_easy_perform(c);
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(c);
    close(fd);
    if (prepared == 0 && cert_fail(rc)) {
        dprintf(STDERR, "download: certificate problem: %s\n(use -k to download anyway)\n",
                err[0] ? err : curl_easy_strerror(rc));
        return -2;
    }
    if (sink.err || rc != CURLE_OK || code < 200 || code >= 300) {
        if (sink.err) dprintf(STDERR, "download: write failed\n");
        else if (rc != CURLE_OK) dprintf(STDERR, "download: %s\n", err[0] ? err : curl_easy_strerror(rc));
        else if (!hdr.line[0]) dprintf(STDERR, "download: bad response\n");
        return -1;
    }
    return 0;
}

static int default_name(const char *url, char *name, int max) {
    const char *p, *path, *end, *slash, *s;
    int i;
    if (!http_url(url)) return -1;
    p = strstr(url, "://") + 3;
    path = strchr(p, '/');
    if (!path) { snprintf(name, (size_t)max, "index.html"); return 0; }
    end = path;
    while (*end && *end != '?' && *end != '#') end++;
    slash = path;
    for (s = path; s < end; s++) if (*s == '/') slash = s;
    s = slash + 1;
    if (s >= end) { snprintf(name, (size_t)max, "index.html"); return 0; }
    i = (int)(end - s);
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
        char temporary[128];
        int temp_fd = reserve_sibling(out, temporary);
        int r;
        if (temp_fd < 0) {
            dprintf(STDERR, "download: cannot create temporary file: %s\n", strerror(errno));
            return 1;
        }
        if (close(temp_fd) < 0) { unlink(temporary); return 1; }
        r = once(url, temporary, insecure);
        if (r == 0) {
            if (publish_download(temporary, out) == 0) { dprintf(STDERR, "download: saved %s\n", out); return 0; }
            dprintf(STDERR, "download: cannot publish %s: %s\n", out, strerror(errno));
            unlink(temporary);
            return 1;
        }
        unlink(temporary);
        if (r == -2) return 1;
        if (attempt < retries) {
            dprintf(STDERR, "download: retry %d/%d\n", attempt + 1, retries);
            sleep(1);
        }
    }
    return 1;
}
