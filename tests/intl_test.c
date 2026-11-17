/* Заглушка gettext возвращает исходную строку и не читает каталоги. */
#include "myos.h"
#include "stdio.h"
#include "string.h"
#include "libintl.h"

int main(void) {
    if (strcmp(gettext("hello"), "hello")) return 1;
    if (strcmp(ngettext("one", "many", 1), "one")) return 1;
    if (strcmp(ngettext("one", "many", 2), "many")) return 1;
    if (!textdomain("myos") || !bindtextdomain("myos", "/nowhere") || !bind_textdomain_codeset("myos", "UTF-8"))
        return 1;
    printf("intl ok\n");
    return 0;
}
