/* Сборка libffi 3.8.0 для MyOS. long double совпадает с double.
 * Скрытых символов и статических трамплинов нет. */
#ifndef FFICONFIG_H
#define FFICONFIG_H 1
#define HAVE_MEMCPY 1
#define HAVE_STDINT_H 1
#define HAVE_STDIO_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_STRINGS_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_UNISTD_H 1
#define STDC_HEADERS 1
#define SIZEOF_DOUBLE 8
#define SIZEOF_LONG_DOUBLE 8
#define SIZEOF_SIZE_T 4
#define PACKAGE "libffi"
#define PACKAGE_NAME "libffi"
#define PACKAGE_VERSION "3.8.0"
#define VERSION "3.8.0"
#define EH_FRAME_FLAGS "a"
#define HAVE_AS_X86_PCREL 1
#ifdef LIBFFI_ASM
#define FFI_HIDDEN(name)
#else
#define FFI_HIDDEN
#ifndef __GNUC__
#define __declspec(x)
#endif
#endif
#endif
