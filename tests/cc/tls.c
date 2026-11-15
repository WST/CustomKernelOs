/* _Thread_local живёт в слоте потока, а не в общей ячейке. */
_Thread_local int slot = 7;
static _Thread_local int hidden;

int main(void) {
    if (slot != 7) return 1;
    slot = 3;
    if (slot != 3) return 2;
    hidden = 9;
    if (hidden != 9) return 3;
    return 0;
}
