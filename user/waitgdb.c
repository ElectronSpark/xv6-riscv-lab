/*
 * waitgdb — launch a program paused for GDB.
 *
 * Usage:  waitgdb <command> [args...]
 *
 * Forks a child, the child calls waitgdb() (EBREAK) to pause itself
 * before exec'ing the target.  The kernel gdbstub blocks the child
 * until a GDB client attaches.
 *
 * Example (from xv6 shell):
 *   $ waitgdb cat README
 *
 * Then from the host:
 *   $ riscv64-unknown-elf-gdb  build/user/cat
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
    if (argc < 2) {
        printf("usage: waitgdb <command> [args...]\n");
        exit(1);
    }

    int pid = fork();
    if (pid < 0) {
        printf("waitgdb: fork failed\n");
        exit(1);
    }

    if (pid == 0) {
        /* Child: pause for debugger, then exec the target. */
        waitgdb();
        exec(argv[1], &argv[1]);
        printf("waitgdb: exec %s failed\n", argv[1]);
        exit(1);
    }

    /* Parent: wait for child. */
    int status = 0;
    waitpid(pid, &status, 0);
    exit(WIFEXITED(status) ? WEXITSTATUS(status) : 1);
}
