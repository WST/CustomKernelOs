/* guess — угадай число. Ввод с клавиатуры через системный вызов read. */
#include "myos.h"

int main(void) {
    char line[32];
    srand(ticks() * 2654435761u);
    int secret = rand() % 100 + 1, tries = 0;

    printf("I'm thinking of a number between 1 and 100. Ctrl+C to give up.\n");
    for (;;) {
        printf("Your guess: ");
        if (getline(line, sizeof(line)) < 0) return 1;
        if (!isdigit(line[0])) { printf("  please type a number\n"); continue; }
        int g = atoi(line);
        tries++;
        if (g < secret)      printf("  bigger!\n");
        else if (g > secret) printf("  smaller!\n");
        else { printf("  Correct! %d is right, you needed %d tries.\n", secret, tries); return 0; }
    }
}
