/* Bounded CSS decoration parsers, sharing the background tokenizer. */
static int mask_extra(const char *v,int which,char *out,int max) {
    const char *parts[CSS_BG_LAYERS];int ll[CSS_BG_LAYERS],n=bg_parts(v,parts,ll,CSS_BG_LAYERS);jsbuf b={0};
    for(int i=0;i<n;i++) {
        char *part=ar_str(&tmp_arena,parts[i],ll[i]);const char *words[48];int wl[48],nw=split_words(part,words,wl,48),found=-1;
        for(int k=0;k<nw;k++) {
            if(!which && (word_is(words[k],wl[k],"alpha") || word_is(words[k],wl[k],"luminance") || word_is(words[k],wl[k],"match-source")))found=k;
            if(which && (word_is(words[k],wl[k],"add") || word_is(words[k],wl[k],"subtract") || word_is(words[k],wl[k],"intersect") || word_is(words[k],wl[k],"exclude")))found=k;
        }
        if(i)jb_addc(&b,',');
        if(found>=0)jb_add(&b,words[found],wl[found]);else jb_adds(&b,which?"add":"match-source");
    }
    int ok=b.n<max;if(ok){memcpy(out,b.p,b.n);out[b.n]=0;}jb_free(&b);return ok;
}
static const struct css_shadows *css_shadows(const char *v,struct lctx *ctx,int color) {
    if(starts(v,"none"))return NULL;
    const char *parts[CSS_SHADOWS+1];int ll[CSS_SHADOWS+1],count=bg_parts(v,parts,ll,CSS_SHADOWS+1);
    if(count>CSS_SHADOWS)return NULL;
    struct css_shadows parsed;memset(&parsed,0,sizeof(parsed));
    for(int i=0;i<count;i++) {
        char *part=ar_str(&tmp_arena,parts[i],ll[i]);const char *w[8];int wl[8],n=split_words(part,w,wl,8),lengths=0,colored=0;
        struct css_shadow *s=&parsed.layer[parsed.count];s->color=0xff000000u|(color&0xffffff);short values[4]={0};
        for(int k=0;k<n;k++) {
            double d,a;int rgb,r=parse_color(w[k],wl[k],&rgb,&a);
            if(word_is(w[k],wl[k],"inset")){if(s->inset)return NULL;s->inset=1;}
            else if(r){if(colored++)return NULL;s->color=r==2?0xff000000u|(color&0xffffff):bg_color(rgb,a);}
            else if(lengths<4 && !bg_percent(w[k],wl[k]) && parse_len(w[k],wl[k],ctx,&d)==1)values[lengths++]=bg_clamp(d,-2000,2000);
            else return NULL;
        }
        if(lengths<2 || values[2]<0)return NULL;
        s->x=values[0];s->y=values[1];s->blur=values[2]>64?64:values[2];s->spread=values[3];parsed.count++;
    }
    struct css_shadows *saved=ar_alloc(&tmp_arena,sizeof(parsed));memcpy(saved,&parsed,sizeof(parsed));return saved;
}
static int css_font_family(const char *v) {
    const char *parts[16];int ll[16],n=bg_parts(v,parts,ll,16);
    for(int i=0;i<n;i++) {
        const char *p=parts[i];int len=ll[i];if(len>1 && (*p=='\'' || *p=='"')){p++;len-=2;}
        if(word_is(p,len,"monospace") || word_is(p,len,"ui-monospace") || word_is(p,len,"Courier New") || word_is(p,len,"Courier") || word_is(p,len,"Consolas") || word_is(p,len,"Ubuntu Mono"))return 1;
        if(word_is(p,len,"sans-serif") || word_is(p,len,"system-ui") || word_is(p,len,"ui-sans-serif") || word_is(p,len,"Arial") || word_is(p,len,"Helvetica") || word_is(p,len,"Ubuntu") || word_is(p,len,"serif"))return 0;
    }
    return 0;
}
