/* Retained CSS backgrounds. Work is bounded by the visible clipped pixels;
 * even enormous cover/percentage sizes never allocate a scaled surface. */
static void background_pixel(int x,int y,unsigned p) {
    unsigned a=p>>24;if(!a)return;
    color_t *dest=&gfx_buf[y*gfx_w+x];if(a==255){*dest=p&0xffffff;return;}
    unsigned q=*dest,ia=255-a;
    *dest=(((((p>>16)&255)*a+((q>>16)&255)*ia)/255)<<16)|
          (((((p>>8)&255)*a+((q>>8)&255)*ia)/255)<<8)|((p&255)*a+(q&255)*ia)/255;
}
static int background_round(struct box *b,int x,int y) {
    int r=clampi(b->radius,0,(b->w<b->h?b->w:b->h)/2);if(!r)return 1;
    int dx=x<r?r-1-x:x>=b->w-r?x-(b->w-r):0;
    int dy=y<r?r-1-y:y>=b->h-r?y-(b->h-r):0;
    return !dx || !dy || dx*dx+dy*dy<=r*r;
}
static unsigned gradient_pixel(const struct css_bg_layer *l,int t) {
    int i=0;while(i+1<l->nstops && t>=l->stops[i+1])i++;
    if(i+1==l->nstops || t<=l->stops[0])return l->colors[i];
    int span=l->stops[i+1]-l->stops[i];if(span<=0)return l->colors[i+1];
    int f=clampi((t-l->stops[i])*256/span,0,256);
    unsigned a=l->colors[i],b=l->colors[i+1];
    int aa=a>>24,ab=b>>24,alpha=(aa*(256-f)+ab*f)/256;if(!alpha)return 0;
    unsigned result=(unsigned)alpha<<24;
    /* Interpolate premultiplied colors so transparent stops have no dark halo. */
    for(int shift=0;shift<=16;shift+=8) {
        unsigned c=((((a>>shift)&255)*aa*(256-f)+((b>>shift)&255)*ab*f)/256)/alpha;
        result|=(c>255?255:c)<<shift;
    }
    return result;
}
static void background_rect(struct box *b,int y,int kind,int *x,int *top,int *w,int *h) {
    int l=kind?b->bw[3]:0,r=kind?b->bw[1]:0,t=kind?b->bw[0]:0,bt=kind?b->bw[2]:0;
    if(kind==2){l+=b->padding[3];r+=b->padding[1];t+=b->padding[0];bt+=b->padding[2];}
    *x=b->x+l;*top=y+t;*w=clampi(b->w-l-r,0,200000);*h=clampi(b->h-t-bt,0,200000);
}
static void draw_background_color(struct box *b,int y) {
    if(b->bg<0)return;
    int oldx,oldy,oldw,oldh,clipped=gfx_clip_get(&oldx,&oldy,&oldw,&oldh);
    if(b->bg_clip) {
        int x0,y0,x1,y1,x,top,w,h;gfx_bounds(&x0,&y0,&x1,&y1);
        background_rect(b,y,b->bg_clip,&x,&top,&w,&h);
        x0=clampi(x,x0,x1);y0=clampi(top,y0,y1);x1=clampi(x+w,x0,x1);y1=clampi(top+h,y0,y1);
        gfx_set_clip(x0,y0,x1-x0,y1-y0);
    }
    int radius=clampi(b->radius,0,(b->w<b->h?b->w:b->h)/2);
    if(radius)gfx_round_rect(b->x,y,b->w,b->h,radius,b->bg);else gfx_fill_rect(b->x,y,b->w,b->h,b->bg);
    if(b->bg_clip){if(clipped)gfx_set_clip(oldx,oldy,oldw,oldh);else gfx_no_clip();}
}
static void draw_background(struct box *b,int y) {
    if(!b->background || b->w<=0 || b->h<=0)return;
    int cx0,cy0,cx1,cy1;gfx_bounds(&cx0,&cy0,&cx1,&cy1);
    for(int layer=b->background->count-1;layer>=0;layer--) {
        const struct css_bg_layer *l=&b->background->layer[layer];
        int k=b->images[layer];struct imgent *e=k>=0?&imgs[k]:NULL;
        if(!l->nstops && (!e || e->state!=IMG_OK || !e->px))continue;
        int ax,ay,aw,ah,clipx,clipy,clipw,cliph;
        background_rect(b,y,l->origin,&ax,&ay,&aw,&ah);background_rect(b,y,l->clip,&clipx,&clipy,&clipw,&cliph);
        if(!aw || !ah)continue;
        int iw=l->nstops?aw:e->w,ih=l->nstops?ah:e->h;
        int w=iw,h=ih;
        if(l->mode) {
            int cover=l->mode==1;
            if((long long)aw*ih>(long long)ah*iw ? cover : !cover){w=aw;h=(long long)ih*aw/iw;}
            else {h=ah;w=(long long)iw*ah/ih;}
        } else {
            if(l->size[0]>=0)w=(l->size_pct&1)?(long long)aw*l->size[0]/100:l->size[0];
            if(l->size[1]>=0)h=(l->size_pct&2)?(long long)ah*l->size[1]/100:l->size[1];
            if(!l->nstops && l->size[0]>=0 && l->size[1]<0)h=(long long)ih*w/iw;
            if(!l->nstops && l->size[1]>=0 && l->size[0]<0)w=(long long)iw*h/ih;
        }
        if(w<=0 || h<=0)continue;
        w=clampi(w,1,2000000);h=clampi(h,1,2000000);
        int ox=ax+((l->pos_pct&1)?(long long)(aw-w)*l->pos[0]/100:l->pos[0]);
        int oy=ay+((l->pos_pct&2)?(long long)(ah-h)*l->pos[1]/100:l->pos[1]);
        int x0=clampi(clipx,cx0,cx1),x1=clampi(clipx+clipw,cx0,cx1),y0=clampi(clipy,cy0,cy1),y1=clampi(clipy+cliph,cy0,cy1);
        double radians=l->angle*3.141592653589793/180,dx=js_sin(radians),dy=-js_cos(radians);
        double extent=(dx<0?-dx:dx)*w+(dy<0?-dy:dy)*h;
        if(extent<1)extent=1;
        for(int yy=y0;yy<y1;yy++) {
            int sy=yy-oy;if(l->repeat&2){sy%=h;if(sy<0)sy+=h;}else if(sy<0 || sy>=h)continue;
            for(int xx=x0;xx<x1;xx++) {
                int sx=xx-ox;if(l->repeat&1){sx%=w;if(sx<0)sx+=w;}else if(sx<0 || sx>=w)continue;
                if(!background_round(b,xx-b->x,yy-y))continue;
                unsigned pixel=l->nstops?gradient_pixel(l,(int)((.5+((sx+.5-w*.5)*dx+(sy+.5-h*.5)*dy)/extent)*10000)):
                    e->px[(int)((long long)sy*e->h/h)*e->w+(int)((long long)sx*e->w/w)];
                background_pixel(xx,yy,pixel);
            }
        }
        if(e)e->used=++img_clock;
    }
}
