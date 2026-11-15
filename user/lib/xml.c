/* xml.c — дерево элементов из буфера. Сущности только встроенные и числовые.
 * Глубина 64: глубже документ отвергается, а не растит стек без предела. */
#include "myos.h"
#include "../include/libxml/parser.h"

static char *xml_dup(const char *s, int n) {
    char *d;
    if (n < 0) n = 0;
    d = malloc((size_t)n + 1);
    if (!d) return NULL;
    if (n) memcpy(d, s, (size_t)n);
    d[n] = 0;
    return d;
}

static void node_free(xmlNode *n) {
    while (n) {
        xmlNode *next = n->next;
        xmlAttr *a = n->properties;
        node_free(n->children);
        while (a) {
            xmlAttr *an = a->next;
            node_free(a->children);
            free(a->name);
            free(a);
            a = an;
        }
        free(n->name);
        free(n->content);
        free(n);
        n = next;
    }
}

void xmlFree(void *mem) { free(mem); }
void xmlFreeDoc(xmlDocPtr doc) {
    if (!doc) return;
    node_free(doc->children);
    free(doc);
}
void xmlCleanupParser(void) {}

xmlNodePtr xmlDocGetRootElement(xmlDocPtr doc) {
    xmlNode *n;
    if (!doc) return NULL;
    for (n = doc->children; n; n = n->next)
        if (n->type == XML_ELEMENT_NODE) return n;
    return NULL;
}

xmlChar *xmlGetProp(const xmlNode *node, const xmlChar *name) {
    xmlAttr *a;
    const char *v = "";
    if (!node || !name) return NULL;
    for (a = node->properties; a; a = a->next) {
        if (a->name && !strcmp(a->name, (const char *)name)) {
            if (a->children && a->children->content) v = a->children->content;
            return (xmlChar *)xml_dup(v, (int)strlen(v));
        }
    }
    return NULL;
}

struct xml_in { const char *p, *end; int err; };

static int xml_space(int c) { return c == ' ' || c == '\n' || c == '\r' || c == '\t'; }

static int xml_starts(struct xml_in *in, const char *s) {
    int n = (int)strlen(s);
    if (in->end - in->p < n) return 0;
    return memcmp(in->p, s, (size_t)n) == 0;
}

static char *xml_unescape(const char *a, const char *b) {
    char *d = malloc((size_t)(b - a) + 1);
    int n = 0;
    if (!d) return NULL;
    while (a < b) {
        if (*a == '&') {
            const char *e = a + 1;
            int v = -1;
            while (e < b && *e != ';') e++;
            if (e < b) {
                if (e - a == 3 && !memcmp(a, "&lt", 3)) v = '<';
                else if (e - a == 3 && !memcmp(a, "&gt", 3)) v = '>';
                else if (e - a == 4 && !memcmp(a, "&amp", 4)) v = '&';
                else if (e - a == 5 && !memcmp(a, "&quot", 5)) v = '"';
                else if (e - a == 5 && !memcmp(a, "&apos", 5)) v = '\'';
                else if (a[1] == '#') {
                    const char *q = a + 2;
                    v = 0;
                    if (q < e && (*q == 'x' || *q == 'X')) {
                        q++;
                        while (q < e) {
                            int h = *q >= '0' && *q <= '9' ? *q - '0' : *q >= 'a' && *q <= 'f' ? *q - 'a' + 10 : *q >= 'A' && *q <= 'F' ? *q - 'A' + 10 : -1;
                            if (h < 0 || v > 0x10ffff) { v = -1; break; }
                            v = (v << 4) + h; q++;
                        }
                    } else {
                        while (q < e && *q >= '0' && *q <= '9') { v = v * 10 + *q - '0'; q++; }
                    }
                    if (q != e || v < 0 || v > 255) v = -1;
                }
                if (v >= 0) { d[n++] = (char)v; a = e + 1; continue; }
            }
        }
        d[n++] = *a++;
    }
    d[n] = 0;
    return d;
}

static const char *xml_local(const char *name, int *len) {
    int i, cut = 0;
    for (i = 0; i < *len; i++) if (name[i] == ':') cut = i + 1;
    *len -= cut;
    return name + cut;
}

static xmlNode *parse_nodes(struct xml_in *in, xmlNode *parent, int depth);

static xmlNode *parse_element(struct xml_in *in, int depth) {
    const char *name, *local;
    int nlen, llen;
    xmlNode *node;
    if (depth > 64 || in->p >= in->end || *in->p != '<') { in->err = 1; return NULL; }
    in->p++;
    name = in->p;
    while (in->p < in->end && !xml_space(*in->p) && *in->p != '>' && *in->p != '/') in->p++;
    nlen = (int)(in->p - name);
    if (nlen < 1) { in->err = 1; return NULL; }
    llen = nlen;
    local = xml_local(name, &llen);
    node = calloc(1, sizeof *node);
    if (!node) { in->err = 1; return NULL; }
    node->type = XML_ELEMENT_NODE;
    node->name = xml_dup(local, llen);
    if (!node->name) { free(node); in->err = 1; return NULL; }
    for (;;) {
        while (in->p < in->end && xml_space(*in->p)) in->p++;
        if (in->p >= in->end) { in->err = 1; node_free(node); return NULL; }
        if (*in->p == '>' || *in->p == '/') break;
        {
            const char *an = in->p, *alocal;
            int anlen, allen;
            char quote, *val;
            const char *vs, *ve;
            xmlAttr *attr;
            xmlNode *text;
            while (in->p < in->end && !xml_space(*in->p) && *in->p != '=' && *in->p != '>' && *in->p != '/') in->p++;
            anlen = (int)(in->p - an);
            while (in->p < in->end && xml_space(*in->p)) in->p++;
            if (anlen < 1 || in->p >= in->end || *in->p != '=') { in->err = 1; node_free(node); return NULL; }
            in->p++;
            while (in->p < in->end && xml_space(*in->p)) in->p++;
            if (in->p >= in->end) { in->err = 1; node_free(node); return NULL; }
            quote = (*in->p == '"' || *in->p == '\'') ? *in->p : 0;
            if (quote) in->p++;
            vs = in->p;
            while (in->p < in->end && (quote ? *in->p != quote : !xml_space(*in->p) && *in->p != '>' && *in->p != '/')) in->p++;
            ve = in->p;
            if (quote) {
                if (in->p >= in->end || *in->p != quote) { in->err = 1; node_free(node); return NULL; }
                in->p++;
            }
            allen = anlen;
            alocal = xml_local(an, &allen);
            val = xml_unescape(vs, ve);
            attr = calloc(1, sizeof *attr);
            text = calloc(1, sizeof *text);
            if (!val || !attr || !text) { free(val); free(attr); free(text); in->err = 1; node_free(node); return NULL; }
            attr->name = xml_dup(alocal, allen);
            text->type = XML_TEXT_NODE;
            text->content = val;
            text->parent = NULL;
            attr->children = text;
            if (!attr->name) { node_free(text); free(attr); in->err = 1; node_free(node); return NULL; }
            attr->next = node->properties;
            node->properties = attr;
        }
    }
    if (in->p < in->end && *in->p == '/') {
        in->p++;
        if (in->p >= in->end || *in->p != '>') { in->err = 1; node_free(node); return NULL; }
        in->p++;
        return node;
    }
    if (in->p >= in->end || *in->p != '>') { in->err = 1; node_free(node); return NULL; }
    in->p++;
    node->children = parse_nodes(in, node, depth + 1);
    if (in->err) { node_free(node); return NULL; }
    if (!xml_starts(in, "</")) { in->err = 1; node_free(node); return NULL; }
    in->p += 2;
    while (in->p < in->end && xml_space(*in->p)) in->p++;
    while (in->p < in->end && !xml_space(*in->p) && *in->p != '>') in->p++;
    while (in->p < in->end && *in->p != '>') in->p++;
    if (in->p >= in->end) { in->err = 1; node_free(node); return NULL; }
    in->p++;
    return node;
}

static xmlNode *parse_nodes(struct xml_in *in, xmlNode *parent, int depth) {
    xmlNode *first = NULL, *last = NULL;
    while (in->p < in->end && !in->err) {
        xmlNode *n = NULL;
        if (xml_starts(in, "</")) break;
        if (xml_space(*in->p)) { in->p++; continue; }
        if (xml_starts(in, "<!--")) {
            in->p += 4;
            while (in->p + 2 < in->end && !(in->p[0] == '-' && in->p[1] == '-' && in->p[2] == '>')) in->p++;
            if (in->p + 2 >= in->end) { in->err = 1; break; }
            in->p += 3;
            continue;
        }
        if (xml_starts(in, "<?")) {
            in->p += 2;
            while (in->p + 1 < in->end && !(in->p[0] == '?' && in->p[1] == '>')) in->p++;
            if (in->p + 1 >= in->end) { in->err = 1; break; }
            in->p += 2;
            continue;
        }
        if (xml_starts(in, "<![CDATA[")) {
            const char *a;
            in->p += 9;
            a = in->p;
            while (in->p + 2 < in->end && !(in->p[0] == ']' && in->p[1] == ']' && in->p[2] == '>')) in->p++;
            n = calloc(1, sizeof *n);
            if (!n) { in->err = 1; break; }
            n->type = XML_TEXT_NODE;
            n->content = xml_dup(a, (int)(in->p - a));
            if (!n->content) { free(n); in->err = 1; break; }
            if (in->p + 2 < in->end) in->p += 3;
            else in->err = 1;
        } else if (xml_starts(in, "<!")) {
            int nest = 0;
            in->p += 2;
            while (in->p < in->end && !(nest == 0 && *in->p == '>')) {
                if (*in->p == '[') nest++;
                else if (*in->p == ']' && nest) nest--;
                in->p++;
            }
            if (in->p < in->end) in->p++;
            continue;
        } else if (*in->p == '<') {
            n = parse_element(in, depth);
            if (!n) break;
        } else {
            const char *a = in->p;
            int any = 0;
            while (in->p < in->end && *in->p != '<') { if (!xml_space(*in->p)) any = 1; in->p++; }
            if (!any) continue;
            n = calloc(1, sizeof *n);
            if (!n) { in->err = 1; break; }
            n->type = XML_TEXT_NODE;
            n->content = xml_unescape(a, in->p);
            if (!n->content) { free(n); in->err = 1; break; }
        }
        if (!n) continue;
        n->parent = parent;
        if (last) last->next = n; else first = n;
        last = n;
    }
    return first;
}

xmlDocPtr xmlReadMemory(const char *buffer, int size, const char *url, const char *encoding, int options) {
    struct xml_in in;
    xmlDocPtr doc;
    (void)url; (void)encoding; (void)options;
    if (!buffer || size < 0) return NULL;
    doc = calloc(1, sizeof *doc);
    if (!doc) return NULL;
    in.p = buffer; in.end = buffer + size; in.err = 0;
    doc->children = parse_nodes(&in, NULL, 0);
    if (in.err) { xmlFreeDoc(doc); return NULL; }
    return doc;
}
