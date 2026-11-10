#ifndef IPC_H
#define IPC_H
#include "types.h"

/* Общение процессов (для оконной системы и не только):
 *   общая память — одни и те же физические страницы видны в нескольких процессах;
 *   сообщения    — у каждого процесса почтовый ящик на 64 письма по 64 байта;
 *   имена служб  — «рабочий стол» регистрируется как "desktop", остальные его находят. */

#define SHM_MAX        64           /* объектов на всю систему */
#define SHM_PER_TASK   16           /* отображений в одном процессе */
#define MSG_SIZE       64
#define MBOX_LEN       64

struct task;
int  shm_alloc(u32 size);           /* -> id (сразу отображён в процесс? нет — см. shm_map) */
int  shm_map(int id, u32 *addr);    /* отобразить в текущий процесс */
int  shm_unmap(u32 addr);
void shm_release_all(struct task *t, u32 dir);   /* процесс умер или сделал exec */

int  msg_send(u32 pid, const void *msg);         /* msg в памяти ядра, MSG_SIZE байт */
int  msg_recv(void *msg, int timeout_ms);        /* 0 — не ждать, <0 — ждать вечно; screen input wakes its owner with -EAGAIN */
void msg_free(struct task *t);
void msg_input_notify(u32 pid); /* wake the screen owner without fabricating an IPC message */

int  svc_register(const char *name);
int  svc_find(const char *name);

struct rtc_time { u32 year, month, day, hour, min, sec; };
void rtc_read(struct rtc_time *t);

#endif
