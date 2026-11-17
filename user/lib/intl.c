/* intl.c — имитация gettext. Переводы не подставляются: вызывающий получает свою строку. */
#include "myos.h"
#include "libintl.h"

static char domain_buf[64] = "messages";

char *gettext(const char *msgid) { return (char *)msgid; }
char *dgettext(const char *domain, const char *msgid) { (void)domain; return (char *)msgid; }
char *dcgettext(const char *domain, const char *msgid, int category) {
    (void)domain; (void)category; return (char *)msgid;
}
char *ngettext(const char *msgid, const char *msgid_plural, unsigned long n) {
    return (char *)(n == 1 ? msgid : msgid_plural);
}
char *dngettext(const char *domain, const char *msgid, const char *msgid_plural, unsigned long n) {
    (void)domain; return ngettext(msgid, msgid_plural, n);
}
char *textdomain(const char *domain) {
    if (domain && domain[0]) {
        int i = 0;
        while (domain[i] && i < (int)sizeof domain_buf - 1) { domain_buf[i] = domain[i]; i++; }
        domain_buf[i] = 0;
    }
    return domain_buf;
}
char *bindtextdomain(const char *domain, const char *dir) { (void)domain; (void)dir; return (char *)dir; }
char *bind_textdomain_codeset(const char *domain, const char *codeset) {
    (void)domain; return (char *)codeset;
}
