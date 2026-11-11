/* Канал в ядре — 4096 байт (PIPE_SIZE в kernel/file.h).
 * psize.sh это не измерит: запись без читателя кончается EPIPE, а сигнал не приходит. */
#define PIPESIZE 4096
