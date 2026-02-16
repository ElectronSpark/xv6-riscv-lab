#include "user/user.h"

static void usage(void) {
    printf("Usage: ps [options]\n");
    printf("  -l        flat thread list (default)\n");
    printf("  -t [pid]  parent-child tree (from pid, or all)\n");
    printf("  -s [sid]  session/pgroup hierarchy (for sid, or all)\n");
    printf("  -b [pid]  blocked-thread backtraces (for pid, or all)\n");
}

int main(int argc, char *argv[]) {
    int mode = 0;
    int id = -1; // -1 means "all"
    int i = 1;

    while (i < argc) {
        if (argv[i][0] == '-' && argv[i][1] && !argv[i][2]) {
            switch (argv[i][1]) {
            case 'l':
                mode = 0;
                break;
            case 't':
                mode = 1;
                break;
            case 's':
                mode = 2;
                break;
            case 'b':
                mode = 3;
                break;
            default:
                usage();
                return 1;
            }
            // Check for optional numeric argument
            if (mode >= 1 && i + 1 < argc && argv[i + 1][0] >= '0' &&
                argv[i + 1][0] <= '9') {
                i++;
                id = atoi(argv[i]);
            }
        } else {
            usage();
            return 1;
        }
        i++;
    }
    dumpproc(mode, id);
    return 0;
}
