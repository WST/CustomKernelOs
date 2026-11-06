/* tls.h — HTTPS для программ MyOS: TLS 1.3 поверх обычного TCP-сокета.
 *
 *   int fd = tcp_connect("example.com", 443);
 *   char err[96];
 *   tls *t = tls_open(fd, "example.com", err, sizeof(err));
 *   if (!t) printf("TLS: %s\n", err);
 *   tls_write(t, "GET / HTTP/1.0\r\n...", n);
 *   while ((n = tls_read(t, buf, sizeof(buf))) > 0) ...
 *   tls_close(t); close(fd);
 *
 * Обмен ключами X25519, шифры ChaCha20-Poly1305 и AES-128-GCM, хеш SHA-256.
 * Всё написано с нуля (user/lib/tls.c), собирается gcc (нужны 64-битные числа).
 *
 * Сертификат сервера проверяется (x509.c): цепочка до корней из /etc/certs.der,
 * имя сайта, сроки, подписи RSA/ECDSA. Если сервер не умеет X25519 — P-256. */
#ifndef TLS_H
#define TLS_H

typedef struct tls tls;

tls  *tls_open(int fd, const char *host, char *err, int errmax);  /* рукопожатие; NULL — ошибка в err */
int   tls_read(tls *t, void *buf, int len);         /* >0 байт, 0 — конец, <0 — ошибка */
int   tls_write(tls *t, const void *buf, int len);  /* -> len или <0 */
void  tls_close(tls *t);                            /* close_notify и освободить; fd не закрывает */
const char *tls_info(tls *t);                       /* "TLS 1.3, ChaCha20-Poly1305" */

/* Сертификат сервера проверяется при tls_open (x509.c): цепочка до корневого центра из
 * /etc/certs.der, имя сайта, сроки, подпись CertificateVerify. Соединение открывается в
 * любом случае — что делать с непроверенным, решает программа: */
int   tls_cert_ok(tls *t);                          /* 1 — всё в порядке */
const char *tls_cert_error(tls *t);                 /* иначе — почему ("issuer is not trusted: ...") */
const char *tls_cert_issuer(tls *t);                /* кто выдал (имя корневого центра) */
extern int tls_verify;                              /* 0 — не проверять вовсе (быстрее) */

/* Если на сокете стоит тайм-аут чтения (sockctl SOCK_RCVTIMEO), то при каждом тайм-ауте
 * зовётся tls_idle(): вернёт не 0 — ждём дальше, 0 — tls_read/tls_open вернут ошибку.
 * Так программа может, пока ждёт сервер, обрабатывать свои события. */
extern int (*tls_idle)(void);

/* примитивы — для тестов и других программ */
void sha256(const void *data, int n, unsigned char out[32]);
void hmac_sha256(const void *key, int klen, const void *data, int n, unsigned char out[32]);
void x25519(unsigned char out[32], const unsigned char scalar[32], const unsigned char point[32]);
void random_bytes(void *out, int n);

#endif
