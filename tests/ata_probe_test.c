/* Host regression test: no real port I/O; compile with -fno-builtin. */
#define TYPES_H
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long size_t;
typedef _Bool bool;
typedef __builtin_va_list va_list;
#define true 1
#define false 0
#define NULL ((void *)0)

extern int puts(const char *);
static u8 status,mid,hi;
static int logs,words,busy_reads,signature_while_busy;
static u32 ticks;
static u8 inb(u16 port) {
    switch(port&7) {
    case 7:
        if(busy_reads>0) { busy_reads--; return 0x80; }
        return status;
    case 4: if(busy_reads) signature_while_busy++; return mid;
    case 5: if(busy_reads) signature_while_busy++; return hi;
    case 1: return 4;
    default: return 0;
    }
}
static void outb(u16 port,u8 value) { (void)port; (void)value; }
static u16 inw(u16 port) {
    (void)port;
    int word=words++;
    if(word==49) return 1<<9;
    if(word==60) return 8192;
    return 0;
}
static void outw(u16 port,u16 value) { (void)port; (void)value; }
u32 timer_ticks(void) { return ticks++; }
void kprintf(const char *fmt,...) { (void)fmt; logs++; }

#include "../kernel/ata.c"

static void reset(u8 st,u8 m,u8 h) {
    status=st; mid=m; hi=h;
    logs=words=busy_reads=signature_while_busy=0; ticks=0;
}
#define CHECK(x) do { if(!(x)) { puts("FAIL: " #x); return 1; } } while(0)
int main(void) {
    reset(0x41,0x14,0xeb);
    CHECK(!identify(1)); CHECK(!logs); CHECK(!words);
    CHECK(!ata_unit_sectors(1));
    reset(0x41,0x69,0x96);
    CHECK(!identify(1)); CHECK(!logs); CHECK(!words);
    reset(0x41,0,0);
    CHECK(!identify(1)); CHECK(logs==1); CHECK(!words);
    reset(0,0,0);
    CHECK(!identify(1)); CHECK(!logs);
    reset(0xff,0,0);
    CHECK(!identify(1)); CHECK(!logs);
    reset(0x48,0,0); busy_reads=3;
    CHECK(identify(1)); CHECK(!logs); CHECK(words==256);
    CHECK(ata_unit_sectors(1)==8192); CHECK(!signature_while_busy);
    reset(0x80,0,0);
    CHECK(!identify(1)); CHECK(logs==1); CHECK(!words);
    /* A packet-like signature during ordinary I/O must not hide errors. */
    reset(0x41,0x14,0xeb);
    CHECK(wait(1,true)<0); CHECK(logs==1);
    puts("ATA probe: PASS (packet/ATA/absent/busy/timeout/I/O errors)");
    return 0;
}
