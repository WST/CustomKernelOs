/* make — сборка по Makefile.
 *
 * Подмножество, которого хватает чистой сборке: NAME = value и NAME := value,
 * $(NAME) и ${NAME}, правила «цель: зависимости», шаблон с одним «%»,
 * рецепты со строки, начинающейся с таба. $@ $< $^ $* подставляются в рецепт,
 * @ в начале прячет строку, - игнорирует код возврата. .PHONY пересобирается
 * всегда. Рецепт запускается через sh -c. -jN запускает независимые цели
 * параллельно; строки рецепта одной цели выполняются последовательно.
 *
 * Времени изменения у файлов нет. Цель устарела, если её файла нет, она
 * .PHONY, или в этом же запуске пересобралась зависимость. Правка исходника
 * при уже существующем результате сама по себе пересборку не вызывает.
 */
#include "myos.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "build_jobs.h"

#define MAX_VARS  48
#define MAX_RULES 64
#define MAX_PRE   64          /* bash линкуется из нескольких десятков архивов */
#define MAX_REC   8
#define MAX_NODE  1024
#define MAX_STACK 16
#define MAX_PHONY 32
#define MAX_BATCH 8
#define MAX_JOBS BUILD_JOBS_MAX
#define NAME      80
#define VAL       1024
#define LINE      2048

struct var {
    char name[NAME];
    char val[VAL];
    int simple;                 /* := уже раскрыто */
};

struct rule {
    char target[NAME];
    char pre[MAX_PRE][NAME];
    int npre;
    char rec[MAX_REC][VAL];
    int nrec;
    int pattern;
    int phony;
};

static struct var vars[MAX_VARS];
static int nvars;
static struct rule rules[MAX_RULES];
static int nrules;
static char default_goal[NAME];
enum node_state { NODE_NEW, NODE_PLANNED, NODE_RUNNING, NODE_DONE };
struct build_node {
    char name[NAME], stem[NAME];
    unsigned short deps[MAX_PRE];
    int ndep, phony, built, state, pid;
    struct rule *recipe;
};
static struct build_node nodes[MAX_NODE];
static int nnodes, jobs=1;
static char phony_name[MAX_PHONY][NAME];
static int nphony;
static struct rule *batch[MAX_BATCH];
static int nbatch;
static int open_recipe;
static int dry_run;
static int any_ran;
static int src_line;
static const char *src_name;

static void note(const char *msg) {
    fprintf(stderr, "make: %s:%d: %s\n", src_name ? src_name : "make", src_line, msg);
}

static int copy_str(char *dst, int max, const char *src) {
    int n = 0;
    if (!src) src = "";
    while (src[n]) {
        if (n >= max - 1) return -1;
        dst[n] = src[n];
        n++;
    }
    dst[n] = 0;
    return 0;
}

static void rtrim(char *s) {
    int n = (int)strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t')) s[--n] = 0;
}

static int is_name_char(int c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '.';
}

static int exists(const char *path) {
    struct dirent_info st;
    if (!path || !path[0]) return 0;
    return dirstat(path, &st) == 0;
}

static int find_var(const char *name) {
    int i;
    for (i = 0; i < nvars; i++)
        if (!strcmp(vars[i].name, name)) return i;
    return -1;
}

static int append_mem(char *out, int *o, int max, const char *s, int n) {
    int i;
    for (i = 0; i < n; i++) {
        if (*o >= max - 1) return -1;
        out[(*o)++] = s[i];
    }
    out[*o] = 0;
    return 0;
}

static int append_str(char *out, int *o, int max, const char *s) {
    return append_mem(out, o, max, s, (int)strlen(s));
}

static const char *builtin_var(const char *name) {
    if (!strcmp(name, "CC")) return "cc";
    if (!strcmp(name, "MAKE")) return "make";
    return NULL;
}

static int expand(const char *in, char *out, int outmax, int depth,
                  const char *at, const char *lt, const char *hat, const char *star);

static int expand_name(const char *name, char *out, int *o, int outmax, int depth,
                       const char *at, const char *lt, const char *hat, const char *star) {
    const char *lit = NULL;
    char tmp[VAL];
    int vi;
    if (!strcmp(name, "@")) lit = at ? at : "";
    else if (!strcmp(name, "<")) lit = lt ? lt : "";
    else if (!strcmp(name, "^")) lit = hat ? hat : "";
    else if (!strcmp(name, "*")) lit = star ? star : "";
    if (lit) return append_str(out, o, outmax, lit);
    vi = find_var(name);
    if (vi < 0) {
        lit = builtin_var(name);
        return append_str(out, o, outmax, lit ? lit : "");
    }
    if (vars[vi].simple) return append_str(out, o, outmax, vars[vi].val);
    if (depth > 16) { note("variable recursion"); return -1; }
    if (expand(vars[vi].val, tmp, VAL, depth + 1, at, lt, hat, star) < 0) return -1;
    return append_str(out, o, outmax, tmp);
}

static int expand(const char *in, char *out, int outmax, int depth,
                  const char *at, const char *lt, const char *hat, const char *star) {
    int o = 0;
    const char *p = in;
    out[0] = 0;
    while (*p) {
        char name[NAME];
        int nlen = 0;
        char endch;
        if (*p != '$') {
            if (append_mem(out, &o, outmax, p, 1) < 0) { note("line too long"); return -1; }
            p++;
            continue;
        }
        p++;
        if (*p == '$') {
            if (append_mem(out, &o, outmax, "$", 1) < 0) { note("line too long"); return -1; }
            p++;
            continue;
        }
        if (*p == '(' || *p == '{') {
            endch = *p == '(' ? ')' : '}';
            p++;
            while (*p && *p != endch) {
                if (nlen >= NAME - 1) { note("name too long"); return -1; }
                if (*p == ' ' || *p == '\t' || *p == ',') {
                    note("functions are not supported");
                    return -1;
                }
                name[nlen++] = *p++;
            }
            name[nlen] = 0;
            if (*p != endch || nlen == 0) { note("bad variable"); return -1; }
            p++;
        } else if (*p) {
            name[0] = *p++;
            name[1] = 0;
        } else {
            note("bad variable");
            return -1;
        }
        if (expand_name(name, out, &o, outmax, depth, at, lt, hat, star) < 0) {
            if (o >= outmax - 1) note("line too long");
            return -1;
        }
    }
    return 0;
}

static struct rule *find_explicit(const char *name) {
    int i;
    for (i = 0; i < nrules; i++)
        if (!rules[i].pattern && !strcmp(rules[i].target, name)) return &rules[i];
    return NULL;
}

static int listed_phony(const char *name) {
    int i;
    for (i = 0; i < nphony; i++)
        if (!strcmp(phony_name[i], name)) return 1;
    return 0;
}

static void mark_phony(const char *name) {
    struct rule *r;
    int i;
    for (i = 0; i < nphony; i++)
        if (!strcmp(phony_name[i], name)) return;
    if (nphony >= MAX_PHONY) return;
    if (copy_str(phony_name[nphony], NAME, name) < 0) return;
    nphony++;
    r = find_explicit(name);
    if (r) r->phony = 1;
}

static int add_pre(struct rule *r, const char *name) {
    int i;
    if (!name[0]) return 0;
    for (i = 0; i < r->npre; i++)
        if (!strcmp(r->pre[i], name)) return 0;
    if (r->npre >= MAX_PRE) { note("too many prerequisites"); return -1; }
    if (copy_str(r->pre[r->npre], NAME, name) < 0) { note("name too long"); return -1; }
    r->npre++;
    return 0;
}

static struct rule *ensure_rule(const char *target) {
    struct rule *r;
    int pattern = strchr(target, '%') != NULL;
    if (!pattern) {
        r = find_explicit(target);
        if (r) return r;
    }
    if (nrules >= MAX_RULES) { note("too many rules"); return NULL; }
    r = &rules[nrules++];
    memset(r, 0, sizeof *r);
    if (copy_str(r->target, NAME, target) < 0) { note("name too long"); return NULL; }
    r->pattern = pattern;
    r->phony = listed_phony(target);
    if (!pattern && target[0] != '.' && !default_goal[0])
        copy_str(default_goal, NAME, target);
    return r;
}

static int add_recipe(struct rule *r, const char *body) {
    if (r->nrec >= MAX_REC) { note("too many recipe lines"); return -1; }
    if (copy_str(r->rec[r->nrec], VAL, body) < 0) { note("recipe too long"); return -1; }
    r->nrec++;
    return 0;
}

static int split_words(char *s, char **av, int max) {
    int n = 0;
    while (*s) {
        while (*s == ' ' || *s == '\t') s++;
        if (!*s) break;
        if (n == max) return -1;
        av[n++] = s;
        while (*s && *s != ' ' && *s != '\t') s++;
        if (*s) *s++ = 0;
    }
    return n;
}

static int define_var(char *line, int simple_at) {
    char *eq = strchr(line, '=');
    char *name;
    char *val;
    char expanded[VAL];
    int vi;
    if (!eq) return -1;
    *eq = 0;
    if (simple_at && eq > line && eq[-1] == ':') eq[-1] = 0;
    name = line;
    while (*name == ' ' || *name == '\t') name++;
    rtrim(name);
    val = eq + 1;
    while (*val == ' ' || *val == '\t') val++;
    rtrim(val);
    if (!name[0]) { note("empty variable name"); return -1; }
    for (vi = 0; name[vi]; vi++)
        if (!is_name_char(name[vi])) { note("bad variable name"); return -1; }
    if (simple_at) {
        if (expand(val, expanded, VAL, 0, NULL, NULL, NULL, NULL) < 0) return -1;
        val = expanded;
    }
    vi = find_var(name);
    if (vi < 0) {
        if (nvars >= MAX_VARS) { note("too many variables"); return -1; }
        vi = nvars++;
        if (copy_str(vars[vi].name, NAME, name) < 0) { note("name too long"); return -1; }
    }
    if (copy_str(vars[vi].val, VAL, val) < 0) { note("value too long"); return -1; }
    vars[vi].simple = simple_at;
    return 0;
}

static int define_rule(char *line) {
    char *colon = strchr(line, ':');
    char left[LINE], right[LINE];
    char exp_l[LINE], exp_r[LINE];
    char *targets[MAX_BATCH];
    char *pres[MAX_PRE];
    int nt, np, i, j;
    if (!colon) { note("missing separator"); return -1; }
    *colon = 0;
    if (copy_str(left, LINE, line) < 0 || copy_str(right, LINE, colon + 1) < 0) {
        note("line too long");
        return -1;
    }
    if (expand(left, exp_l, LINE, 0, NULL, NULL, NULL, NULL) < 0) return -1;
    if (expand(right, exp_r, LINE, 0, NULL, NULL, NULL, NULL) < 0) return -1;
    nt = split_words(exp_l, targets, MAX_BATCH);
    np = split_words(exp_r, pres, MAX_PRE);
    if (nt < 0 || np < 0) { note("too many words"); return -1; }
    if (nt == 0) { note("empty target"); return -1; }
    if (nt == 1 && !strcmp(targets[0], ".PHONY")) {
        for (i = 0; i < np; i++) mark_phony(pres[i]);
        open_recipe = 0;
        nbatch = 0;
        return 0;
    }
    nbatch = 0;
    for (i = 0; i < nt; i++) {
        struct rule *r;
        int pct = 0;
        for (j = 0; targets[i][j]; j++)
            if (targets[i][j] == '%') pct++;
        if (pct > 1) { note("one % per target"); return -1; }
        r = ensure_rule(targets[i]);
        if (!r) return -1;
        for (j = 0; j < np; j++) {
            int k, pp = 0;
            for (k = 0; pres[j][k]; k++)
                if (pres[j][k] == '%') pp++;
            if (pp > 1) { note("one % per prerequisite"); return -1; }
            if (add_pre(r, pres[j]) < 0) return -1;
        }
        if (nbatch < MAX_BATCH) batch[nbatch++] = r;
    }
    open_recipe = 1;
    return 0;
}

static int classify(char *line) {
    int i;
    for (i = 0; line[i]; i++) {
        if (line[i] == '\\' && line[i + 1]) { i++; continue; }
        if (line[i] == ':' && line[i + 1] == '=') return define_var(line, 1);
        if (line[i] == '=') return define_var(line, 0);
        if (line[i] == ':') return define_rule(line);
    }
    note("missing separator");
    return -1;
}

static void strip_comment(char *s) {
    int i;
    for (i = 0; s[i]; i++) {
        if (s[i] == '\\' && s[i + 1]) { i++; continue; }
        if (s[i] == '#') { s[i] = 0; return; }
    }
}

static int read_logical(FILE *f, char *buf, int max) {
    int n = 0;
    buf[0] = 0;
    for (;;) {
        char phys[LINE];
        int len, cont;
        if (!fgets(phys, (int)sizeof phys, f)) return n ? 1 : 0;
        src_line++;
        len = (int)strlen(phys);
        if (len == (int)sizeof phys - 1 && phys[len - 1] != '\n') {
            note("line too long");
            return -1;
        }
        if (len && phys[len - 1] == '\n') phys[--len] = 0;
        if (len && phys[len - 1] == '\r') phys[--len] = 0;
        cont = len > 0 && phys[len - 1] == '\\';
        if (cont) phys[--len] = 0;
        if (n && n < max - 1 && len) buf[n++] = ' ';
        if (n + len >= max) { note("line too long"); return -1; }
        memcpy(buf + n, phys, (size_t)len);
        n += len;
        buf[n] = 0;
        if (!cont) return 1;
    }
}

static int load_file(const char *path) {
    FILE *f = fopen(path, "r");
    char buf[LINE];
    int rc;
    if (!f) {
        fprintf(stderr, "make: %s: %s\n", path, strerror(errno));
        return -1;
    }
    src_name = path;
    src_line = 0;
    while ((rc = read_logical(f, buf, LINE)) != 0) {
        char *body;
        if (rc < 0) { fclose(f); return -1; }
        strip_comment(buf);
        rtrim(buf);
        if (!buf[0]) continue;
        if (buf[0] == '\t') {
            int i;
            if (!open_recipe || nbatch == 0) {
                note("recipe before target");
                fclose(f);
                return -1;
            }
            body = buf + 1;
            if (!body[0]) continue;
            for (i = 0; i < nbatch; i++)
                if (add_recipe(batch[i], body) < 0) { fclose(f); return -1; }
            continue;
        }
        open_recipe = 0;
        nbatch = 0;
        if (classify(buf) < 0) { fclose(f); return -1; }
    }
    fclose(f);
    return 0;
}

static int subst_stem(const char *pat, const char *stem, char *out, int max) {
    int o = 0;
    while (*pat) {
        if (*pat == '%') {
            if (append_str(out, &o, max, stem) < 0) return -1;
            pat++;
            continue;
        }
        if (o >= max - 1) return -1;
        out[o++] = *pat++;
    }
    out[o] = 0;
    return 0;
}

static int match_pat(const char *pat, const char *name, char *stem, int stemmax) {
    const char *pct = strchr(pat, '%');
    int pre, post, nlen, sl;
    if (!pct) return !strcmp(pat, name);
    pre = (int)(pct - pat);
    post = (int)strlen(pct + 1);
    nlen = (int)strlen(name);
    if (nlen < pre + post) return 0;
    if (pre && memcmp(name, pat, (size_t)pre)) return 0;
    if (post && strcmp(name + (nlen - post), pct + 1)) return 0;
    sl = nlen - pre - post;
    if (sl >= stemmax) return 0;
    memcpy(stem, name + pre, (size_t)sl);
    stem[sl] = 0;
    return 1;
}

static int pattern_ok(struct rule *r, const char *stem) {
    int i;
    if (r->npre == 0) return 1;
    for (i = 0; i < r->npre; i++) {
        char dep[NAME];
        if (subst_stem(r->pre[i], stem, dep, NAME) < 0) return 0;
        if (exists(dep) || find_explicit(dep)) continue;
        return 0;
    }
    return 1;
}

static struct rule *find_pattern(const char *name, char *stem) {
    int i;
    for (i = 0; i < nrules; i++) {
        if (!rules[i].pattern) continue;
        if (!match_pat(rules[i].target, name, stem, NAME)) continue;
        if (!pattern_ok(&rules[i], stem)) continue;
        return &rules[i];
    }
    return NULL;
}

static int join_pre(char pres[][NAME], int npre, char *out, int max) {
    int i, o = 0;
    out[0] = 0;
    for (i = 0; i < npre; i++) {
        if (i && append_str(out, &o, max, " ") < 0) return -1;
        if (append_str(out, &o, max, pres[i]) < 0) return -1;
    }
    return 0;
}

static int run_recipes(struct rule *r, const char *target, const char *stem,
                       char pres[][NAME], int npre) {
    char all[VAL];
    int i;
    if (join_pre(pres, npre, all, VAL) < 0) {
        fprintf(stderr, "make: prerequisite list too long\n");
        return -1;
    }
    for (i = 0; i < r->nrec; i++) {
        const char *body = r->rec[i];
        char cmd[VAL];
        int quiet = 0, ignore = 0, st;
        while (*body == '@' || *body == '-' || *body == ' ') {
            if (*body == '@') quiet = 1;
            else if (*body == '-') ignore = 1;
            body++;
        }
        if (!body[0]) continue;
        if (expand(body, cmd, VAL, 0, target, npre ? pres[0] : "", all, stem) < 0)
            return -1;
        if (!cmd[0]) continue;
        if (!quiet) printf("%s\n", cmd);
        any_ran = 1;
        if (dry_run) continue;
        st = system(cmd);
        if (st != 0 && !ignore) {
            if (st < 0) fprintf(stderr, "make: *** [%s] recipe failed to start\n", target);
            else fprintf(stderr, "make: *** [%s] Error %d\n", target, st);
            return -1;
        }
    }
    return 0;
}

/* Plan the entire graph before executing anything. Shared prerequisites have
 * one node; a parent becomes runnable only after every dependency completes. */
static int plan(const char *name,int depth) {
    for(int i=0;i<nnodes;i++) if(!strcmp(nodes[i].name,name)) {
        if(nodes[i].state==NODE_NEW) {
            fprintf(stderr,"make: circular dependency: %s\n",name); return -1;
        }
        return i;
    }
    if(depth>=MAX_STACK || nnodes>=MAX_NODE) {
        fprintf(stderr,"make: dependency graph limit exceeded\n"); return -1;
    }
    int index=nnodes++; struct build_node *node=&nodes[index];
    if(copy_str(node->name,NAME,name)<0) return -1;
    struct rule *ex=find_explicit(name),*source=ex,*pat;
    char pres[MAX_PRE][NAME]; int npre=0;
    node->phony=listed_phony(name)||(ex && ex->phony);
    if(ex) for(int i=0;i<ex->npre;i++) {
        if(copy_str(pres[npre++],NAME,ex->pre[i])<0) return -1;
    }
    if(!ex || !ex->nrec) {
        pat=find_pattern(name,node->stem);
        if(pat) {
            for(int i=0;i<pat->npre;i++) {
                char dep[NAME];
                if(subst_stem(pat->pre[i],node->stem,dep,NAME)<0) return -1;
                int seen=0;
                for(int j=0;j<npre;j++) if(!strcmp(pres[j],dep)) seen=1;
                if(!seen) {
                    if(npre>=MAX_PRE) { fprintf(stderr,"make: too many prerequisites\n"); return -1; }
                    if(copy_str(pres[npre++],NAME,dep)<0) return -1;
                }
            }
            source=pat;
        } else if(!ex && !node->phony) {
            if(!exists(name)) { fprintf(stderr,"make: *** no rule to make target '%s'\n",name); return -1; }
            node->state=NODE_DONE; return index;
        }
    }
    node->recipe=source;
    for(int i=0;i<npre;i++) {
        int dep=plan(pres[i],depth+1);
        if(dep<0) return -1;
        node->deps[node->ndep++]=(unsigned short)dep;
    }
    node->state=NODE_PLANNED;
    return index;
}
static int node_recipes(struct build_node *node) {
    char pres[MAX_PRE][NAME];
    for(int i=0;i<node->ndep;i++) strcpy(pres[i],nodes[node->deps[i]].name);
    return run_recipes(node->recipe,node->name,node->stem,pres,node->ndep);
}
static int reap(int block,int *active,int *failed) {
    int status=0,pid;
    do { pid=waitpid(-1,&status,block?0:WNOHANG); } while(pid==-EINTR);
    if(pid<0) { fprintf(stderr,"make: waitpid failed (%d)\n",pid); return -1; }
    if(!pid) return 0;
    for(int i=0;i<nnodes;i++) if(nodes[i].state==NODE_RUNNING && nodes[i].pid==pid) {
        nodes[i].state=NODE_DONE; nodes[i].built=status==0; (*active)--;
        if(status) *failed=1;
        return 1;
    }
    fprintf(stderr,"make: unexpected child %d\n",pid); return -1;
}
static int execute_graph(void) {
    int active=0,failed=0;
    for(;;) {
        while(active) {
            int result=reap(0,&active,&failed);
            if(result<0) { failed=1; break; }
            if(!result) break;
        }
        if(failed) break;
        int remaining=0,progress=0;
        for(int i=0;i<nnodes;i++) {
            struct build_node *node=&nodes[i];
            if(node->state==NODE_DONE) continue;
            remaining++;
            if(node->state!=NODE_PLANNED || active>=jobs) continue;
            int ready=1,changed=0;
            for(int j=0;j<node->ndep;j++) {
                struct build_node *dep=&nodes[node->deps[j]];
                if(dep->state!=NODE_DONE) ready=0;
                if(dep->built) changed=1;
            }
            if(!ready) continue;
            int need=node->phony || !exists(node->name) || changed;
            if(!need || !node->recipe || !node->recipe->nrec) {
                node->built=need; node->state=NODE_DONE; progress=1; continue;
            }
            any_ran=1;
            /* Dry runs remain deterministic and never fork a recipe. */
            if(dry_run) {
                if(node_recipes(node)<0) { failed=1; break; }
                node->built=1; node->state=NODE_DONE; progress=1; continue;
            }
            fflush(stdout); fflush(stderr);
            int pid=fork();
            if(pid<0) { fprintf(stderr,"make: fork failed (%d)\n",pid); failed=1; break; }
            if(!pid) exit(node_recipes(node)<0?1:0);
            node->pid=pid; node->state=NODE_RUNNING; active++; progress=1;
        }
        if(failed || !remaining) break;
        if(!progress) {
            if(!active || reap(1,&active,&failed)<0) { failed=1; break; }
        }
    }
    /* Stop launching dependants on failure, but reap every worker already
     * started so recipes cannot outlive make or an ext installation marker. */
    while(active) if(reap(1,&active,&failed)<0) break;
    return failed?-1:0;
}
static int job_count(const char *value) {
    int count=build_jobs_parse(value);
    return count<0?0:count?count:build_jobs_auto();
}
static void usage(void) {
    fprintf(stderr, "usage: make [-n] [-j N|-jN|--jobs=N] [-C dir] [-f file] [target...]\n");
}

int main(int argc, char **argv) {
    const char *file = NULL;
    const char *dir = NULL;
    char *goals[8];
    int ngoals = 0, i;
    for (i = 1; i < argc; i++) {
        if(!strncmp(argv[i],"-j",2) || !strncmp(argv[i],"--jobs=",7) || !strcmp(argv[i],"--jobs")) {
            const char *value="auto";
            if(!strncmp(argv[i],"--jobs=",7)) value=argv[i]+7;
            else if(argv[i][1]=='j' && argv[i][2]) value=argv[i]+2;
            else if(i+1<argc && ((argv[i+1][0]>='0' && argv[i+1][0]<='9') || !strcmp(argv[i+1],"auto"))) value=argv[++i];
            jobs=job_count(value);
            if(!jobs) { fprintf(stderr,"make: jobs must be auto or 1..%d\n",MAX_JOBS); return 2; }
            continue;
        }
        if (!strcmp(argv[i], "-n")) { dry_run = 1; continue; }
        if (!strcmp(argv[i], "--help")) { usage(); return 0; }
        if (!strcmp(argv[i], "-f") || !strcmp(argv[i], "-C")) {
            if (i + 1 >= argc) { usage(); return 2; }
            if (argv[i][1] == 'f') file = argv[++i];
            else dir = argv[++i];
            continue;
        }
        if (argv[i][0] == '-') { usage(); return 2; }
        if (ngoals >= 8) { fprintf(stderr, "make: too many targets\n"); return 2; }
        goals[ngoals++] = argv[i];
    }
    if (dir) {
        int r = chdir(dir);
        if (r < 0) {
            fprintf(stderr, "make: %s: %s\n", dir, strerror(-r));
            return 2;
        }
    }
    if (!file) {
        if (exists("Makefile")) file = "Makefile";
        else if (exists("makefile")) file = "makefile";
        else { fprintf(stderr, "make: no Makefile\n"); return 2; }
    }
    if (load_file(file) < 0) return 2;
    if (ngoals == 0) {
        if (!default_goal[0]) { fprintf(stderr, "make: no targets\n"); return 2; }
        goals[ngoals++] = default_goal;
    }
    for (i = 0; i < ngoals; i++)
        if (plan(goals[i],0) < 0) return 2;
    if(execute_graph()<0) return 2;
    if (!any_ran) printf("make: '%s' is up to date\n", goals[0]);
    return 0;
}
