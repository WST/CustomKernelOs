/* clear — очистить экран (консоль MyOS понимает символ \f) */
#include "myos.h"

int main(void) {
    write(STDOUT, "\f", 1);
    return 0;
}
