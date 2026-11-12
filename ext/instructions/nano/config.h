#ifndef MYOS_NANO_CONFIG_H
#define MYOS_NANO_CONFIG_H
#undef __linux__
#define PACKAGE "nano"
#define PACKAGE_NAME "GNU nano"
#define PACKAGE_VERSION "9.3"
#define PACKAGE_STRING "GNU nano 9.3"
#define VERSION "9.3"
#define SYSCONFDIR "/etc"
#define LOCALEDIR "/usr/share/locale"
#define HAVE_LIMITS_H 1
#define HAVE_PWD_H 1
#define HAVE_TERMIOS_H 1
#define HAVE_WCHAR_H 1
#define HAVE_WCTYPE_H 1
#define HAVE_GETOPT_H 1
#define ENABLE_UTF8 1
#define ENABLE_HELP 1
#define ENABLE_MULTIBUFFER 1
#define ENABLE_LINENUMBERS 1
#define ENABLE_WRAPPING 1
#define HAVE_STRCASECMP 1
#define HAVE_STRNCASECMP 1
#define HAVE_ISBLANK 1
#define NANO_REG_EXTENDED REG_EXTENDED
#define _GL_ATTRIBUTE_FORMAT_PRINTF(a,b)
#define _GL_ATTRIBUTE_NORETURN __attribute__((noreturn))
#define _GL_ATTRIBUTE_MALLOC
#define _GL_ATTRIBUTE_DEALLOC_FREE
#define _GL_ATTRIBUTE_ALLOC_SIZE(a)
#define _GL_ATTRIBUTE_NONNULL(a)
#define FALLTHROUGH ((void)0)
#include "myos-posix.h"
#endif
