/* tests/img_test.c — декодеры картинок (user/lib/img.c) на Linux: сравнить с образцами.
 *   build/img-test картинка эталон.rgba ширина высота допуск
 * эталон — сырые байты RGBA (их делает tests/img/make.py через Pillow). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "img.h"

static unsigned char *load(const char *f, int *n) {
    FILE *fp = fopen(f, "rb");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END); *n = ftell(fp); fseek(fp, 0, SEEK_SET);
    unsigned char *b = malloc(*n + 1);
    *n = fread(b, 1, *n, fp);
    fclose(fp);
    return b;
}

int main(int argc, char **argv) {
    if (argc == 2) {                                 /* только распаковать (для фаззинга) */
        int n; unsigned char *d = load(argv[1], &n);
        img im;
        int r = d ? img_decode(d, n, &im, 1600) : -1;
        printf("%s: %d %dx%d\n", argv[1], r, im.w, im.h);
        if (!r) img_free(&im);
        free(d);
        return 0;
    }
    if (argc < 6) return 2;
    int n, rn;
    unsigned char *d = load(argv[1], &n), *ref = load(argv[2], &rn);
    int w = atoi(argv[3]), h = atoi(argv[4]);
    double tol = atof(argv[5]);
    img im;
    int r = img_decode(d, n, &im, 0);
    if (r || im.w != w || im.h != h) { printf("FAIL %s: r=%d size %dx%d, want %dx%d\n", argv[1], r, im.w, im.h, w, h); return 1; }
    double sum = 0; int worst = 0;
    for (int i = 0; i < w * h; i++) {
        unsigned c = im.px[i];
        int a = c >> 24, rr = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
        const unsigned char *e = ref + i * 4;
        int ea = e[3];
        if (!a && !ea) continue;                     /* оба прозрачны — цвет не важен */
        int dd[4] = { rr - e[0], g - e[1], b - e[2], a - ea };
        for (int k = 0; k < 4; k++) { int v = dd[k] < 0 ? -dd[k] : dd[k]; sum += v; if (v > worst) worst = v; }
    }
    double mean = sum / (w * h * 4.0);
    printf("%s %s: mean diff %.3f, worst %d\n", mean <= tol ? "ok  " : "FAIL", argv[1], mean, worst);
    img_free(&im);
    return mean <= tol ? 0 : 1;
}
