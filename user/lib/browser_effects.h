/* CSS effects operate on clipped visible rectangles, with a shared 16 MiB
 * transient budget for nested snapshots and shadow scratch surfaces. */
static unsigned effect_bytes;
static unsigned effect_budget=16u<<20;
static void *effect_alloc(unsigned bytes) {
    if(bytes>effect_budget || effect_bytes>effect_budget-bytes)return NULL;
    void *p=malloc(bytes);if(p)effect_bytes+=bytes;return p;
}
static void effect_free(void *p,unsigned bytes) {if(p){free(p);effect_bytes-=bytes;}}

struct effect_mask_layer {
    const struct css_bg_layer *style;struct imgent *image;
    int valid,x,y,w,h,cx,cy,cw,ch;double dx,dy,extent;
};
static void mask_prepare(struct effect_mask_layer *r,struct box *b,int y,int index) {
    memset(r,0,sizeof(*r));r->style=&b->mask->layer[index];const struct css_bg_layer *l=r->style;
    int k=b->mask_images[index];r->image=k>=0?&imgs[k]:NULL;
    if(!l->nstops && (!r->image || r->image->state!=IMG_OK || !r->image->px))return;
    int ax,ay,aw,ah;background_rect(b,y,l->origin,&ax,&ay,&aw,&ah);
    background_rect(b,y,l->clip,&r->cx,&r->cy,&r->cw,&r->ch);if(!aw || !ah)return;
    int iw=l->nstops?aw:r->image->w,ih=l->nstops?ah:r->image->h,w=iw,h=ih;
    if(l->mode) {
        int cover=l->mode==1;
        if((long long)aw*ih>(long long)ah*iw?cover:!cover){w=aw;h=(long long)ih*aw/iw;}
        else {h=ah;w=(long long)iw*ah/ih;}
    } else {
        if(l->size[0]>=0)w=(l->size_pct&1)?(long long)aw*l->size[0]/100:l->size[0];
        if(l->size[1]>=0)h=(l->size_pct&2)?(long long)ah*l->size[1]/100:l->size[1];
        if(!l->nstops && l->size[0]>=0 && l->size[1]<0)h=(long long)ih*w/iw;
        if(!l->nstops && l->size[1]>=0 && l->size[0]<0)w=(long long)iw*h/ih;
    }
    if(w<=0 || h<=0)return;
    r->w=clampi(w,1,2000000);r->h=clampi(h,1,2000000);
    r->x=ax+((l->pos_pct&1)?(long long)(aw-r->w)*l->pos[0]/100:l->pos[0]);
    r->y=ay+((l->pos_pct&2)?(long long)(ah-r->h)*l->pos[1]/100:l->pos[1]);
    double angle=l->angle*3.141592653589793/180;r->dx=js_sin(angle);r->dy=-js_cos(angle);
    r->extent=(r->dx<0?-r->dx:r->dx)*r->w+(r->dy<0?-r->dy:r->dy)*r->h;if(r->extent<1)r->extent=1;
    r->valid=1;if(r->image)r->image->used=++img_clock;
}
static int mask_alpha(struct effect_mask_layer *r,int x,int y,int luminance) {
    if(!r->valid || x<r->cx || x>=r->cx+r->cw || y<r->cy || y>=r->cy+r->ch)return 0;
    const struct css_bg_layer *l=r->style;int sx=x-r->x,sy=y-r->y;
    if(l->repeat&1){sx%=r->w;if(sx<0)sx+=r->w;}else if(sx<0 || sx>=r->w)return 0;
    if(l->repeat&2){sy%=r->h;if(sy<0)sy+=r->h;}else if(sy<0 || sy>=r->h)return 0;
    unsigned p=l->nstops?gradient_pixel(l,(int)((.5+((sx+.5-r->w*.5)*r->dx+(sy+.5-r->h*.5)*r->dy)/r->extent)*10000)):
        r->image->px[(int)((long long)sy*r->image->h/r->h)*r->image->w+(int)((long long)sx*r->image->w/r->w)];
    int a=p>>24;if(luminance)a=a*(54*((p>>16)&255)+183*((p>>8)&255)+19*(p&255))/(256*255);
    return a;
}
static int rounded_inside(int x,int y,int w,int h,int radius) {
    if(x<0 || x>=w || y<0 || y>=h || w<=0 || h<=0)return 0;
    int r=clampi(radius,0,(w<h?w:h)/2);if(!r)return 1;
    int dx=x<r?r-1-x:x>=w-r?x-(w-r):0,dy=y<r?r-1-y:y>=h-r?y-(h-r):0;
    return !dx || !dy || (long long)dx*dx+(long long)dy*dy<=(long long)r*r;
}
static void box_ink_bounds(struct box *b,int y,int *x0,int *y0,int *x1,int *y1) {
    *x0=b->x;*y0=y;*x1=b->x+b->w;*y1=y+b->h;
    for(int i=0;b->shadows && i<b->shadows->count;i++) {
        const struct css_shadow *s=&b->shadows->layer[i];if(s->inset)continue;
        int pad=s->spread+3*((s->blur+1)/2);
        int x=b->x+s->x-pad,top=y+s->y-pad,right=b->x+b->w+s->x+pad,bottom=y+b->h+s->y+pad;
        if(x<*x0)*x0=x;
        if(top<*y0)*y0=top;
        if(right>*x1)*x1=right;
        if(bottom>*y1)*y1=bottom;
    }
}
/* Three separable box passes approximate a Gaussian with sigma ~= blur/2.
 * Sliding sums keep blur cost linear in visible pixels, independent of radius. */
static void shadow_blur(unsigned char *a,unsigned char *temp,int w,int h,int r) {
    int divisor=2*r+1;
    for(int pass=0;pass<3;pass++) {
        for(int y=0;y<h;y++) {
            int sum=0;for(int x=0;x<=r && x<w;x++)sum+=a[y*w+x];
            for(int x=0;x<w;x++) {temp[y*w+x]=sum/divisor;if(x-r>=0)sum-=a[y*w+x-r];if(x+r+1<w)sum+=a[y*w+x+r+1];}
        }
        for(int x=0;x<w;x++) {
            int sum=0;for(int y=0;y<=r && y<h;y++)sum+=temp[y*w+x];
            for(int y=0;y<h;y++) {a[y*w+x]=sum/divisor;if(y-r>=0)sum-=temp[(y-r)*w+x];if(y+r+1<h)sum+=temp[(y+r+1)*w+x];}
        }
    }
}
static void draw_box_shadows(struct box *b,int y,int inset) {
    if(!b->shadows || b->w<=0 || b->h<=0)return;
    int cx0,cy0,cx1,cy1;gfx_bounds(&cx0,&cy0,&cx1,&cy1);
    for(int i=b->shadows->count-1;i>=0;i--) {
        const struct css_shadow *s=&b->shadows->layer[i];if(s->inset!=inset || !(s->color>>24))continue;
        int rx=b->x+(inset?b->bw[3]:0),ry=y+(inset?b->bw[0]:0);
        int rw=b->w-(inset?b->bw[1]+b->bw[3]:0),rh=b->h-(inset?b->bw[0]+b->bw[2]:0);
        int r=clampi(b->radius-(inset?b->bw[0]:0),0,(rw<rh?rw:rh)/2),blur=(s->blur+1)/2,extent=3*blur;
        int spread=inset?-s->spread:s->spread,sx=rx+s->x-spread,sy=ry+s->y-spread,sw=rw+2*spread,sh=rh+2*spread;
        int x0=inset?rx:sx-extent,y0=inset?ry:sy-extent,x1=inset?rx+rw:sx+sw+extent,y1=inset?ry+rh:sy+sh+extent;
        x0=clampi(x0,cx0,cx1);x1=clampi(x1,cx0,cx1);y0=clampi(y0,cy0,cy1);y1=clampi(y1,cy0,cy1);
        if(x1<=x0 || y1<=y0)continue;
        int w=x1-x0+2*extent,h=y1-y0+2*extent,ox=x0-extent,oy=y0-extent;
        unsigned bytes=(unsigned)w*h;unsigned char *a=effect_alloc(bytes),*temp=blur?effect_alloc(bytes):NULL;
        if(!a || (blur && !temp)){effect_free(a,bytes);effect_free(temp,bytes);continue;}
        for(int yy=0;yy<h;yy++)for(int xx=0;xx<w;xx++) {
            int inside=rounded_inside(ox+xx-sx,oy+yy-sy,sw,sh,r+spread);a[yy*w+xx]=(inset?!inside:inside)?255:0;
        }
        if(blur)shadow_blur(a,temp,w,h,blur);
        for(int yy=y0;yy<y1;yy++)for(int xx=x0;xx<x1;xx++) {
            int inside=rounded_inside(xx-rx,yy-ry,rw,rh,r);if(inset?!inside:inside)continue;
            unsigned alpha=a[(yy-oy)*w+xx-ox]*(s->color>>24)/255;
            background_pixel(xx,yy,(s->color&0xffffff)|(alpha<<24));
        }
        effect_free(a,bytes);effect_free(temp,bytes);
    }
}
static int effect_begin(int context,int command) {
    struct paint_context *p=&paint_contexts[context];struct box *own=&boxes[p->effect_box];
    if(p->anchor_first>=0) {
        int left=2000000000,top=2000000000,right=-2000000000,bottom=-2000000000;
        for(int i=p->anchor_first;i<p->anchor_end;i++) {
            struct item *it=&items[i];if(it->fixed!=own->fixed)continue;
            if(it->x<left)left=it->x;
            if(it->y<top)top=it->y;
            if(it->x+it->w>right)right=it->x+it->w;
            if(it->y+it->h+it->below>bottom)bottom=it->y+it->h+it->below;
        }
        if(right>=left && bottom>=top){own->x=left;own->y=top;own->w=right-left;own->h=bottom-top;}
    }
    if(!own->opacity)return 0;
    int x0=gfx_w,y0=gfx_h,x1=0,y1=0;
    for(int i=command+1;i<p->end;i++) {
        struct paint_command *op=&paint_commands[paint_order[i]];int x,y,w,h;
        if(op->kind==PAINT_BOX) {
            struct box *b=&boxes[op->index];int bx1,by1;
            box_ink_bounds(b,b->y-(b->fixed?0:scroll)+VIEW_Y,&x,&y,&bx1,&by1);w=bx1-x;h=by1-y;
        } else if(op->kind==PAINT_ITEM) {
            struct item *it=&items[op->index];if(it->kind==IT_ATOMIC)continue;
            x=it->x;y=it->y-(it->fixed?0:scroll)+VIEW_Y;w=it->w;h=it->h+it->below;
        } else continue;
        if(x<x0)x0=x;
        if(y<y0)y0=y;
        if(x+w>x1)x1=x+w;
        if(y+h>y1)y1=y+h;
    }
    page_clip(-1);int cx0,cy0,cx1,cy1;gfx_bounds(&cx0,&cy0,&cx1,&cy1);
    x0=clampi(x0,cx0,cx1);x1=clampi(x1,cx0,cx1);y0=clampi(y0,cy0,cy1);y1=clampi(y1,cy0,cy1);
    if(x1<=x0 || y1<=y0)return 0;
    p->x=x0;p->y=y0;p->w=x1-x0;p->h=y1-y0;
    unsigned bytes=(unsigned)p->w*p->h*sizeof(color_t);p->snapshot=effect_alloc(bytes);if(!p->snapshot)return 0;
    for(int row=0;row<p->h;row++)memcpy(p->snapshot+row*p->w,gfx_buf+(p->y+row)*gfx_w+p->x,p->w*sizeof(color_t));
    return 1;
}
static void effect_end(int context) {
    struct paint_context *p=&paint_contexts[context];if(!p->snapshot)return;
    struct box *b=&boxes[p->effect_box];struct effect_mask_layer layers[CSS_BG_LAYERS];
    int box_y=b->y-(b->fixed?0:scroll)+VIEW_Y;
    for(int i=0;b->mask && i<b->mask->count;i++)mask_prepare(&layers[i],b,box_y,i);
    for(int row=0;row<p->h;row++)for(int col=0;col<p->w;col++) {
        int x=p->x+col,y=p->y+row,alpha=255;
        if(b->mask) {
            int dest=0;
            for(int i=b->mask->count-1;i>=0;i--) {
                int source=mask_alpha(&layers[i],x,y,b->mask_luminance&(1<<i));
                if(i==b->mask->count-1)dest=source;
                else switch(b->mask_composite[i]) {
                    case 1:dest=source*(255-dest)/255;break;
                    case 2:dest=source*dest/255;break;
                    case 3:dest=(source*(255-dest)+dest*(255-source))/255;break;
                    default:dest=source+dest*(255-source)/255;break;
                }
            }
            alpha=dest;
        }
        alpha=alpha*b->opacity/255;
        color_t old=p->snapshot[row*p->w+col],now=gfx_buf[y*gfx_w+x];
        if(alpha==255)continue;
        if(!alpha){gfx_buf[y*gfx_w+x]=old;continue;}
        unsigned ia=255-alpha;
        gfx_buf[y*gfx_w+x]=(((((now>>16)&255)*alpha+((old>>16)&255)*ia)/255)<<16)|
            (((((now>>8)&255)*alpha+((old>>8)&255)*ia)/255)<<8)|((now&255)*alpha+(old&255)*ia)/255;
    }
    effect_free(p->snapshot,(unsigned)p->w*p->h*sizeof(color_t));p->snapshot=NULL;
}
