/* Хостовая проверка libcurl: GET по HTTP и HTTPS на локальный сервер.
 * Линкует gcc, потому что объекты curl и OpenSSL несут R_386_GOT32X.
 * _start нужен: в linux-архиве libc нет входной точки cc. */
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char body[512];
static size_t body_len;

static size_t take(char *ptr, size_t size, size_t nmemb, void *ud) {
    size_t n = size * nmemb;
    size_t i;
    (void)ud;
    for (i = 0; i < n && body_len + 1 < sizeof(body); i++) body[body_len++] = ptr[i];
    body[body_len] = 0;
    return n;
}

static int get(const char *url, int tls) {
    char err[256];
    CURL *c = curl_easy_init();
    long code = 0;
    CURLcode rc;
    body_len = 0;
    body[0] = 0;
    err[0] = 0;
    if (!c) return 1;
    curl_easy_setopt(c, CURLOPT_ERRORBUFFER, err);
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, take);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 5L);
    if (tls) {
        curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
    }
    rc = curl_easy_perform(c);
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(c);
    if (rc != CURLE_OK || code != 200 || !strstr(body, "myos-curl-ok")) {
        printf("%s: fail rc=%d http=%ld body=%s err=%s\n", url, (int)rc, code, body, err);
        return 1;
    }
    return 0;
}

int main(void) {
    int bad = 0;
    curl_global_init(0);
    bad |= get("http://127.0.0.1:18765/", 0);
    bad |= get("https://127.0.0.1:18766/", 1);
    if (!bad) printf("curl: ok\n");
    return bad ? 1 : 0;
}

void _start(void) { exit(main()); }
