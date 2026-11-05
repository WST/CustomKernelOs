/* domint.h — общее для dom.c (дерево, разбор HTML, селекторы, CSS) и
 * domjs.c (объекты для JavaScript, события, таймеры). */
#ifndef DOMINT_H
#define DOMINT_H
#include "dom.h"
#include "jsint.h"

#ifndef JS_HOST
#define isupper(c) ((c) >= 'A' && (c) <= 'Z')
#endif

enum { DN_WINDOW = 0, DN_ELEMENT = 1, DN_TEXT = 3, DN_COMMENT = 8, DN_DOCUMENT = 9, DN_FRAGMENT = 11 };

struct listener { jsstr *type; jsval fn; int capture, once; };

typedef struct dnode dnode;
struct dnode {
    int id, type;
    char *tag;                      /* у элементов: имя тега маленькими буквами */
    char **attr;                    /* пары имя, значение */
    int nattr, acap;
    char *text;                     /* у текста и комментариев */
    int tlen;
    dnode *parent, *first, *last, *next, *prev;
    jsobj *wrap;                    /* объект для JS (создаётся при первом обращении) */
    struct listener *ls;
    int nls, lcap;
    ju8 script_done, parser_inserted;
};

extern dnode *d_document, *d_window, **d_nodes;
extern int d_count;
extern struct dom_host *d_host;
extern char d_url[1024];
extern int d_parsing;

/* дерево */
dnode *dn_new(int type, const char *tag);
dnode *dn_text(const char *s, int n);
void   dn_append(dnode *parent, dnode *child);
void   dn_insert_before(dnode *parent, dnode *child, dnode *ref);
void   dn_remove(dnode *child);
const char *dn_get_attr(dnode *n, const char *name);
void   dn_set_attr(dnode *n, const char *name, const char *value);
void   dn_del_attr(dnode *n, const char *name);
dnode *dn_by_id(dnode *root, const char *id);
dnode *dn_find_tag(dnode *root, const char *tag);
int    dn_connected(dnode *n);
dnode *dn_clone(dnode *n, int deep);
void   dn_text_content(dnode *n, jsbuf *b, int inner_text);
void   dn_set_text(dnode *n, const char *s, int len);
int    dn_has_class(dnode *n, const char *cls, int len);
int    dn_is_void(const char *tag);

/* HTML */
void   html_parse_into(dnode *parent, const char *s, int n);   /* фрагмент (innerHTML) — без скриптов */
void   html_serialize(jsbuf *b, dnode *n, int outer);         /* точный HTML (innerHTML/outerHTML) */
void   html_decode(const char *s, int n, jsbuf *out);         /* &amp; -> & и т. д. (в UTF-8) */
void   html_write(const char *s, int n);                       /* document.write во время разбора */

/* селекторы и CSS */
struct selector;
struct selector *sel_parse(const char *s);
void   sel_free(struct selector *s);
int    sel_match(struct selector *s, dnode *n);
void   sel_query(dnode *root, struct selector *s, int first_only, dnode ***out, int *count);
void   css_add_sheet(const char *s, int n);
int    css_hidden(dnode *n);
int    style_hidden(const char *st);
int    dn_visible(dnode *n);
void   css_reset(void);

/* domjs.c */
jsval  dn_wrap(dnode *n);
dnode *dn_of(jsval v);
void   domjs_init_realm(void);
void   domjs_run_script(const char *src, int len, const char *name, dnode *script);
void   domjs_script_inserted(dnode *n);
void   domjs_fire(dnode *target, const char *type, int bubbles, int cancelable, int *prevented);
void   domjs_reset(void);
void   domjs_mark(void);

#endif
