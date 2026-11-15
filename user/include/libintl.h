/* libintl.h — заглушка gettext: строка возвращается как есть, каталоги не читаются. */
#ifndef LIBINTL_H
#define LIBINTL_H

char *gettext(const char *msgid);
char *dgettext(const char *domain, const char *msgid);
char *dcgettext(const char *domain, const char *msgid, int category);
char *ngettext(const char *msgid, const char *msgid_plural, unsigned long n);
char *dngettext(const char *domain, const char *msgid, const char *msgid_plural, unsigned long n);
char *textdomain(const char *domain);
char *bindtextdomain(const char *domain, const char *dir);
char *bind_textdomain_codeset(const char *domain, const char *codeset);

#endif
