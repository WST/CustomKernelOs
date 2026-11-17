/* tree.h — узел, свойство и документ. Без XPath, DTD и схем. */
#ifndef LIBXML_TREE_H
#define LIBXML_TREE_H
#include "xmlversion.h"

typedef unsigned char xmlChar;

#define XML_ELEMENT_NODE 1
#define XML_TEXT_NODE 3

typedef struct _xmlNode xmlNode;
typedef xmlNode *xmlNodePtr;
typedef struct _xmlAttr xmlAttr;
typedef xmlAttr *xmlAttrPtr;
typedef struct _xmlDoc xmlDoc;
typedef xmlDoc *xmlDocPtr;

struct _xmlAttr {
    xmlAttr *next;
    xmlNode *children;
    char *name;
};

struct _xmlNode {
    xmlNode *next;
    xmlNode *parent;
    xmlNode *children;
    xmlAttr *properties;
    int type;
    char *name;
    char *content;
};

struct _xmlDoc {
    xmlNode *children;
};

xmlChar *xmlGetProp(const xmlNode *node, const xmlChar *name);
void xmlFree(void *mem);
#endif
