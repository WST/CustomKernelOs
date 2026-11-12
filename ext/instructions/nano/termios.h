#ifndef NANO_TERMIOS_H
#define NANO_TERMIOS_H
struct termios {unsigned c_iflag,c_oflag,c_cflag,c_lflag;unsigned char c_cc[32];unsigned myos_mode;};
#define OPOST 1
#define ONLCR 4
#define ICRNL 1
#define IXON 2
#define ISIG 1
#define ICANON 2
#define ECHO 4
#define IEXTEN 8
#define TCSANOW 0
#define TCSAFLUSH 2
#define VINTR 0
#define VQUIT 1
#define VSUSP 2
#define VSTOP 3
#define VSTART 4
#define VMIN 5
#define VTIME 6
#define tcgetattr nano_tcgetattr
#define tcsetattr nano_tcsetattr
#endif
