/*
 * waitgdb — launch a program paused for GDB.
 *
 * Usage:  waitgdb [-e] <command> [args...]
 *
 * Forks a child, the child calls waitgdb() (EBREAK) to pause itself
 * before exec'ing the target.  The kernel gdbstub blocks the child
 * until a GDB client attaches.
 *
 * Options:
 *   -e   Stop at the entry point of the exec'd program.  Without this
 *        flag the process runs freely after GDB continues from the
 *        initial EBREAK (useful when you've already set breakpoints).
 *
 * Example (from xv6 shell):
 *   $ waitgdb python
 *   $ waitgdb -e python      # also stop at _start after exec
 *
 * Then from the host:
 *   $ riscv64-unknown-elf-gdb  build/user/python
 *   (gdb) target remote localhost:1234
 *   (gdb) attach <pid printed by kernel>
 *   (gdb) b main
 *   (gdb) continue
 */
#include "kernel/inc/types.h"
#include "kernel/inc/vfs/stat.h"
#include "user/user.h"

int main(int argc, char *argv[])
{
    int stop_entry = 0;
    int cmd_idx = 1;

    if (argc >= 2 && strcmp(argv[1], "-e") == 0) {
        stop_entry = 1;
        cmd_idx = 2;
    }

    if (cmd_idx >= argc) {
        printf("usage: waitgdb [-e] <command> [args...]\n");
        exit(1);
    }

    int pid = fork();
    if (pid < 0) {
        printf("waitgdb: fork failed\n");
        exit(1);
    }

    if (pid == 0) {
        /* Child: pause for debugger, then exec the target. */
        if (stop_entry)
            waitgdb_stopentry();
        else
            waitgdb();
        exec(argv[cmd_idx], &argv[cmd_idx]);
        printf("waitgdb: exec %s failed\n", argv[cmd_idx]);
        exit(1);
    }

    /* Parent: wait for child. */
    int status = 0;
    waitpid(pid, &status, 0);
    exit(WIFEXITED(status) ? WEXITSTATUS(status) : 1);
}
