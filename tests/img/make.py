# tests/img/make.py OUTDIR — сделать тестовые картинки разных видов и эталоны RGBA (Pillow)
import sys, os, random
from PIL import Image, ImageDraw
out = sys.argv[1]; os.makedirs(out, exist_ok=True)
def base(w, h, alpha=False):
    im = Image.new("RGBA", (w, h), (255, 255, 255, 255))
    d = ImageDraw.Draw(im)
    for y in range(h):
        for x in range(0, w, 1):
            pass
    r = random.Random(w * 31 + h)
    for i in range(40):
        x0, y0 = r.randrange(w), r.randrange(h)
        d.ellipse([x0, y0, x0 + r.randrange(1, w), y0 + r.randrange(1, h)], fill=(r.randrange(256), r.randrange(256), r.randrange(256), r.randrange(256) if alpha else 255))
    for x in range(w):                                   # градиент сверху
        d.line([(x, 0), (x, h // 8)], fill=(x * 255 // max(1, w - 1), 100, 255 - x * 255 // max(1, w - 1), 255))
    d.text((5, h // 2), "MyOS img", fill=(0, 0, 0, 255))
    return im
cases = []
def save(name, im, ref, tol, **kw):
    path = os.path.join(out, name)
    im.save(path, **kw)
    r = Image.open(path).convert("RGBA")              # эталон — как Pillow её прочтёт
    r.save(path + ".rgba.png")
    open(path + ".rgba", "wb").write(r.tobytes())
    cases.append((name, r.size[0], r.size[1], tol))
im = base(97, 61)
im2 = base(64, 40, alpha=True)
save("rgb.png", im.convert("RGB"), None, 0)
save("rgba.png", im2, None, 0)
save("rgb_interlaced.png", im.convert("RGB"), None, 0, interlace=1)
save("gray.png", im.convert("L"), None, 0)
save("graya.png", im2.convert("LA"), None, 0)
save("pal.png", im.convert("P", palette=Image.ADAPTIVE, colors=200), None, 0)
save("pal16.png", im.convert("P", palette=Image.ADAPTIVE, colors=16), None, 0, bits=4)
save("bw.png", im.convert("1"), None, 0)
p = im2.convert("P", palette=Image.ADAPTIVE, colors=64); save("pal_trns.png", im2, None, 0, optimize=True)
# gray16: Pillow обрезает 16 бит до 255, а не берёт старший байт — сравнивать не с чем
save("big.png", base(1000, 700).convert("RGB"), None, 0, compress_level=9)
save("store.png", im.convert("RGB"), None, 0, compress_level=0)
save("a.gif", im.convert("RGB"), None, 0)
g = im2.convert("P", palette=Image.ADAPTIVE, colors=255); g.info["transparency"] = 0; save("t.gif", g, None, 0, transparency=0)
save("i.gif", base(50, 77).convert("RGB"), None, 0, interlace=True)
save("a.bmp", im.convert("RGB"), None, 0)
save("p8.bmp", im.convert("P", palette=Image.ADAPTIVE, colors=100), None, 0)
save("bw.bmp", im.convert("1"), None, 0)
save("rgba.bmp", im2, None, 0)
for q in (95, 75, 30):
    save("q%d.jpg" % q, im.convert("RGB"), None, 6, quality=q)
save("s444.jpg", im.convert("RGB"), None, 4, quality=90, subsampling=0)
save("s422.jpg", im.convert("RGB"), None, 6, quality=90, subsampling=1)
save("s420.jpg", base(301, 199).convert("RGB"), None, 6, quality=85, subsampling=2)
save("gray.jpg", im.convert("L"), None, 3, quality=90)
save("prog.jpg", base(301, 199).convert("RGB"), None, 6, quality=85, progressive=True)
save("prog444.jpg", im.convert("RGB"), None, 4, quality=90, progressive=True, subsampling=0)
save("proggray.jpg", im.convert("L"), None, 3, quality=80, progressive=True)
save("restart.jpg", base(200, 130).convert("RGB"), None, 6, quality=80, restart_marker_blocks=3) if 0 else None
save("big.jpg", base(1600, 1100).convert("RGB"), None, 6, quality=80)
save("bigprog.jpg", base(1200, 900).convert("RGB"), None, 6, quality=80, progressive=True)
save("cmyk.jpg", im.convert("CMYK"), None, 6, quality=90)
with open(os.path.join(out, "cases.txt"), "w") as f:
    for c in cases: f.write("%s %d %d %g\n" % c)
