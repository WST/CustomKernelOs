#ifndef SIGNAL_H
#define SIGNAL_H
/* Ядро не доставляет сигналы: Ctrl+C убивает группу переднего плана.
 * signal() только запоминает обработчик, чтобы интерпретатор Lua собрался. */
typedef int sig_atomic_t;
#define SIGINT 2
#define SIG_ERR ((void (*)(int))-1)
#define SIG_DFL ((void (*)(int))0)
#define SIG_IGN ((void (*)(int))1)
void (*signal(int sig, void (*handler)(int)))(int);
#endif
