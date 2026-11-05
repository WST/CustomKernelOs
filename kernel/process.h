#ifndef PROCESS_H
#define PROCESS_H
#include "types.h"
#include "idt.h"

#define USER_STACK_PAGES 16             /* 64 КБ стека у программы */
#define PROC_MAX_ARGS    32

enum {
    PROC_ENOENT  = -1,
    PROC_ENOEXEC = -2,
    PROC_ENOMEM  = -3,
    PROC_EAGAIN  = -4,
    PROC_EISDIR  = -5,
};

/* Загрузить ELF-файл и запустить его как процесс. -> pid или PROC_E* */
int  proc_spawn(const char *path, u32 cwd, int argc, char **argv, bool foreground);
const char *proc_strerror(int err);

int  proc_fork(struct regs *r);         /* -> id потомка или PROC_E* */
int  proc_exec(struct regs *r, const char *path, int argc, char **argv);

/* группа процессов «на переднем плане» — её убивает Ctrl+C */
void proc_set_foreground(u32 pgid);
u32  proc_foreground(void);
bool proc_ctrl_c(void);                 /* из обработчика клавиатуры; true = кого-то убили */

#endif
