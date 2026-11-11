#include "myos.h"
#include "gfx.h"
#include "js.h"
#include "svg.h"

#define SVG_PI 3.141592653589793
struct svg_matrix { double a,b,c,d,e,f; };
struct svg_style { struct svg_matrix m; int fill,stroke,color; double opacity,fill_alpha,stroke_alpha,stroke_width; };
static int svg_space(int c) { return c==' ' || c=='\n' || c=='\r' || c=='\t' || c==','; }
static void svg_skip(const char **p) { while (svg_space(**p)) (*p)++; }
static int svg_num(const char **p,double *out) {
    svg_skip(p); const char *s=*p; double v=0,f=.1; int sign=1,any=0;
    if (*s=='-' || *s=='+') { if (*s=='-') sign=-1;s++; }
    while (*s>='0' && *s<='9') {v=v*10+*s++-'0';any=1;}
    if (*s=='.') {s++;while (*s>='0' && *s<='9') {v+=(*s++-'0')*f;f*=.1;any=1;}}
    if (!any) return 0;
    if (*s=='e' || *s=='E') {
        const char *e=s+1;int es=1,n=0,digits=0;
        if (*e=='-' || *e=='+') { if (*e=='-') es=-1;e++; }
        while (*e>='0' && *e<='9') { if(n<100)n=n*10+*e-'0';e++;digits++; }
        if (digits) { if(n>30)return 0; while(n--) v=es>0?v*10:v/10; s=e; }
    }
    v*=sign;if(v>1000000 || v< -1000000 || js_isnan(v))return 0;
    *p=s;*out=v;return 1;
}
static int svg_attr(const char *tag,const char *end,const char *key,char *out,int cap) {
    const char *p=tag;int n=strlen(key);out[0]=0;
    while(p<end) {
        while(p<end && !svg_space(*p))p++;
        while(p<end && svg_space(*p))p++;
        const char *name=p;while(p<end && !svg_space(*p) && *p!='=' && *p!='>')p++;
        int length=p-name;while(p<end && svg_space(*p))p++;
        if(p==end || *p!='=')continue;
        p++;while(p<end && svg_space(*p))p++;
        char quote=p<end && (*p=='\'' || *p=='"')?*p++:0;
        const char *value=p;while(p<end && (quote?*p!=quote:!svg_space(*p) && *p!='>'))p++;
        if(length==n && !strncmp(name,key,n)) {
            int size=p-value;if(size>=cap)size=cap-1;memcpy(out,value,size);out[size]=0;return 1;
        }
        if(quote && p<end)p++;
    }
    return 0;
}
static int svg_prop(const char *tag,const char *end,const char *key,char *out,int cap) {
    char style[1024];int found=svg_attr(tag,end,key,out,cap);
    if(svg_attr(tag,end,"style",style,sizeof(style))) {
        char *p=style;int n=strlen(key);
        while(*p) {
            while(*p && (svg_space(*p)||*p==';'))p++;
            char *name=p;while(*p && *p!=':' && *p!=';')p++;
            char *ne=p;while(ne>name && svg_space(ne[-1]))ne--;
            if(*p!=':') { if(*p)p++;continue; }
            p++;while(svg_space(*p))p++;char *value=p;while(*p && *p!=';')p++;
            char *ve=p;while(ve>value && svg_space(ve[-1]))ve--;
            if(ne-name==n && !strncmp(name,key,n)) {int size=ve-value;if(size>=cap)size=cap-1;memcpy(out,value,size);out[size]=0;found=1;}
        }
    }
    return found;
}
static double svg_value(const char *tag,const char *end,const char *key,double fallback) {
    char b[80];double v;if(svg_attr(tag,end,key,b,sizeof(b))) {const char *p=b;if(svg_num(&p,&v))return v;}return fallback;
}
static int svg_hex(int c) { return c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:-1; }
static int svg_color(const char *s,int current) {
    if(!strcmp(s,"none") || !strncmp(s,"url(",4))return -1;
    if(!strcmp(s,"currentColor") || !strcmp(s,"currentcolor"))return current;
    if(!strcmp(s,"white"))return 0xffffff;
    if(!strcmp(s,"black"))return 0;
    if(!strcmp(s,"red"))return 0xff0000;
    if(!strcmp(s,"blue"))return 0xff;
    if(!strcmp(s,"green"))return 0x008000;
    if(*s=='#') {int n=strlen(++s),value=0;if(n!=3 && n!=6)return -1;
        for(int i=0;i<n;i++) {int h=svg_hex(s[i]);if(h<0)return -1;value=(value<<(n==3?8:4))+(n==3?h*17:h);}return value;}
    return -1;
}
static struct svg_matrix svg_multiply(struct svg_matrix x,struct svg_matrix y) {
    return (struct svg_matrix){x.a*y.a+x.c*y.b,x.b*y.a+x.d*y.b,x.a*y.c+x.c*y.d,x.b*y.c+x.d*y.d,
        x.a*y.e+x.c*y.f+x.e,x.b*y.e+x.d*y.f+x.f};
}
static void svg_transform(struct svg_matrix *m,const char *s) {
    for(int step=0;*s && step<32;step++) {
        while(svg_space(*s))s++;
        const char *name=s;
        while(*s && *s!='(')s++;
        if(!*s)break;
        int length=s-name;s++;double v[6]={0};int n=0;while(n<6 && svg_num(&s,&v[n]))n++;
        while(*s && *s!=')')s++;
        if(*s)s++;
        struct svg_matrix t={1,0,0,1,0,0};
        if(length==6 && !strncmp(name,"matrix",6) && n==6)t=(struct svg_matrix){v[0],v[1],v[2],v[3],v[4],v[5]};
        else if(length==9 && !strncmp(name,"translate",9) && n)t.e=v[0],t.f=n>1?v[1]:0;
        else if(length==5 && !strncmp(name,"scale",5) && n)t.a=v[0],t.d=n>1?v[1]:v[0];
        else if(length==6 && !strncmp(name,"rotate",6) && n) {
            double a=v[0]*SVG_PI/180;t.a=t.d=js_cos(a);t.b=js_sin(a);t.c=-t.b;
            if(n==3) {t.e=v[1]-t.a*v[1]-t.c*v[2];t.f=v[2]-t.b*v[1]-t.d*v[2];}
        }
        *m=svg_multiply(*m,t);
    }
}
static int svg_fixed(double x) { if(x>30000)x=30000;if(x< -30000)x=-30000;return (int)(x*256); }
static gfx_path *svg_stroke_outline;
static double svg_stroke_thickness;
static void svg_stroke_segment(int x0,int y0,int x1,int y1) {
    if(!svg_stroke_outline)return;
    double dx=(double)x1-x0,dy=(double)y1-y0,length=js_sqrt(dx*dx+dy*dy);
    if(length<.01)return;
    double half=svg_stroke_thickness*128,nx=-dy*half/length,ny=dx*half/length;
    gfx_path *p=svg_stroke_outline;
    path_fix_move(p,x0+nx,y0+ny);path_fix_lineto(p,x1+nx,y1+ny);
    path_fix_lineto(p,x1-nx,y1-ny);path_fix_lineto(p,x0-nx,y0-ny);path_close(p);
}
static void svg_point(struct svg_matrix m,double x,double y,int *xx,int *yy) {
    *xx=svg_fixed(m.a*x+m.c*y+m.e);*yy=svg_fixed(m.b*x+m.d*y+m.f);
}
static void svg_move(gfx_path *p,struct svg_matrix m,double x,double y) {int xx,yy;svg_point(m,x,y,&xx,&yy);path_fix_move(p,xx,yy);}
static void svg_line(gfx_path *p,struct svg_matrix m,double x,double y) {int xx,yy;svg_point(m,x,y,&xx,&yy);svg_stroke_segment(p->cx,p->cy,xx,yy);path_fix_lineto(p,xx,yy);}
static void svg_close(gfx_path *p) {if(p->open)svg_stroke_segment(p->cx,p->cy,p->sx,p->sy);path_close(p);}
static void svg_cubic(gfx_path *p,struct svg_matrix m,double x1,double y1,double x2,double y2,double x,double y) {
    int a,b,c,d,e,f;svg_point(m,x1,y1,&a,&b);svg_point(m,x2,y2,&c,&d);svg_point(m,x,y,&e,&f);
    if(svg_stroke_outline) {
        int x0=p->cx,y0=p->cy;
        for(int i=1;i<=24;i++) {double t=i/24.,u=1-t;int xx=u*u*u*x0+3*u*u*t*a+3*u*t*t*c+t*t*t*e,yy=u*u*u*y0+3*u*u*t*b+3*u*t*t*d+t*t*t*f;svg_stroke_segment(p->cx,p->cy,xx,yy);path_fix_lineto(p,xx,yy);}
    } else path_fix_cubic(p,a,b,c,d,e,f);
}
static void svg_quad(gfx_path *p,struct svg_matrix m,double x1,double y1,double x,double y) {
    int a,b,c,d;svg_point(m,x1,y1,&a,&b);svg_point(m,x,y,&c,&d);
    if(svg_stroke_outline) {int x0=p->cx,y0=p->cy;for(int i=1;i<=24;i++){double t=i/24.,u=1-t;int xx=u*u*x0+2*u*t*a+t*t*c,yy=u*u*y0+2*u*t*b+t*t*d;svg_stroke_segment(p->cx,p->cy,xx,yy);path_fix_lineto(p,xx,yy);}}
    else path_fix_quad(p,a,b,c,d);
}
static double svg_abs(double v) {return v<0?-v:v;}
static void svg_arc(gfx_path *p,struct svg_matrix m,double x0,double y0,double rx,double ry,double angle,int large,int sweep,double x,double y) {
    rx=svg_abs(rx);ry=svg_abs(ry);if(rx<.0001 || ry<.0001) {svg_line(p,m,x,y);return;}
    if(x==x0 && y==y0)return;
    double phi=angle*SVG_PI/180,c=js_cos(phi),s=js_sin(phi),dx=(x0-x)/2,dy=(y0-y)/2;
    double xp=c*dx+s*dy,yp=-s*dx+c*dy,ratio=xp*xp/(rx*rx)+yp*yp/(ry*ry);
    if(ratio>1) {double scale=js_sqrt(ratio);rx*=scale;ry*=scale;}
    double den=rx*rx*yp*yp+ry*ry*xp*xp;
    double num=rx*rx*ry*ry-den,k=den>0?js_sqrt(num>0?num/den:0):0;
    if(large==sweep)k=-k;
    double cxp=k*rx*yp/ry,cyp=-k*ry*xp/rx,cx=c*cxp-s*cyp+(x0+x)/2,cy=s*cxp+c*cyp+(y0+y)/2;
    double ux=(xp-cxp)/rx,uy=(yp-cyp)/ry,vx=(-xp-cxp)/rx,vy=(-yp-cyp)/ry;
    double start=js_atan2(uy,ux),delta=js_atan2(ux*vy-uy*vx,ux*vx+uy*vy);
    if(!sweep && delta>0)delta-=2*SVG_PI;
    if(sweep && delta<0)delta+=2*SVG_PI;
    int steps=(int)(svg_abs(delta)*8)+1;
    for(int i=1;i<=steps;i++) {double t=start+delta*i/steps,xx=rx*js_cos(t),yy=ry*js_sin(t);svg_line(p,m,cx+c*xx-s*yy,cy+s*xx+c*yy);}
}
static void svg_path(gfx_path *p,struct svg_matrix m,const char *s) {
    double x=0,y=0,sx=0,sy=0,cx=0,cy=0;int cmd=0,previous=0;
    for(int step=0;*s && step<65536;step++) {
        svg_skip(&s);if(!*s)break;
        if((*s>='A'&&*s<='Z')||(*s>='a'&&*s<='z'))cmd=*s++;
        int lower=cmd>='a'&&cmd<='z',op=lower?cmd-32:cmd;
        if(op=='Z') {svg_close(p);x=sx;y=sy;previous=op;cmd=0;continue;}
        int n=op=='H'||op=='V'?1:op=='M'||op=='L'||op=='T'?2:op=='S'||op=='Q'?4:op=='C'?6:op=='A'?7:0;
        if(!n)break;
        double v[7];
        int valid=1;
        for(int i=0;i<n;i++) {
            svg_skip(&s);
            if(op=='A' && (i==3||i==4)) {if(*s=='0'||*s=='1')v[i]=*s++-'0';else valid=0;}
            else if(!svg_num(&s,&v[i]))valid=0;
            if(!valid)break;
        }
        if(!valid)break;
        if(op=='H') {x=lower?x+v[0]:v[0];svg_line(p,m,x,y);}
        else if(op=='V') {y=lower?y+v[0]:v[0];svg_line(p,m,x,y);}
        else if(op=='A') {double xx=v[5]+(lower?x:0),yy=v[6]+(lower?y:0);svg_arc(p,m,x,y,v[0],v[1],v[2],(int)v[3],(int)v[4],xx,yy);x=xx;y=yy;}
        else {
            for(int i=0;i<n;i+=2)if(lower){v[i]+=x;v[i+1]+=y;}
            if(op=='M') {svg_move(p,m,v[0],v[1]);sx=v[0];sy=v[1];cmd=lower?'l':'L';}
            else if(op=='L')svg_line(p,m,v[0],v[1]);
            else if(op=='C') {svg_cubic(p,m,v[0],v[1],v[2],v[3],v[4],v[5]);cx=v[2];cy=v[3];}
            else if(op=='S') {double a=previous=='C'||previous=='S'?2*x-cx:x,b=previous=='C'||previous=='S'?2*y-cy:y;svg_cubic(p,m,a,b,v[0],v[1],v[2],v[3]);cx=v[0];cy=v[1];}
            else if(op=='Q') {svg_quad(p,m,v[0],v[1],v[2],v[3]);cx=v[0];cy=v[1];}
            else if(op=='T') {double a=previous=='Q'||previous=='T'?2*x-cx:x,b=previous=='Q'||previous=='T'?2*y-cy:y;svg_quad(p,m,a,b,v[0],v[1]);cx=a;cy=b;}
            x=v[n-2];y=v[n-1];
        }
        previous=op;
    }
}
static void svg_paint(gfx_path *p,struct svg_style st) {
    if(st.fill>=0)path_fill(p,st.fill,(int)(255*st.opacity*st.fill_alpha));
    if(svg_stroke_outline)path_fill(svg_stroke_outline,st.stroke,(int)(255*st.opacity*st.stroke_alpha));
}
static void svg_render(char *xml,struct svg_style initial) {
    struct svg_style stack[64];int sp=0,skip=0;stack[0]=initial;const char *p=xml;
    for(int shapes=0;*p && shapes<8192;shapes++) {
        p=strchr(p,'<');if(!p)break;const char *tag=++p;char quote=0;
        while(*p && (quote || *p!='>')) {if(quote) {if(*p==quote)quote=0;}else if(*p=='\''||*p=='"')quote=*p;p++;}
        if(!*p)break;
        const char *end=p++;
        if(*tag=='!'||*tag=='?')continue;
        if(*tag=='/') {if(sp>0)sp--;if(skip>0)skip--;continue;}
        char name[24];int n=0;while(tag+n<end && !svg_space(tag[n]) && tag[n]!='/' && n<23){name[n]=tag[n];n++;}name[n]=0;
        int self=end>tag && end[-1]=='/';struct svg_style st=stack[sp];char b[4096];
        if(skip || !strcmp(name,"defs") || !strcmp(name,"clipPath") || !strcmp(name,"mask") || !strcmp(name,"symbol")) {
            if(!self) {skip++;if(sp==63)return;stack[++sp]=st;}
            continue;
        }
        if(svg_prop(tag,end,"color",b,sizeof(b)))st.color=svg_color(b,st.color);
        if(svg_prop(tag,end,"fill",b,sizeof(b)))st.fill=svg_color(b,st.color);
        if(svg_prop(tag,end,"stroke",b,sizeof(b)))st.stroke=svg_color(b,st.color);
        if(svg_prop(tag,end,"opacity",b,sizeof(b))) {const char *q=b;double v;if(svg_num(&q,&v))st.opacity*=v<0?0:v>1?1:v;}
        if(svg_prop(tag,end,"fill-opacity",b,sizeof(b))) {const char *q=b;double v;if(svg_num(&q,&v))st.fill_alpha=v<0?0:v>1?1:v;}
        if(svg_prop(tag,end,"stroke-opacity",b,sizeof(b))) {const char *q=b;double v;if(svg_num(&q,&v))st.stroke_alpha=v<0?0:v>1?1:v;}
        if(svg_prop(tag,end,"stroke-width",b,sizeof(b))) {const char *q=b;svg_num(&q,&st.stroke_width);}
        if(svg_attr(tag,end,"transform",b,sizeof(b)))svg_transform(&st.m,b);
        if(!skip) {
            gfx_path path,outline;path_begin(&path,0,0,1,1);path_begin(&outline,0,0,1,1);
            svg_stroke_thickness=st.stroke_width*js_sqrt(st.m.a*st.m.a+st.m.b*st.m.b);
            if(svg_stroke_thickness>256)svg_stroke_thickness=256;
            svg_stroke_outline=st.stroke>=0 && st.stroke_width>0?&outline:NULL;
            if(!strcmp(name,"path")) {
                /* Path data can be much longer than an ordinary attribute. */
                int cap=end-tag+1;char *d=malloc(cap);
                if(d) {if(svg_attr(tag,end,"d",d,cap))svg_path(&path,st.m,d);free(d);}
            } else if(!strcmp(name,"rect")) {
                double x=svg_value(tag,end,"x",0),y=svg_value(tag,end,"y",0),w=svg_value(tag,end,"width",0),h=svg_value(tag,end,"height",0);
                if(w>0 && h>0) {svg_move(&path,st.m,x,y);svg_line(&path,st.m,x+w,y);svg_line(&path,st.m,x+w,y+h);svg_line(&path,st.m,x,y+h);svg_close(&path);}
            } else if(!strcmp(name,"circle") || !strcmp(name,"ellipse")) {
                double x=svg_value(tag,end,"cx",0),y=svg_value(tag,end,"cy",0),rx=svg_value(tag,end,"rx",svg_value(tag,end,"r",0)),ry=svg_value(tag,end,"ry",rx);
                if(rx>0 && ry>0) {for(int i=0;i<=64;i++){double a=i*SVG_PI/32;if(!i)svg_move(&path,st.m,x+rx,y);else svg_line(&path,st.m,x+rx*js_cos(a),y+ry*js_sin(a));}svg_close(&path);}
            } else if(!strcmp(name,"line")) {
                svg_move(&path,st.m,svg_value(tag,end,"x1",0),svg_value(tag,end,"y1",0));svg_line(&path,st.m,svg_value(tag,end,"x2",0),svg_value(tag,end,"y2",0));st.fill=-1;
            } else if(!strcmp(name,"polyline") || !strcmp(name,"polygon")) {
                if(svg_attr(tag,end,"points",b,sizeof(b))) {const char *q=b;double x,y;int first=1;while(svg_num(&q,&x)&&svg_num(&q,&y)){if(first)svg_move(&path,st.m,x,y);else svg_line(&path,st.m,x,y);first=0;}if(!strcmp(name,"polygon"))svg_close(&path);}
            }
            svg_paint(&path,st);svg_stroke_outline=NULL;path_free(&path);path_free(&outline);
        }
        if(!self) {if(sp==63)return;stack[++sp]=st;}
    }
}
int svg_decode(const unsigned char *data,int len,img *out,int max_side) {
    if(len<5 || len>2*1024*1024)return -1;
    char *xml=malloc(len+1);if(!xml)return -2;memcpy(xml,data,len);xml[len]=0;
    char *tag=strstr(xml,"<svg");if(!tag || tag-xml>1024) {free(xml);return -1;}
    char *end=strchr(tag,'>');if(!end) {free(xml);return -1;}
    double vb[4]={0,0,300,150};char b[160];
    int has_viewbox=svg_attr(tag+1,end,"viewBox",b,sizeof(b)) || svg_attr(tag+1,end,"viewbox",b,sizeof(b));
    if(has_viewbox) {const char *p=b;for(int i=0;i<4;i++)if(!svg_num(&p,&vb[i])){free(xml);return -1;}}
    double w=svg_value(tag+1,end,"width",vb[2]),h=svg_value(tag+1,end,"height",vb[3]);
    if(!has_viewbox){vb[2]=w;vb[3]=h;}
    if(w<=0 || h<=0 || vb[2]<=0 || vb[3]<=0) {free(xml);return -1;}
    if(max_side<1 || max_side>2048)max_side=2048;
    double limit=w>h?w:h;if(limit>max_side) {w=w*max_side/limit;h=h*max_side/limit;}
    int iw=(int)(w+.5),ih=(int)(h+.5);if(iw<1)iw=1;if(ih<1)ih=1;
    double scale=w/vb[2];if(h/vb[3]<scale)scale=h/vb[3];
    struct svg_style st={{scale,0,0,scale,(w-vb[2]*scale)/2-vb[0]*scale,(h-vb[3]*scale)/2-vb[1]*scale},0,-1,0,1,1,1,1};
    struct gfx_canvas cv;memset(&cv,0,sizeof(cv));
    cv.black=calloc(iw*ih,4);cv.white=malloc(iw*ih*4);
    if(!cv.black || !cv.white) {free(cv.black);free(cv.white);free(xml);return -2;}
    cv.w=iw;cv.h=ih;cv.save_buf=gfx_buf;cv.save_w=gfx_w;cv.save_h=gfx_h;
    cv.saved_clip=gfx_clip_get(&cv.cx,&cv.cy,&cv.cw,&cv.ch);gfx_no_clip();
    for(int i=0;i<iw*ih;i++)cv.white[i]=0xffffff;
    for(int pass=0;pass<2;pass++) {gfx_buf=pass?cv.white:cv.black;gfx_w=iw;gfx_h=ih;svg_render(xml,st);}
    unsigned *px=gfx_canvas_end(&cv);free(xml);if(!px)return -2;
    out->w=iw;out->h=ih;out->px=px;out->has_alpha=1;return 0;
}
