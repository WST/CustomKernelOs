/* Internal CSS background parser. Included after css.c's token/length helpers.
 * Bounded layers and stops; unsupported image types remain transparent. */
static int bg_percent(const char *s,int n) { for(int i=0;i<n;i++)if(s[i]=='%')return 1;return 0; }
static int bg_clamp(double n,int lo,int hi) { return n<lo?lo:n>hi?hi:(int)n; }

/* Commas inside functions or quoted URLs do not separate layers/stops. */
static int bg_parts(const char *s,const char **p,int *len,int max) {
    int count=0,depth=0;char q=0;const char *start=s;
    for(const char *c=s;;c++) {
        if(!*c || (!q && !depth && *c==',')) {
            if(count==max)return count;
            while(start<c && is_sp(*start))start++;
            const char *end=c;while(end>start && is_sp(end[-1]))end--;
            p[count]=start;len[count++]=end-start;start=c+1;if(!*c)break;
        } else if(*c=='\\' && c[1])c++;
        else if(q){if(*c==q)q=0;}
        else if(*c=='\'' || *c=='"')q=*c;
        else if(*c=='(')depth++;
        else if(*c==')' && depth)depth--;
    }
    return count;
}

/* Extract the longhands reset by background, independently of cascade order. */
static int bg_extract(const char *v,int which,char *out,int max,int mask) {
    const char *layers[CSS_BG_LAYERS];int ll[CSS_BG_LAYERS],n=bg_parts(v,layers,ll,CSS_BG_LAYERS);
    jsbuf result={0};
    for(int k=0;k<n;k++) {
        char *part=malloc(ll[k]+1);if(!part){jb_free(&result);return 0;}
        memcpy(part,layers[k],ll[k]);part[ll[k]]=0;
        /* Split '/' outside a function even when no surrounding spaces exist. */
        jsbuf spaced={0};int depth=0;char quote=0;
        for(const char *c=part;*c;c++) {
            if(*c=='\\' && c[1]){jb_addc(&spaced,*c++);jb_addc(&spaced,*c);continue;}
            if(quote){if(*c==quote)quote=0;}
            else if(*c=='\'' || *c=='"')quote=*c;
            else if(*c=='(')depth++;else if(*c==')')depth--;
            if(!quote && !depth && *c=='/')jb_adds(&spaced," / ");else jb_addc(&spaced,*c);
        }
        jb_addc(&spaced,0);const char *w[48];int wl[48],nw=split_words(spaced.p,w,wl,48);
        jsbuf value={0};int slash=0,boxes=0;
        for(int i=0;i<nw;i++) {
            if(word_is(w[i],wl[i],"/")){slash=1;continue;}
            int box=word_is(w[i],wl[i],"border-box") || word_is(w[i],wl[i],"padding-box") || word_is(w[i],wl[i],"content-box");
            if(box) {if((which==3 && !boxes) || which==4){value.n=0;jb_add(&value,w[i],wl[i]);}boxes++;continue;}
            int repeat=word_is(w[i],wl[i],"repeat") || word_is(w[i],wl[i],"no-repeat") || word_is(w[i],wl[i],"repeat-x") || word_is(w[i],wl[i],"repeat-y");
            double d;struct lctx ctx={16,16,100};
            int pos=word_is(w[i],wl[i],"left") || word_is(w[i],wl[i],"right") || word_is(w[i],wl[i],"top") || word_is(w[i],wl[i],"bottom") || word_is(w[i],wl[i],"center");
            int size=word_is(w[i],wl[i],"auto") || word_is(w[i],wl[i],"cover") || word_is(w[i],wl[i],"contain");
            int length=parse_len(w[i],wl[i],&ctx,&d)==1;
            if((which==2 && repeat) || (which==1 && !slash && (pos || length)) || (which==0 && slash && (size || length))) {
                if(value.n)jb_addc(&value,' ');
                jb_add(&value,w[i],wl[i]);
            }
        }
        if(k)jb_addc(&result,',');
        if(value.n)jb_add(&result,value.p,value.n);
        else jb_adds(&result,which==0?"auto":which==1?"0% 0%":which==2?"repeat":which==3 && !mask?"padding-box":"border-box");
        jb_free(&value);jb_free(&spaced);free(part);
    }
    int ok=result.n<max;if(ok){memcpy(out,result.p,result.n);out[result.n]=0;}jb_free(&result);return ok;
}

static void bg_pair(const char *v,short values[2],unsigned char *pct,struct lctx *ctx,int position) {
    const char *w[5];int wl[5],n=split_words(v,w,wl,5);if(n<1 || n>2)return;
    values[0]=position?50:-1;values[1]=position?50:-1;*pct=position?3:0;
    int swap=n==1?(word_is(w[0],wl[0],"top") || word_is(w[0],wl[0],"bottom")):
        (word_is(w[0],wl[0],"top") || word_is(w[0],wl[0],"bottom") || word_is(w[1],wl[1],"left") || word_is(w[1],wl[1],"right"));
    for(int i=0;i<n;i++) {
        int axis=swap?1-i:i;double d;
        if(position && (word_is(w[i],wl[i],"left") || word_is(w[i],wl[i],"top")))values[axis]=0;
        else if(position && (word_is(w[i],wl[i],"right") || word_is(w[i],wl[i],"bottom")))values[axis]=100;
        else if(position && word_is(w[i],wl[i],"center"))values[axis]=50;
        else if(parse_len(w[i],wl[i],&(struct lctx){ctx->em,ctx->rem,100},&d)==1) {
            values[axis]=bg_clamp(d,-20000,20000);
            if(bg_percent(w[i],wl[i]))*pct|=1<<axis;else *pct&=~(1<<axis);
        }
    }
}

static unsigned bg_color(int rgb,double a) { return ((unsigned)bg_clamp(a*255+.5,0,255)<<24)|(rgb&0xffffff); }
static int bg_gradient(const char *v,struct css_bg_layer *l) {
    if(!starts(v,"linear-gradient("))return 0;
    const char *end=strrchr(v,')');if(!end)return 0;
    char *args=ar_str(&tmp_arena,v+16,end-v-16);
    const char *parts[CSS_GRADIENT_STOPS+1];int lengths[CSS_GRADIENT_STOPS+1];
    int n=bg_parts(args,parts,lengths,CSS_GRADIENT_STOPS+1),first=0;l->angle=180;
    if(n<2)return 0;
    if(starts(parts[0],"to ")) {
        char *dir=ar_str(&tmp_arena,parts[0],lengths[0]);
        int right=strstr(dir,"right")!=NULL,left=strstr(dir,"left")!=NULL,top=strstr(dir,"top")!=NULL,bottom=strstr(dir,"bottom")!=NULL;
        if(right)l->angle=top?45:bottom?135:90;else if(left)l->angle=top?315:bottom?225:270;else l->angle=top?0:180;first=1;
    } else {
        double angle;int used=read_num(parts[0],&angle);
        if(used>0 && lengths[0]==used+3 && !strncmp(parts[0]+used,"deg",3)) {l->angle=bg_clamp(angle,-20000,20000)%360;first=1;}
    }
    for(int i=first;i<n && l->nstops<CSS_GRADIENT_STOPS;i++) {
        char *stop=ar_str(&tmp_arena,parts[i],lengths[i]);const char *w[3];int wl[3],nw=split_words(stop,w,wl,3),rgb;double a,d;
        if(nw<1 || nw>2 || parse_color(w[0],wl[0],&rgb,&a)!=1)return 0;
        int offset=CSS_AUTO;
        if(nw==2) {if(!bg_percent(w[1],wl[1]) || parse_len(w[1],wl[1],&(struct lctx){16,16,10000},&d)!=1)return 0;offset=bg_clamp(d,-20000,20000);}
        l->colors[l->nstops]=bg_color(rgb,a);l->stops[l->nstops++]=offset;
    }
    if(l->nstops<2){l->nstops=0;return 0;}
    if(l->stops[0]==CSS_AUTO)l->stops[0]=0;
    if(l->stops[l->nstops-1]==CSS_AUTO)l->stops[l->nstops-1]=10000;
    int prev=0;
    for(int i=1;i<l->nstops;i++)if(l->stops[i]!=CSS_AUTO) {
        if(l->stops[i]<l->stops[prev])l->stops[i]=l->stops[prev];
        for(int j=prev+1;j<i;j++)l->stops[j]=l->stops[prev]+(l->stops[i]-l->stops[prev])*(j-prev)/(i-prev);
        prev=i;
    }
    return 1;
}

static const char *bg_at(const char *v,int index) {
    if(!v)return NULL;
    const char *p[CSS_BG_LAYERS];int n[CSS_BG_LAYERS],count=bg_parts(v,p,n,CSS_BG_LAYERS);
    if(!count)return NULL;
    index%=count;return ar_str(&tmp_arena,p[index],n[index]);
}
static const struct css_background *bg_parse(const char *image,const char *size,const char *pos,const char *repeat,const char *origin,const char *clip,struct lctx *ctx) {
    if(!image)return NULL;
    struct css_background temporary,*bg=&temporary;memset(bg,0,sizeof(*bg));
    const char *parts[CSS_BG_LAYERS];int lengths[CSS_BG_LAYERS];bg->count=bg_parts(image,parts,lengths,CSS_BG_LAYERS);
    int any=0;
    for(int i=0;i<bg->count;i++) {
        struct css_bg_layer *l=&bg->layer[i];l->size[0]=l->size[1]=-1;l->pos_pct=3;l->repeat=3;l->origin=1;
        char *part=malloc(lengths[i]+1);if(!part)continue;memcpy(part,parts[i],lengths[i]);part[lengths[i]]=0;
        const char *u=strstr(part,"url(");
        if(u) {
            u+=4;while(is_sp(*u))u++;char q=(*u=='\'' || *u=='"')?*u++:0;const char *e=u;
            while(*e && (q?*e!=q:*e!=')'))e++;
            if(e>u && *e){l->url=ar_str(&tmp_arena,u,e-u);any=1;}
        } else if(bg_gradient(part,l))any=1;else l->nstops=0;
        free(part);
        const char *v=bg_at(size,i);
        if(v){l->mode=starts(v,"cover")?1:starts(v,"contain")?2:0;if(!l->mode)bg_pair(v,l->size,&l->size_pct,ctx,0);}
        if((v=bg_at(pos,i)))bg_pair(v,l->pos,&l->pos_pct,ctx,1);
        if((v=bg_at(repeat,i))) {
            const char *w[2];int wl[2],n=split_words(v,w,wl,2);
            if(n==1)l->repeat=starts(v,"no-repeat")?0:starts(v,"repeat-x")?1:starts(v,"repeat-y")?2:3;
            else if(n==2)l->repeat=(word_is(w[0],wl[0],"repeat")?1:0)|(word_is(w[1],wl[1],"repeat")?2:0);
        }
        if((v=bg_at(origin,i)))l->origin=starts(v,"content-box")?2:starts(v,"border-box")?0:1;
        if((v=bg_at(clip,i)))l->clip=starts(v,"content-box")?2:starts(v,"padding-box")?1:0;
    }
    if(!any)return NULL;
    struct css_background *saved=ar_alloc(&tmp_arena,sizeof(*saved));memcpy(saved,bg,sizeof(*saved));return saved;
}


/* Grid tracks used by the browser's row-based grid placement. Fixed/percent
 * tracks consume space first; fr/minmax(0,fr) divide the remainder by weight. */
static void grid_track(struct dom_style *o,const char *s,int len,struct lctx *ctx) {
    if(o->grid_columns>=32)return;
    char *v=ar_str(&tmp_arena,s,len);double d;int type=0,value=100;
    if(starts(v,"minmax(")) {
        const char *end=strrchr(v,')');if(end){char *inner=ar_str(&tmp_arena,v+7,end-v-7);const char *parts[2];int ll[2];
            if(bg_parts(inner,parts,ll,2)==2)v=ar_str(&tmp_arena,parts[1],ll[1]);}
    }
    int used=read_num(v,&d),n=strlen(v);
    if(used>0 && n==used+2 && !strncmp(v+used,"fr",2))value=bg_clamp(d*100,0,20000);
    else if(parse_len(v,n,&(struct lctx){ctx->em,ctx->rem,100},&d)==1){type=strchr(v,'%')?2:1;value=bg_clamp(d,0,20000);}
    int i=o->grid_columns++;o->grid_track_type[i]=type;o->grid_track[i]=value;
}
static void grid_tracks(struct dom_style *o,const char *cols,struct lctx *ctx,int depth) {
    if(!cols || depth>4)return;
    const char *w[32];int wl[32],n=split_words(cols,w,wl,32);
    for(int i=0;i<n && o->grid_columns<32;i++) {
        if(wl[i]>8 && starts(w[i],"repeat(")) {
            char *inner=ar_str(&tmp_arena,w[i]+7,wl[i]-8);const char *parts[2];int ll[2];
            if(bg_parts(inner,parts,ll,2)!=2)continue;
            char *count=ar_str(&tmp_arena,parts[0],ll[0]),*tracks=ar_str(&tmp_arena,parts[1],ll[1]);
            if(starts(count,"auto-fit") || starts(count,"auto-fill")) {
                o->grid_auto=starts(count,"auto-fit")?1:2;
                if(starts(tracks,"minmax(")) {
                    const char *end=strrchr(tracks,')');if(end) {
                        char *args=ar_str(&tmp_arena,tracks+7,end-tracks-7);const char *minmax[2];int ml[2];double px;
                        if(bg_parts(args,minmax,ml,2)==2 && parse_len(minmax[0],ml[0],ctx,&px)==1)o->grid_min=bg_clamp(px,1,20000);
                    }
                }
                grid_tracks(o,tracks,ctx,depth+1);
            } else {double repeat;if(read_num(count,&repeat)==(int)strlen(count))for(int k=0;k<bg_clamp(repeat,0,32) && o->grid_columns<32;k++)grid_tracks(o,tracks,ctx,depth+1);}
        } else grid_track(o,w[i],wl[i],ctx);
    }
}
