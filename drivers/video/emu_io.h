#ifndef MYOS_EMU_IO_H
#define MYOS_EMU_IO_H
/* The upstream default memio backend is unused. Keep its I/O inert. */
#define inb(p) ((void)(p),0xffu)
#define inw(p) ((void)(p),0xffffu)
#define inl(p) ((void)(p),0xffffffffu)
#define outb(v,p) do {(void)(v);(void)(p);}while(0)
#define outw(v,p) do {(void)(v);(void)(p);}while(0)
#define outl(v,p) do {(void)(v);(void)(p);}while(0)
#endif
