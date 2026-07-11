#ifndef XV6_KQUEUE_GRAPH_WALK_H
#define XV6_KQUEUE_GRAPH_WALK_H

/*
 * Generic, allocation-free breadth-first graph walker.  References stored in
 * state are owned by the caller until kqueue_graph_walk_release_all().
 * snapshot_children() must return retained references and set *count even on
 * failure so the common cleanup path can release partial snapshots.
 */
struct kqueue_graph_walk_ops {
    void *(*identity)(void *context, void *reference);
    int (*snapshot_children)(void *context, void *parent_reference,
                             void **children, int capacity, int *count);
    void (*release)(void *context, void *reference);
};

struct kqueue_graph_walk_state {
    void **references;
    void **scratch;
    int capacity;
    int count;
    int cursor;
};

static inline int kqueue_graph_walk_reaches(
    const struct kqueue_graph_walk_ops *ops, void *context,
    struct kqueue_graph_walk_state *state, void *start_reference,
    void *needle_identity, int overflow_error)
{
    state->count = 0;
    state->cursor = 0;
    if (state->capacity <= 0) {
        ops->release(context, start_reference);
        return overflow_error;
    }
    state->references[state->count++] = start_reference;

    while (state->cursor < state->count) {
        void *parent = state->references[state->cursor++];
        void *identity = ops->identity(context, parent);
        if (identity == needle_identity)
            return 1;
        if (identity == NULL)
            continue;

        int child_count = 0;
        int remaining = state->capacity - state->count;
        int ret = ops->snapshot_children(context, parent, state->scratch,
                                         remaining, &child_count);
        if (ret != 0) {
            for (int i = 0; i < child_count; i++)
                ops->release(context, state->scratch[i]);
            return ret;
        }

        for (int i = 0; i < child_count; i++) {
            void *child_identity = ops->identity(context, state->scratch[i]);
            bool seen = false;
            for (int j = 0; j < state->count; j++) {
                if (ops->identity(context, state->references[j]) ==
                    child_identity) {
                    seen = true;
                    break;
                }
            }
            if (seen) {
                ops->release(context, state->scratch[i]);
                continue;
            }
            if (state->count == state->capacity) {
                ops->release(context, state->scratch[i]);
                for (int j = i + 1; j < child_count; j++)
                    ops->release(context, state->scratch[j]);
                return overflow_error;
            }
            state->references[state->count++] = state->scratch[i];
        }
    }
    return 0;
}

static inline void kqueue_graph_walk_release_all(
    const struct kqueue_graph_walk_ops *ops, void *context,
    struct kqueue_graph_walk_state *state)
{
    for (int i = 0; i < state->count; i++)
        ops->release(context, state->references[i]);
    state->count = 0;
    state->cursor = 0;
}

#endif
