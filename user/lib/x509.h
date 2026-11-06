/* x509.h — проверка сертификатов HTTPS (для tls.c): цепочка до корневого центра,
 * имя сайта, сроки, подписи RSA (PKCS#1 v1.5 и PSS) и ECDSA (P-256, P-384).
 *
 * Корневые центры — файл /etc/certs.der (сертификаты DER подряд, список Mozilla).
 * Всё написано с нуля: ASN.1/DER, большие числа (Монтгомери), эллиптические кривые. */
#ifndef X509_H
#define X509_H

/* certs[0] — сертификат сайта, дальше — промежуточные (как прислал сервер).
 * 0 — цепочка ведёт к доверенному корню и годится для host; иначе текст в err.
 * В issuer — кто выдал (имя корневого центра), для показа человеку. */
int x509_verify_chain(const unsigned char *const *certs, const int *lens, int n, const char *host,
                      char *err, int emax, char *issuer, int imax);

/* подпись TLS 1.3 CertificateVerify: scheme (0x0403, 0x0804...) ключом из сертификата сайта
 * над сообщением msg; 0 — верна */
int x509_verify_tls(const unsigned char *cert, int clen, int scheme, const unsigned char *sig, int slen,
                    const unsigned char *msg, int mlen);

/* где лежат корневые сертификаты (можно поменять до первой проверки) */
extern const char *x509_roots_file;

/* ECDH на P-256 (для TLS, если сервер не умеет X25519): pub — 65 байт 04|X|Y */
int  p256_keygen(const unsigned char priv[32], unsigned char pub[65]);
int  p256_shared(const unsigned char priv[32], const unsigned char peer[65], unsigned char out[32]);

/* хеши */
void sha384(const void *data, int n, unsigned char out[48]);
void sha512(const void *data, int n, unsigned char out[64]);

#endif
