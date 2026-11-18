/* parser.h — документ из памяти. Сеть, внешние сущности и DTD не читаются. */
#ifndef LIBXML_PARSER_H
#define LIBXML_PARSER_H
#include "tree.h"

#define XML_PARSE_RECOVER 1

xmlDocPtr xmlReadMemory(const char *buffer, int size, const char *url, const char *encoding, int options);
xmlNodePtr xmlDocGetRootElement(xmlDocPtr doc);
void xmlFreeDoc(xmlDocPtr doc);
void xmlCleanupParser(void);
#endif
