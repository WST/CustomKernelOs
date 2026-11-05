#ifndef KEYBOARD_H
#define KEYBOARD_H

#define KEY_UP    0x80
#define KEY_DOWN  0x81
#define KEY_LEFT  0x82
#define KEY_RIGHT 0x83
#define KEY_HOME  0x84
#define KEY_END   0x85
#define KEY_PGUP  0x86
#define KEY_PGDN  0x87
#define KEY_DEL   0x88
#define KEY_INS   0x89
#define KEY_CTRL_C 3
#define KEY_CTRL_D 4

void keyboard_init(void);
#define KEY_UNI   0x100             /* KEY_UNI + кодовая точка (буквы не латиницей) */
int  kb_getchar(void);   /* блокирующее чтение клавиши */
int  kb_layout(void);    /* 0 — US, 1 — русская */
void kb_set_layout(int l);
int  kb_poll(void);      /* без ожидания: код или -1 */
void *kb_channel(void);  /* на чём спать в ожидании клавиши */

#endif
