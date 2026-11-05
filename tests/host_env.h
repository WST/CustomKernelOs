#define HOST_TEST 1
/* Подставляется в heap.c при сборке тестов на хосте (gcc -include):
 * куча живёт в обычном буфере процесса вместо адреса 0xD0000000. */
extern unsigned long heap_base;
#define HEAP_START heap_base
#define HEAP_MAX   (heap_base + (8u << 20))      /* 8 МБ */
