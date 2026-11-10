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

static void compat_round_rects(void) {
    color_t *saved=gfx_buf;int sw=gfx_w,sh=gfx_h,cx,cy,cw,ch;
    int clipped=gfx_clip_get(&cx,&cy,&cw,&ch);
    color_t *reference=malloc(80*80*4),*fast=malloc(80*80*4);
    CHECK(reference && fast,"rounded rectangle comparison buffers");
    if(!reference || !fast) {free(reference);free(fast);return;}
    unsigned seed=12345;
    static const int alphas[]={0,1,2,64,127,128,200,254,255};
    for(int test=0;test<512;test++) {
        seed=seed*1664525u+1013904223u;
        int x=(seed&31)-8,y=((seed>>5)&31)-8,w=1+((seed>>10)%75),h=1+((seed>>17)%75);
        int radius=(int)(seed%48)-3,corners=test&15,alpha=alphas[test%9];
        for(int k=0;k<80*80;k++)reference[k]=fast[k]=(k*1234567u+seed)&0xffffff;
        gfx_w=gfx_h=80;gfx_buf=reference;
        if(test&1)gfx_set_clip(9,13,47,41);else gfx_no_clip();
        gfx_path p;path_begin(&p,0,0,1,1);path_round_rect_ex(&p,x,y,w,h,radius,corners);
        path_fill(&p,0x438fce,alpha);path_free(&p);
        gfx_buf=fast;gfx_round_rect_ex(x,y,w,h,radius,corners,0x438fce,alpha);
        if(memcmp(reference,fast,80*80*4)) {
            printf("rounded mismatch: x=%d y=%d w=%d h=%d r=%d corners=%d alpha=%d\n",x,y,w,h,radius,corners,alpha);
            CHECK(0,"rounded rectangle fast path matches reference pixels");break;
        }
    }
    gfx_buf=saved;gfx_w=sw;gfx_h=sh;
    if(clipped)gfx_set_clip(cx,cy,cw,ch);else gfx_no_clip();
    free(reference);free(fast);
}

static void compat_scroll_frame(int delta,int expected_reuse) {
    unsigned before=scroll_reuse_frames;
    scroll_by(delta);redraw();
    CHECK((scroll_reuse_frames!=before)==expected_reuse,"scroll chooses the appropriate paint path");
    unsigned *actual=malloc(W*H*4);
    CHECK(actual!=NULL,"scroll comparison buffer");if(!actual)return;
    memcpy(actual,win->pixels,W*H*4);
    draw_page();draw_toolbar();draw_status();
    CHECK(!memcmp(actual,win->pixels,W*H*4),"retained scrolling matches a full repaint pixel for pixel");
    free(actual);
}

static void compat_scrolling(void) {
    const char *html="<style>body{margin:0;background:#f3f5f8}.card{height:180px;padding:16px;margin:12px;"
        "border-radius:18px;background:#fb8b24;color:white;box-sizing:border-box}</style>"
        "<div class=card>First card <a href='/a'>Link</a></div>"
        "<div style='height:80px;overflow:hidden'><div class=card>Clipped card</div></div>"
        "<img src='http://test.local/pending.svg' width=96 height=64>"
        "<div class=card>Third card <input name=q value=test></div>"
        "<div class=card>Fourth card</div><div class=card>Fifth card</div>"
        "<div class=card>Sixth card</div><div class=card>Seventh card</div>";
    static const int widths[]={800,480,1280};
    for(int k=0;k<3;k++) {
        resize(widths[k],576);compat_page(html);editing_addr=false;redraw();
        compat_scroll_frame(48,1);compat_scroll_frame(17,1);compat_scroll_frame(-31,1);
        compat_scroll_frame(180,1);compat_scroll_frame(-48,1);
        focus_input=0;compat_scroll_frame(48,0);focus_input=-1;redraw();
        compat_scroll_frame(12,1);
        int pending=img_find("http://test.local/pending.svg",false);
        CHECK(pending>=0,"pending image exists for paint invalidation test");
        if(pending>=0) {
            const char *svg="<svg width='96' height='64'><path fill='#123456' d='M0 0H96V64H0Z'/></svg>";
            img_store(&imgs[pending],(const unsigned char*)svg,strlen(svg));
            compat_scroll_frame(-48,0);
        }
        jsval result;const char *change="document.querySelectorAll('.card')[2].style.backgroundColor='#123456'";
        CHECK(js_eval(change,strlen(change),"scroll mutation",&result)==0,"scroll mutation fixture executes");
        relayout(true);compat_scroll_frame(24,0);
        int selected=-1;
        for(int i=0;i<n_items;i++)if(items[i].kind==IT_TEXT && items[i].y>=scroll && items[i].y<scroll+VIEW_H) {selected=i;break;}
        if(selected>=0) {
            sel_item=sel_end_item=selected;sel_byte=0;sel_end_byte=1;redraw();
            compat_scroll_frame(12,0);
            sel_item=sel_end_item=-1;redraw();compat_scroll_frame(-12,1);
        }
        compat_scroll_frame(doc_h,0);compat_scroll_frame(-48,1);
    }
}

static int compat_tests(void) {
    compat_round_rects();
    compat_scrolling();
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
    const char *check_view = "innerWidth === 468 && innerHeight === 604 && document.documentElement.clientWidth === 468 && matchMedia('(max-width:600px)').matches";
    CHECK(js_eval(check_view, strlen(check_view), "viewport", &r) == 0 && r.tag == JS_BOOL && r.u.b,
          "JavaScript dimensions agree with CSS");
    resize(1000, 700);
    CHECK(compat_text("Desktop") && !compat_text("Mobile"), "resize back restores desktop rules");
    compat_page("<p id=size>Before</p><script>window.addEventListener('resize',function(){document.getElementById('size').textContent=innerWidth<600?'Narrow':'Wide';});</script>");
    resize(480,700);CHECK(compat_text("Narrow"),"resize listeners see the updated viewport");
    resize(1000,700);CHECK(compat_text("Wide"),"resize listener updates the DOM after growing the window");
    compat_page("<p id=media>Before</p><script>var mq=matchMedia('(min-width:600px)');"
                "mq.addEventListener('change',function(e){document.getElementById('media').textContent=e.matches?'Desktopmq':'Mobilemq';});</script>");
    resize(480,700);CHECK(compat_text("Mobilemq"),"MediaQueryList change notifies mobile breakpoint");
    resize(1000,700);CHECK(compat_text("Desktopmq"),"MediaQueryList change notifies desktop breakpoint");

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

    compat_page("<div style='display:flex;width:600px;gap:20px'>"
                "<div style='width:160px;background:#112233'>Sidebar</div>"
                "<div style='flex:1;background:#223344'>Article</div></div>");
    b = compat_box(0x112233);
    struct box *fluid = compat_box(0x223344);
    CHECK(b && fluid && b->w == 160 && fluid->w == 420 && fluid->x == b->x+180,
          "fixed sidebar and flex:1 fill remaining space including gap");

    compat_page("<div style='display:flex;width:600px;gap:12px'>"
                "<div style='flex:1;background:#112233'>One</div>"
                "<div style='flex:2;background:#223344'>Two</div></div>");
    b = compat_box(0x112233); fluid = compat_box(0x223344);
    CHECK(b && fluid && b->w == 196 && fluid->w == 392, "flex shorthand distributes grow factors");

    compat_page("<div style='display:flex;width:400px;justify-content:space-between;align-items:center;min-height:120px;background:#445566'>"
                "<div style='width:80px;background:#112233'><a href='/a'>Short</a></div>"
                "<div style='width:100px;background:#223344'>Long<br>Second</div></div><p>Following</p>");
    b = compat_box(0x112233); fluid = compat_box(0x223344);
    struct box *row = compat_box(0x445566);
    a = compat_text("Short"); z = compat_text("Following");
    CHECK(row && b && fluid && row->h == 120 && b->x == row->x && fluid->x+fluid->w == row->x+row->w,
          "space-between uses free horizontal space");
    CHECK(row && b && fluid && 2*b->y+b->h >= 2*row->y+119 && 2*b->y+b->h <= 2*row->y+121 &&
          2*fluid->y+fluid->h >= 2*row->y+119 && 2*fluid->y+fluid->h <= 2*row->y+121,
          "align-items centers unequal children in min-height row");
    CHECK(a && b && a->x == b->x && a->y == b->y && a->link >= 0,
          "aligned link geometry follows painted box");
    CHECK(z && row && z->y >= row->y+row->h, "following content clears explicit flex height");

    compat_page("<div style='height:40px;background:#112233'>Header</div>"
                "<div style='display:flex;margin-top:24px'><div style='background:#223344'>Card</div></div>");
    b=compat_box(0x112233); fluid=compat_box(0x223344);
    CHECK(b && fluid && fluid->y == b->y+b->h+24, "transparent flex container preserves top margin");

    compat_page("<div style='display:flex'><div style='min-height:100px;background:#112233'>"
                "<p style='margin:0 0 16px'>Inside</p></div></div>"
                "<div style='background:#223344'>Outside</div>");
    b=compat_box(0x112233); fluid=compat_box(0x223344);
    CHECK(b && fluid && b->h == 100 && fluid->y == b->y+100,
          "paragraph bottom margin stays inside flex item minimum height");

    compat_page("<div style='display:flex;width:250px;flex-wrap:wrap;gap:20px;justify-content:center'>"
                "<div style='width:100px;background:#112233'>First</div>"
                "<div style='width:100px;background:#223344'>Second</div>"
                "<div style='width:100px;background:#334455'>Third</div></div>");
    b=compat_box(0x112233); fluid=compat_box(0x223344); row=compat_box(0x334455);
    CHECK(b && fluid && row && b->x+120 == fluid->x && row->x == b->x+60 && row->y > b->y,
          "each wrapped flex line is justified independently");

    compat_page("<div style='display:flex;width:300px;gap:10px'>"
                "<div style='width:200px;flex-shrink:0;background:#112233'>Fixed</div>"
                "<div style='width:200px;background:#223344'>Shrink</div></div>");
    b=compat_box(0x112233); fluid=compat_box(0x223344);
    CHECK(b && fluid && b->w == 200 && fluid->w == 90, "flex-shrink:0 preserves fixed item");

    compat_page("<div style='display:flex;width:400px;gap:10px'>"
                "<div style='width:100px;margin:0 10px;background:#112233'>Margins</div>"
                "<div style='flex:1;background:#223344'>Remaining</div></div>");
    b=compat_box(0x112233); fluid=compat_box(0x223344);
    CHECK(b && fluid && b->w == 100 && fluid->w == 270 && fluid->x == b->x+120,
          "flex margins consume space outside the border box");

    compat_page("<div style='display:flex;width:500px;min-height:100px;justify-content:flex-end;align-items:center'>"
                "<div style='width:200px'><input name=q value=aligned></div></div>");
    CHECK(n_inputs == 1 && inputs[0].item >= 0, "aligned input exists");
    if (n_inputs == 1 && inputs[0].item >= 0) {
        struct item *field=&items[inputs[0].item];
        focus_input=-1; editing_addr=false;
        on_click(field->x+4, field->y+VIEW_Y+4);
        CHECK(focus_input == 0, "aligned form field remains clickable at painted position");
    }

    compat_page("<div style='display:flex;width:300px;align-items:stretch'>"
                "<div style='background:#112233'>Short</div>"
                "<div style='background:#223344'>Tall<br>Second<br>Third</div></div>");
    b=compat_box(0x112233); fluid=compat_box(0x223344);
    CHECK(b && fluid && b->h == fluid->h, "stretch equalizes auto-height backgrounds");

    compat_page("<div style='height:80px;padding:10px;border:2px solid red;background:#112233'>Height</div>"
                "<div style='height:80px;padding:10px;border:2px solid red;box-sizing:border-box;background:#223344'>Borderheight</div>"
                "<div style='min-height:60px;padding:10px;background:#334455'></div>"
                "<div style='max-width:100px;padding:10px;background:#445566'>Maxwidth</div>");
    b=compat_box(0x112233); fluid=compat_box(0x223344); row=compat_box(0x334455);
    CHECK(b && fluid && row && b->h == 104 && fluid->h == 80 && row->h == 80,
          "height and min-height respect content-box and border-box including empty blocks");
    b=compat_box(0x445566);
    CHECK(b && b->w == 120, "content-box max-width excludes padding");

    compat_page("<div style='font-size:16px;line-height:2'>Leadone<br>Leadtwo"
                "<div style='font-size:20px'>Bigone<br>Bigtwo</div></div>");
    a=compat_text("Leadone"); z=compat_text("Leadtwo");
    CHECK(a && z && z->y-a->y == 32, "unitless line-height controls line spacing");
    a=compat_text("Bigone"); z=compat_text("Bigtwo");
    CHECK(a && z && z->y-a->y == 40, "unitless line-height inherits as factor");

    /* Escaped punctuation in a selector must not swallow the rest of a stylesheet. */
    compat_page("<style>@layer utilities{.after\\:content-\\[\\'\\'\\]::after{content:\"\"}"
                ".bg-\\[var\\(--brand\\)\\]{background:var(--brand)}"
                "@media(width>=40rem){.md\\:grid-cols-3{grid-template-columns:repeat(3,minmax(0,1fr))}}}"
                ":root{--brand:#fb8b24}.grid{display:grid;gap:24px}.card{background:#abcdef}</style>"
                "<div class='bg-[var(--brand)]'>Brand</div>"
                "<div class='grid md:grid-cols-3'><div class=card>First</div><div class=card>Second</div>"
                "<div class=card>Third</div><div class=card>Fourth</div></div>");
    b=compat_box(0xfb8b24);a=compat_text("First");z=compat_text("Second");after=compat_text("Fourth");
    CHECK(b, "escaped quotes do not hide later brand variables or selectors");
    CHECK(a && z && after && a->y==z->y && z->x>a->x && after->y>a->y,
          "responsive escaped Grid selector lays out three tracks and a new row");
    resize(480,700);a=compat_text("First");z=compat_text("Second");
    CHECK(a && z && z->y>a->y, "Grid responsive track count updates on resize");
    resize(1000,700);

    char variables[16000];int used=snprintf(variables,sizeof(variables),"<style>:root{");
    for(int i=0;i<240;i++)used+=snprintf(variables+used,sizeof(variables)-used,"--v%d:%d;",i,i);
    used+=snprintf(variables+used,sizeof(variables)-used,"--brand:#fb8b24}p{background:var(--brand)}</style><p>Manyvars</p>");
    compat_page(variables);
    CHECK(compat_box(0xfb8b24), "large theme blocks preserve late custom properties");

    compat_page("<div style='background:oklch(100% 0 0)'>White</div>"
                "<div style='background:oklch(0% 0 0)'>Black</div>"
                "<div style='background:oklch(70.5% .213 47.604);border-radius:8px'>Orange</div>"
                "<style>.fallback{background:#123456}@supports(color:color-mix(in lab,red,red)){.fallback{background:color-mix(in lab,red,red)}}</style>"
                "<div class=fallback>Fallback</div>");
    CHECK(compat_box(0xffffff) && compat_box(0x000000), "OKLCH black and white conversion");
    a=compat_text("Orange");
    const struct dom_style *orange=a?dom_style(a->node):NULL;
    CHECK(orange && orange->radius==8 && ((orange->bg>>16)&255)>245 && ((orange->bg>>8)&255)>95 &&
          ((orange->bg>>8)&255)<130 && (orange->bg&255)<10, "OKLCH chromatic color and rounded corners");
    CHECK(compat_box(0x123456), "unsupported color-mix keeps its fallback declaration");

    compat_page("<div style='display:flex'><aside style='position:fixed;width:200px;background:#112233'>Navigation</aside>"
                "<main style='flex:1;margin-left:200px;background:#223344'>Maincontent</main></div>");
    b=compat_box(0x112233);fluid=compat_box(0x223344);
    CHECK(b && fluid && b->x==0 && b->w==200 && fluid->x<230 && fluid->x>=200,
          "fixed sidebar does not reserve a second flex column");
    compat_page("<aside style='position:fixed;width:200px;transform:translateX(-100%);background:#112233'>Offscreen</aside><p>Visible</p>");
    b=compat_box(0x112233);
    CHECK(b && b->x==-200,"translateX percentage moves fixed navigation out of the viewport");

    compat_page("<div style='position:relative;width:200px;height:100px;overflow:hidden;background:#112233'>"
                "<a href='/overlay' style='position:absolute;left:150px;top:20px;width:100px;height:40px;background:#fb8b24'>Overlay</a></div>");
    b=compat_box(0x112233);fluid=compat_box(0xfb8b24);
    CHECK(b && fluid && b->h==100 && fluid->x==b->x+150 && fluid->y==b->y+20,
          "absolute child uses the positioned containing block without changing its height");
    if(b && fluid) {
        redraw();int y=b->y+VIEW_Y+35;
        CHECK(win->pixels[y*W+b->x+199]==0xfb8b24 && win->pixels[y*W+b->x+201]!=0xfb8b24,
              "overflow hidden clips painted backgrounds");
        CHECK(item_at(b->x+201,y)<0,"clipped content cannot be clicked outside its container");
    }
    compat_page("<div style='height:100px'><div style='height:100%;padding:10px;box-sizing:border-box;background:#112233'>Percentheight</div></div>");
    b=compat_box(0x112233);
    CHECK(b && b->h==100,"percentage border-box height includes padding once");

    compat_page("<button style='background:#fb8b24;color:white;padding:12px;border-radius:6px'"
                " onclick=\"document.getElementById('result').textContent='Clicked'\"><span>Styledbutton</span></button><p id=result>Waiting</p>");
    int branded=compat_box(0xfb8b24)!=NULL;
    for(int k=0;k<n_items;k++)if(items[k].kind==IT_FILL && items[k].bg==0xfb8b24)branded=1;
    CHECK(compat_text("Styledbutton") && branded, "styled button renders DOM text and CSS background");
    if(n_inputs==1 && inputs[0].item>=0) {
        struct item control=items[inputs[0].item];
        on_click(control.x+2,control.y+VIEW_Y+2);
        CHECK(compat_text("Clicked"), "styled button keeps DOM click behavior");
    } else CHECK(0,"styled button keeps a clickable control");

    const char *vector="<svg width='32' height='24' viewBox='0 0 32 24'><g transform='translate(4,2)' fill='#fb8b24'>"
        "<path d='M0 0H16V16H0Z'/></g><path fill='none' stroke='#0000ff' stroke-width='2' d='M2 22H30'/></svg>";
    img vector_image;
    int vr=svg_decode((const unsigned char*)vector,strlen(vector),&vector_image,256);
    CHECK(vr==0 && vector_image.w==32 && vector_image.h==24, "SVG dimensions and decoding");
    if(vr==0) {
        CHECK(vector_image.px[8*32+8]==0xfffb8b24 && vector_image.px[0]==0, "SVG inherited fill, transform and transparency");
        CHECK((vector_image.px[22*32+16]&0xffffff)==0xff && (vector_image.px[22*32+16]>>24)>200,
              "SVG horizontal stroke is painted");
        img_free(&vector_image);
    }
    const char *bad_svg="<svg viewBox='0 0 0 0'><path d='M'/></svg>";
    CHECK(svg_decode((const unsigned char*)bad_svg,strlen(bad_svg),&vector_image,256)<0,"invalid SVG viewBox is rejected");
    const char *default_svg="<svg width='8' height='8'><path fill='#fb8b24' d='M0 0H8V8H0Z'/></svg>";
    vr=svg_decode((const unsigned char*)default_svg,strlen(default_svg),&vector_image,256);
    CHECK(vr==0 && vector_image.w==8 && vector_image.px[4*8+4]==0xfffb8b24,"SVG without viewBox uses its viewport coordinates");
    if(vr==0)img_free(&vector_image);
    compat_page("<svg class=icon style='width:32px;height:32px;color:#fb8b24' viewBox='0 0 24 24' fill='none' stroke='currentColor'>"
                "<path d='M2 12H22' stroke-width='2'/></svg><p>Icontext</p>");
    int svg_item=-1;for(int k=0;k<n_items;k++)if(items[k].kind==IT_IMG)svg_item=k;
    CHECK(svg_item>=0, "inline SVG enters the image pipeline");
    if(svg_item>=0) {
        while(img_pump());
        int found=0;for(int k=0;k<n_items;k++)if(items[k].kind==IT_IMG && imgs[items[k].img].state==IMG_OK)found=1;
        CHECK(found,"inline SVG data URI decodes without network access");
    }

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
