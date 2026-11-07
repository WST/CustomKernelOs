/* Regression checks against the real browser layout and response decoder. */
static int compat_failures;
#define CHECK(c, name) do { if (!(c)) { printf("FAIL: %s\n", name); compat_failures++; } } while (0)

static struct item *compat_text(const char *s) {
    for (int i = 0; i < n_items; i++)
        if (items[i].kind == IT_TEXT && !strcmp(items[i].text, s)) return &items[i];
    return NULL;
}

static struct box *compat_box(int color) {
    for (int i = 0; i < n_boxes; i++) if (boxes[i].bg == color) return &boxes[i];
    return NULL;
}

static void compat_page(const char *html) {
    page_is_dom = true;
    dom_load(html, strlen(html), "http://test.local/");
    relayout(false);
}

static int compat_tests(void) {
    resize(800, 576);
    compat_page("<style>.small{display:none}@media(max-width:600px){.small{display:block}.wide{display:none}}"
                "@media(orientation:portrait){.land{display:none}}"
                ".v{width:50vw;background:#123456;box-sizing:border-box}</style>"
                "<p class=wide>Desktop</p><p class=small>Mobile</p><p class=land>Landscape</p><div class=v>Viewport</div>");
    CHECK(compat_text("Desktop") && !compat_text("Mobile"), "desktop media query");
    struct box *b = compat_box(0x123456);
    CHECK(b && b->w == dom_view_width / 2, "vw matches browser viewport");
    resize(480, 700);
    CHECK(compat_text("Mobile") && !compat_text("Desktop") && !compat_text("Landscape"), "resize rebuilds media rules");
    b = compat_box(0x123456);
    CHECK(b && b->w == dom_view_width / 2, "vw recomputed after resize");
    jsval r;
    const char *check_view = "innerWidth === 468 && innerHeight === 644 && document.documentElement.clientWidth === 468 && matchMedia('(max-width:600px)').matches";
    CHECK(js_eval(check_view, strlen(check_view), "viewport", &r) == 0 && r.tag == JS_BOOL && r.u.b,
          "JavaScript dimensions agree with CSS");
    resize(1000, 700);
    CHECK(compat_text("Desktop") && !compat_text("Mobile"), "resize back restores desktop rules");

    compat_page("<style>.row{display:flex;flex-wrap:wrap;gap:16px}.card{width:50%;padding:12px;box-sizing:border-box;background:#abcdef}"
                ".row2{display:flex}.row2>div{width:50%;padding:12px;box-sizing:border-box;background:#fedcba}</style>"
                "<div class=row2><div><h2>Alpha</h2><p>First</p></div><div><h2>Beta</h2><p>Second</p></div></div>"
                "<p>After</p><div class=row><div class=card>Wrapone</div><div class=card>Wraptwo</div></div>");
    struct item *a = compat_text("Alpha"), *z = compat_text("Beta"), *after = compat_text("After");
    CHECK(a && z && a->y == z->y && z->x > a->x + a->w, "nested flex cards form separate columns");
    CHECK(after && a && after->y > a->y + a->h, "following block clears flex row");
    a = compat_text("Wrapone"); z = compat_text("Wraptwo");
    CHECK(a && z && z->y > a->y, "oversized flex row wraps without overlap");
    compat_page("<div style='display:flex;font-size:42px;text-align:center'>295<span style='font-size:13px'>rub</span></div>");
    a = compat_text("295"); z = compat_text("rub");
    CHECK(a && z && z->x >= a->x + a->w, "anonymous flex text does not overlap sibling");

    compat_page("<div style='width:200px;padding:10px;border:2px solid red;background:#123456'>Contentbox</div>"
                "<div style='width:200px;padding:10px;border:2px solid red;box-sizing:border-box;background:#654321'>Borderbox</div>");
    b = compat_box(0x123456);
    CHECK(b && b->w == 224, "content-box width includes padding and border");
    b = compat_box(0x654321);
    CHECK(b && b->w == 200, "border-box width includes padding and border once");

    compat_page("<script type=module src='/app.js'></script><div id=root></div>");
    CHECK(!n_items && dom_module_scripts() == 1 && dom_last_error()[0], "module load failure is diagnosed");
    compat_page("<p>Normal</p>");
    CHECK(dom_module_scripts() == 0, "module diagnostic resets on navigation");
    const char *malformed_modules[] = { "import", "export", "import d from", "export * as", "import {x as}", "import {", "export const {x}=obj;" };
    for (int k = 0; k < 7; k++) {
        char html[256];
        snprintf(html, sizeof(html), "<script type=module>%s</script>", malformed_modules[k]);
        compat_page(html);
        CHECK(dom_last_error()[0], "malformed module rejected without crashing");
    }

    static const unsigned char gz[] = {31,139,8,0,0,0,0,0,0,3,1,5,0,250,255,'h','e','l','l','o',0x86,0xa6,0x10,0x36,5,0,0,0};
    static const unsigned char zl[] = {0x78,1,1,5,0,250,255,'h','e','l','l','o',6,0x2c,2,0x15};
    memcpy(body, gz, sizeof(gz)); body_len = sizeof(gz);
    CHECK(decode_response("gzip") == 0 && body_len == 5 && !memcmp(body, "hello", 5), "gzip response decoded");
    memcpy(body, zl, sizeof(zl)); body_len = sizeof(zl);
    CHECK(decode_response("deflate") == 0 && body_len == 5 && !memcmp(body, "hello", 5), "deflate response decoded");
    memcpy(body, gz, sizeof(gz)); body_len = sizeof(gz); body[20] ^= 1;
    CHECK(decode_response("gzip") < 0, "gzip checksum corruption rejected");
    memcpy(body, gz, sizeof(gz)); body_len = 12;
    CHECK(decode_response("gzip") < 0, "truncated gzip rejected");
    memcpy(body, gz, sizeof(gz)); body_len = sizeof(gz); body[27] = 1;
    CHECK(decode_response("gzip") < 0, "oversized gzip rejected");
    CHECK(decode_response("br") < 0, "unsupported encoding reported");
    CHECK(response_header("CONTENT-LENGTH: 5", "content-length") &&
          response_header("cOnTeNt-LeNgTh: 5", "content-length") &&
          !response_header("X-Content-Length: 5", "content-length"), "HTTP header names are case insensitive");
    const char *chunks = "5;test=yes\r\nhello\r\n0\r\nX-Test: ok\r\n\r\n";
    CHECK(chunks_complete(chunks, strlen(chunks)) == 1, "chunked keep-alive finishes including trailers");
    CHECK(chunks_complete(chunks, strlen(chunks) - 1) == 0, "fragmented chunk trailer waits for completion");
    CHECK(chunks_complete("5\r\nhell", 7) == 0, "fragmented chunk body waits for completion");
    CHECK(chunks_complete("z\r\n", 3) < 0, "malformed chunk size rejected");
    printf("browser compatibility: %s (%d failures)\n", compat_failures ? "FAIL" : "OK", compat_failures);
    return compat_failures ? 1 : 0;
}
