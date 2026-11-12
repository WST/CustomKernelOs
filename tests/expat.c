/* Короткий документ и битый XML. Собирается cc -lexpat. */
#include <expat.h>
#include <stdio.h>
#include <string.h>

static int saw;

static void start(void *user, const XML_Char *name, const XML_Char **atts) {
    (void)user;
    (void)atts;
    if (strcmp(name, "note") == 0) saw++;
}

static void text(void *user, const XML_Char *s, int len) {
    (void)user;
    if (len == 2 && s[0] == 'h' && s[1] == 'i') saw++;
}

int main(void) {
    const char *good = "<note>hi</note>";
    const char *bad = "<note>";
    XML_Parser p = XML_ParserCreate(0);
    if (!p) return 1;
    XML_SetElementHandler(p, start, 0);
    XML_SetCharacterDataHandler(p, text);
    if (XML_Parse(p, good, (int)strlen(good), 1) != XML_STATUS_OK) return 2;
    if (saw != 2) return 3;
    XML_ParserFree(p);

    p = XML_ParserCreate(0);
    if (!p) return 4;
    if (XML_Parse(p, bad, (int)strlen(bad), 1) == XML_STATUS_OK) return 5;
    if (XML_GetErrorCode(p) == XML_ERROR_NONE) return 6;
    XML_ParserFree(p);
    printf("expat: ok\n");
    return 0;
}
