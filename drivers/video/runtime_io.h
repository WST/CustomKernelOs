#ifndef VIDEO_RUNTIME_IO_H
#define VIDEO_RUNTIME_IO_H
#include "../../kernel/types.h"
#ifdef HOST_TEST
u32 video_bios_port_read(u16 port,u32 bytes);
void video_bios_port_write(u16 port,u32 value,u32 bytes);
#else
static inline u32 video_bios_port_read(u16 port,u32 bytes) {
    if(bytes==1)return inb(port);
    if(bytes==2)return inw(port);
    u32 value;__asm__ volatile("inl %1,%0":"=a"(value):"Nd"(port));return value;
}
static inline void video_bios_port_write(u16 port,u32 value,u32 bytes) {
    if(bytes==1)outb(port,value);
    else if(bytes==2)outw(port,value);
    else __asm__ volatile("outl %0,%1"::"a"(value),"Nd"(port));
}
#endif
#endif
