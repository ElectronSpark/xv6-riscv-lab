/**
 * @file ipc_util.c
 * @brief Generic System V IPC ID-table management.
 *
 * Provides init / find / add / remove / get helpers used by
 * shm.c, sem.c, and msg.c.
 */

#include "types.h"
#include "string.h"
#include "defs.h"
#include "printf.h"
#include "ipc.h"

/**
 * ipc_ids_init — zero out an IPC ID table and init its spinlock.
 */
void ipc_ids_init(struct ipc_ids *ids, const char *name)
{
    memset(ids, 0, sizeof(*ids));
    spin_init(&ids->lock, (char *)name);
    ids->max_id = 0;
}

/**
 * ipc_findkey — find the slot index whose key == @key.
 *
 * Caller must hold ids->lock.
 * Returns slot index [0, IPC_MAX_IDS) or -1 if not found.
 */
int ipc_findkey(struct ipc_ids *ids, int key)
{
    for (int i = 0; i < ids->max_id; i++) {
        if (ids->entries[i].in_use && ids->entries[i].key == key)
            return i;
    }
    return -1;
}

/**
 * ipc_addid — allocate a free slot, store @obj.
 *
 * Caller must hold ids->lock.
 * Returns the Linux-style IPC id (idx + seq * IPC_MAX_IDS), or -1 on full.
 */
int ipc_addid(struct ipc_ids *ids, int key, void *obj)
{
    for (int i = 0; i < IPC_MAX_IDS; i++) {
        if (!ids->entries[i].in_use) {
            ids->entries[i].in_use   = 1;
            ids->entries[i].key      = key;
            ids->entries[i].kern_obj = obj;
            ids->entries[i].seq++;
            if (i + 1 > ids->max_id)
                ids->max_id = i + 1;
            return ipc_buildid(i, ids->entries[i].seq);
        }
    }
    return -1;
}

/**
 * ipc_rmid — mark slot @id (Linux IPC id) as free.
 *
 * Caller must hold ids->lock.
 */
void ipc_rmid(struct ipc_ids *ids, int id)
{
    int idx = ipc_id_to_idx(id);
    if (idx < 0 || idx >= IPC_MAX_IDS)
        return;
    ids->entries[idx].in_use   = 0;
    ids->entries[idx].kern_obj = NULL;

    /* Shrink max_id */
    while (ids->max_id > 0 && !ids->entries[ids->max_id - 1].in_use)
        ids->max_id--;
}

/**
 * ipc_getobj — look up the kernel object for @id.
 *
 * Caller must hold ids->lock.
 * Returns the kern_obj pointer or NULL.
 */
void *ipc_getobj(struct ipc_ids *ids, int id)
{
    int idx = ipc_id_to_idx(id);
    if (idx < 0 || idx >= IPC_MAX_IDS)
        return NULL;
    if (!ids->entries[idx].in_use)
        return NULL;
    /* Optionally verify seq — accept any seq for simplicity */
    return ids->entries[idx].kern_obj;
}
