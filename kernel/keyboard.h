#ifndef KEYBOARD_H
#define KEYBOARD_H

#define KEY_UP    ((char)0x80)
#define KEY_DOWN  ((char)0x81)
#define KEY_LEFT  ((char)0x82)
#define KEY_RIGHT ((char)0x83)
#define KEY_HOME  ((char)0x84)
#define KEY_END   ((char)0x85)
#define KEY_PGUP  ((char)0x86)
#define KEY_PGDN  ((char)0x87)
#define KEY_DEL   ((char)0x88)
#define KEY_INS   ((char)0x89)
#define KEY_CTRL_C ((char)3)
#define KEY_CTRL_D ((char)4)

void keyboard_init(void);
char kb_getchar(void);   /* блокирующее чтение символа */
int  kb_poll(void);      /* без ожидания: код или -1 */
void *kb_channel(void);  /* на чём спать в ожидании клавиши */

#endif
