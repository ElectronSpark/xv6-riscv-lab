/*
 * histedit.h — Minimal libedit (editline) shim for dash.
 *
 * Maps the BSD editline API that dash uses onto GNU readline + a tiny
 * built-in history implementation.  Only the subset actually called by
 * dash is provided.
 */

#ifndef _HISTEDIT_H_
#define _HISTEDIT_H_

#include <stdio.h>

/* ------------------------------------------------------------------ */
/* Opaque handles                                                      */
/* ------------------------------------------------------------------ */
typedef struct editline   EditLine;
typedef struct history    History;

typedef struct {
    int         num;
    const char *str;
} HistEvent;

/* ------------------------------------------------------------------ */
/* History operation codes (H_*)                                       */
/* ------------------------------------------------------------------ */
#define H_SETSIZE    1
#define H_ENTER      2
#define H_APPEND     3
#define H_FIRST      4
#define H_LAST       5
#define H_NEXT       6
#define H_PREV       7
#define H_NEXT_EVENT 8
#define H_PREV_STR   9

/* ------------------------------------------------------------------ */
/* EditLine option codes (EL_*)                                        */
/* ------------------------------------------------------------------ */
#define EL_HIST       1
#define EL_PROMPT_ESC 2
#define EL_EDITOR     3
#define EL_TERMINAL   4

/* ------------------------------------------------------------------ */
/* History API                                                         */
/* ------------------------------------------------------------------ */
History *history_init(void);
void     history_end(History *);
int      history(History *, HistEvent *, int op, ...);

/* ------------------------------------------------------------------ */
/* EditLine API                                                        */
/* ------------------------------------------------------------------ */
EditLine   *el_init(const char *prog, FILE *fin, FILE *fout, FILE *ferr);
void        el_end(EditLine *);
int         el_set(EditLine *, int op, ...);
int         el_source(EditLine *, const char *);
const char *el_gets(EditLine *, int *);

#endif /* _HISTEDIT_H_ */
