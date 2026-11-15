# libcurl и OpenSSL

Браузер открывает HTTPS своим стеком в `user/lib/tls.c` и `user/lib/x509.c`.
Команды `wget` и `download` этот разбор не повторяют: запрос передаётся libcurl,
а TLS выполняет OpenSSL. Исходники библиотек лежат в `user/lib/curl` и
`user/lib/openssl`, программы линкуются с `libcurl.so`.

Корневые сертификаты те же, что у браузера: список Mozilla в `/etc/certs.der`.
Перед запросом он перекладывается в PEM и отдаётся libcurl. Ключ `-k`
отключает проверку.

```sh
wget https://example.com/
wget https://self-signed.badssl.com/
```

Второй адрес останавливается с ошибкой сертификата. С `-k` файл всё равно
скачивается.

Тот же PEM, `/etc/ssl/cert.pem`, читает Git из [ext](ext.md) при установке.
