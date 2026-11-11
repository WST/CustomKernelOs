#ifndef E1000_H
#define E1000_H
#include "types.h"

bool e1000_init(void);
const char *e1000_name(void);
bool e1000_link(void);
void e1000_diagnose(void); /* passive queue/MAC snapshot; no reset or recovery */
void e1000_mac(u8 mac[6]);
int  e1000_send(const void *frame, u32 len);          /* 0 или -1 (очередь полна) */
int  e1000_receive(void *buf, u32 max);               /* длина кадра, 0 — ничего нет */
void e1000_stats(u32 *rx, u32 *tx, u32 *rx_bytes, u32 *tx_bytes);

#endif
