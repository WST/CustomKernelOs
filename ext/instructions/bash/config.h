/* Заготовка config.h для MyOS. configure внутри системы не запускается:
 * наш sh не умеет переменные и ветвления. Чего нет в libc, то не объявлено. */
#ifndef _CONFIG_H_
#define _CONFIG_H_

#define _POSIX_VERSION 200809L
#define SHELL 1
#define EXTGLOB_DEFAULT 0
#define BASH_SOURCE_FULLPATH_DEFAULT 0
#define GLOBASCII_DEFAULT 1
#define MB_CUR_MAX 1

#define PROTOTYPES 1
#define __PROTOTYPES 1
#define HAVE_STRINGIZE 1

#define ALIAS 1
#define PUSHD_AND_POPD 1
#define BRACE_EXPANSION 1
#define HELP_BUILTIN 1
#define RESTRICTED_SHELL 1
#define DISABLED_BUILTINS 1
#define PROMPT_STRING_DECODE 1
#define SELECT_COMMAND 1
#define ARRAY_VARS 1
#define DPAREN_ARITHMETIC 1
#define EXTENDED_GLOB 1
#define COND_COMMAND 1
#define ARITH_FOR_COMMAND 1
#define FUNCTION_IMPORT 1
#define CASEMOD_ATTRS 1
#define CASEMOD_EXPANSIONS 1

#include "config-top.h"

#define HAVE_LONG_LONG_INT 1
#define HAVE_UNSIGNED_LONG_LONG_INT 1
#define HAVE_LONG_LONG 1
#define SIZEOF_INT 4
#define SIZEOF_LONG 4
#define SIZEOF_CHAR_P 4
#define SIZEOF_SIZE_T 4
#define SIZEOF_DOUBLE 8
#define SIZEOF_INTMAX_T 8
#define SIZEOF_LONG_LONG 8
#define SIZEOF_WCHAR_T 4

#define HAVE_C_BOOL 1
#define HAVE_STDBOOL_H 1
#define HAVE_STDINT_H 1
#define HAVE_STDDEF_H 1
#define HAVE_STDARG_H 1
#define HAVE_STRING_H 1
#define HAVE_STRINGS_H 1
#define HAVE_STDLIB_H 1
#define HAVE_UNISTD_H 1
#define HAVE_LIMITS_H 1
#define HAVE_CTYPE_H 1
#define HAVE_ERRNO_H 1
#define HAVE_SIGNAL_H 1
#define HAVE_SETJMP_H 1
#define HAVE_LOCALE_H 1
#define HAVE_TIME_H 1
#define HAVE_DIRENT_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TIME_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_FCNTL_H 1
#define HAVE_REGEX_H 1

#define PATH_MAX 1024
#define NAME_MAX 255

/* Типов ниже в системных заголовках нет. autoconf подставляет их так же. */
#define sigset_t int
#define bits16_t short
#define u_bits16_t unsigned short
#define bits32_t int
#define u_bits32_t unsigned int
#define bits64_t long long
#define u_int unsigned int
#define u_long unsigned long
#define socklen_t unsigned int
#define GETGROUPS_T gid_t

#define HAVE_STRUCT_DIRENT_D_INO 1
#define HAVE_STRUCT_DIRENT_D_TYPE 1

#define HAVE_VPRINTF 1
#define HAVE_STRDUP 1
#define HAVE_STRERROR 1
#define HAVE_STRTOL 1
#define HAVE_STRTOLL 1
#define HAVE_STRTOUL 1
#define HAVE_STRTOULL 1
#define HAVE_STRTOD 1
#define HAVE_MEMMOVE 1
#define HAVE_MEMCPY 1
#define HAVE_MEMSET 1
#define HAVE_MEMCHR 1
#define HAVE_STRCASECMP 1
#define HAVE_STRNCASECMP 1
#define HAVE_STRPBRK 1
#define HAVE_STRSPN 1
#define HAVE_STRCSPN 1
#define HAVE_STRSTR 1
#define HAVE_STRCHR 1
#define HAVE_STRRCHR 1
#define HAVE_STRCOLL 1
#define HAVE_STRTOK 1
#define HAVE_QSORT 1
#define HAVE_SETVBUF 1
#define HAVE_GETTIMEOFDAY 1
#define HAVE_ISATTY 1
#define HAVE_DUP2 1
#define HAVE_PIPE 1
#define HAVE_FORK 1
#define HAVE_GETPID 1
#define HAVE_WAITPID 1
#define HAVE_GETUID 1
#define HAVE_GETEUID 1
#define HAVE_GETGID 1
#define HAVE_GETCWD 1
#define HAVE_RENAME 1
#define HAVE_READLINK 1
#define HAVE_SYMLINK 1
#define HAVE_LSTAT 1
#define HAVE_STAT 1
#define HAVE_FSTAT 1
#define HAVE_OPENDIR 1
#define HAVE_REGCOMP 1
#define HAVE_REGEXEC 1
#define HAVE_STRFTIME 1
#define HAVE_MKTIME 1
#define HAVE_LOCALTIME 1
#define HAVE_GMTIME 1
#define HAVE_SETLOCALE 1
#define HAVE_DPRINTF 1
#define HAVE_PWD_H 1
#define HAVE_GETPWUID 1
#define HAVE_GETPWENT 1

#define HAVE_DECL_PRINTF 1
#define HAVE_DECL_STRCPY 1
#define HAVE_DECL_STRTOL 1
#define HAVE_DECL_STRTOLL 1
#define HAVE_DECL_STRTOUL 1
#define HAVE_DECL_STRTOULL 1
#define HAVE_DECL_STRTOIMAX 1
#define HAVE_DECL_STRTOUMAX 1
#define HAVE_STRSIGNAL 1
#define HAVE_DECL_STRSIGNAL 1

#define GETPGRP_VOID 1
#define NAMED_PIPES_MISSING 1

#define PROGRAM "bash"
#define CONF_HOSTTYPE "i386"
#define CONF_OSTYPE "myos"
#define CONF_VENDOR "pc"
#define CONF_MACHTYPE "i386-pc-myos"
#define PACKAGE "bash"
#define LOCALEDIR "/usr/share/locale"

#include "config-bot.h"

extern int locale_utf8locale;
#include <sys/time.h>

#endif /* _CONFIG_H_ */
