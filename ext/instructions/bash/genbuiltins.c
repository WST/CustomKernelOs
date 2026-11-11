#include "myos.h"

int main(void) {
    char *args[] = {
        "mkbuiltins",
        "-externfile", "builtext.h",
        "-includefile", "builtext.h",
        "-structfile", "builtins.c",
        "-D", ".",
        "alias.def",
        "bind.def",
        "break.def",
        "builtin.def",
        "caller.def",
        "cd.def",
        "colon.def",
        "command.def",
        "complete.def",
        "declare.def",
        "echo.def",
        "enable.def",
        "eval.def",
        "exec.def",
        "exit.def",
        "fc.def",
        "fg_bg.def",
        "getopts.def",
        "hash.def",
        "help.def",
        "history.def",
        "jobs.def",
        "kill.def",
        "let.def",
        "mapfile.def",
        "printf.def",
        "pushd.def",
        "read.def",
        "reserved.def",
        "return.def",
        "set.def",
        "setattr.def",
        "shift.def",
        "shopt.def",
        "source.def",
        "suspend.def",
        "test.def",
        "times.def",
        "trap.def",
        "type.def",
        "ulimit.def",
        "umask.def",
        "wait.def",
        0
    };
    /* $PRODUCES в .def — это «alias.c», без каталога. Запуск из builtins/,
     * иначе файл затрёт одноимённый alias.c в корне исходников. */
    if (chdir("builtins") < 0) {
        dprintf(STDERR, "genbuiltins: cannot enter builtins\n");
        return 1;
    }
    exec("../obj/mkbuiltins", args);
    dprintf(STDERR, "genbuiltins: cannot run mkbuiltins\n");
    return 1;
}
