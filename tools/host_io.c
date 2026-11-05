/* host_io.c — та часть утилиты myfs, что работает с файлами компьютера.
 * Отдельный файл, потому что здесь стандартные заголовки ОС, а в myfs.c —
 * заголовки ядра MyOS (у них свой size_t, вместе их не скрестить). */
#include <stdio.h>
#include <stdlib.h>

static FILE *img;
static unsigned img_sectors;

int host_open_image(const char *path) {
    img = fopen(path, "r+b");
    if (!img) return -1;
    fseek(img, 0, SEEK_END);
    img_sectors = (unsigned)(ftell(img) / 512);
    return 0;
}

unsigned host_sectors(void) { return img_sectors; }

int host_read_sector(unsigned lba, void *buf) {
    if (lba >= img_sectors || fseek(img, (long)lba * 512, SEEK_SET)) return -1;
    return fread(buf, 1, 512, img) == 512 ? 0 : -1;
}

int host_write_sector(unsigned lba, const void *buf) {
    if (lba >= img_sectors || fseek(img, (long)lba * 512, SEEK_SET)) return -1;
    return fwrite(buf, 1, 512, img) == 512 ? 0 : -1;
}

void host_close_image(void) { if (img) fclose(img); }

/* прочитать файл компьютера целиком; *len = размер */
void *host_load_file(const char *path, unsigned *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    void *buf = malloc(n ? n : 1);
    if (buf && fread(buf, 1, n, f) != (size_t)n) { free(buf); buf = NULL; }
    fclose(f);
    *len = (unsigned)n;
    return buf;
}

void host_print(const char *s) { fputs(s, stdout); }
void host_error(const char *s) { fputs(s, stderr); }
