#!/bin/sh
# patch_vim_exit.sh — Patch VIM's mch_exit() to force-restore terminal state.
# VIM's normal terminal restoration can leave ONLCR cleared on xv6,
# causing missing carriage returns after exit.  This inserts an explicit
# tcsetattr() right before the exit(r) call in mch_exit().

FILE="$1"
if [ ! -f "$FILE" ]; then
    echo "patch_vim_exit.sh: $FILE not found" >&2
    exit 1
fi

# Only patch if not already patched
if grep -q 'xv6: force-restore' "$FILE"; then
    exit 0
fi

sed -i '/^    exit(r);$/i\
    /* xv6: force-restore sane terminal state before exit */\
    {\
        struct termios __xt;\
        if (tcgetattr(0, \&__xt) == 0) {\
            __xt.c_iflag |= (ICRNL | IXON);\
            __xt.c_oflag |= (OPOST | ONLCR);\
            __xt.c_lflag |= (ICANON | ECHO | ISIG | ECHOE);\
            tcsetattr(0, TCSANOW, \&__xt);\
        }\
    }' "$FILE"
