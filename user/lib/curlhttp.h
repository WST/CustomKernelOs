/* Общая обвязка libcurl для wget и download.
 * Корни HTTPS — тот же /etc/certs.der, что у браузера: перед запросом он
 * перекладывается в PEM и передаётся CURLOPT_CAINFO_BLOB. */
#ifndef CURLHTTP_H
#define CURLHTTP_H
#include <sys/select.h>
#include <curl/curl.h>

struct curlhttp_hdr {
    int status;
    char line[160];
    char location[512];
};

/* 0 — можно вызывать curl_easy_perform.
 * -1 — https, а /etc/certs.der не прочитался или в нём не сертификаты.
 * -2 — libcurl не поднялся или кончилась память. */
int curlhttp_prepare(CURL *c, const char *url, const char *useragent, int insecure);

/* Заголовок ответа: статусная строка и Location. Тело не трогает. */
size_t curlhttp_on_header(char *ptr, size_t size, size_t nmemb, void *ud);
#endif
