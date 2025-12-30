/*
 * Syscall wrappers for unit tests
 */

void __wrap_argint(int n, int *ip) {
    (void)n;
    if (ip) {
        *ip = 0;
    }
}
