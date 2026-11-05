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
 * ВАЖНО: сертификат сервера пока НЕ проверяется (нет списка корневых центров
 * и RSA/ECDSA). Канал зашифрован, но подменённый сервер мы не распознаем. */
#ifndef TLS_H
#define TLS_H

typedef struct tls tls;

tls  *tls_open(int fd, const char *host, char *err, int errmax);  /* рукопожатие; NULL — ошибка в err */
int   tls_read(tls *t, void *buf, int len);         /* >0 байт, 0 — конец, <0 — ошибка */
int   tls_write(tls *t, const void *buf, int len);  /* -> len или <0 */
void  tls_close(tls *t);                            /* close_notify и освободить; fd не закрывает */
const char *tls_info(tls *t);                       /* "TLS 1.3, ChaCha20-Poly1305" */

/* примитивы — для тестов и других программ */
void sha256(const void *data, int n, unsigned char out[32]);
void hmac_sha256(const void *key, int klen, const void *data, int n, unsigned char out[32]);
void x25519(unsigned char out[32], const unsigned char scalar[32], const unsigned char point[32]);
void random_bytes(void *out, int n);

#endif
