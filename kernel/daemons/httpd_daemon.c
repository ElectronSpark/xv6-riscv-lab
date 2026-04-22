/*
 * httpd_daemon — Kernel-resident HTTP server daemon
 *
 * Uses lwIP's built-in httpd application to serve files over HTTP.
 * Files are served from the xv6 VFS via lwIP's custom filesystem hooks,
 * with the embedded default pages (404.html, index.html) as fallback.
 *
 * The web root is HTTPD_VFS_ROOT (default "/www").  Requests for "/"
 * are mapped to "/index.html".
 *
 * httpd uses the raw TCP API and runs entirely within the tcpip thread,
 * so no separate kthread is needed.
 *
 * QEMU network: guest port 80, reachable from host as localhost:8080
 *   via QEMU SLIRP hostfwd.
 *
 * Usage from host:
 *   curl http://localhost:8080/
 */

#include "types.h"
#include "param.h"
#include "riscv.h"
#include "defs.h"
#include "printf.h"
#include "string.h"
#include "errno.h"
#include <vfs/vfs_types.h>
#include <vfs/file.h>
#include <vfs/fcntl.h>
#include <vfs/fs.h>
#include <vfs/stat.h>
#include <mm/vm.h>

#include "lwip/apps/httpd.h"
#include "lwip/apps/fs.h"

/* ──────────────────────────────────────────────────────────────────────────── */
/* Configuration                                                               */
/* ──────────────────────────────────────────────────────────────────────────── */

/* VFS directory from which HTTP files are served */
#define HTTPD_VFS_ROOT "/www"

/* Maximum path length for VFS lookups */
#define HTTPD_PATH_MAX 256

/* Maximum simultaneous open files for HTTP serving */
#define HTTPD_MAX_OPEN_FILES 16

/* ──────────────────────────────────────────────────────────────────────────── */
/* Per-file state — tracks the VFS file handle alongside the fs_file           */
/* ──────────────────────────────────────────────────────────────────────────── */

struct httpd_vfs_state {
    struct vfs_file *vf;
    int in_use;
};

static struct httpd_vfs_state vfs_slots[HTTPD_MAX_OPEN_FILES];

static struct httpd_vfs_state *alloc_vfs_slot(void)
{
    for (int i = 0; i < HTTPD_MAX_OPEN_FILES; i++) {
        if (!vfs_slots[i].in_use) {
            vfs_slots[i].in_use = 1;
            return &vfs_slots[i];
        }
    }
    return NULL;
}

static void free_vfs_slot(struct httpd_vfs_state *s)
{
    if (s) {
        s->vf = NULL;
        s->in_use = 0;
    }
}

/* ──────────────────────────────────────────────────────────────────────────── */
/* Custom filesystem hooks for lwIP httpd                                      */
/*                                                                             */
/* These are called by fs.c when LWIP_HTTPD_CUSTOM_FILES is enabled.           */
/* Files are looked up relative to HTTPD_VFS_ROOT.                             */
/* ──────────────────────────────────────────────────────────────────────────── */

/*
 * fs_open_custom — Try to open a file from the xv6 VFS.
 *
 * Returns 1 if the file was found and opened, 0 to fall back to embedded
 * fsdata.  The file->pextension is used to store our VFS state.
 */
int fs_open_custom(struct fs_file *file, const char *name)
{
    char path[HTTPD_PATH_MAX];
    size_t root_len = strlen(HTTPD_VFS_ROOT);
    size_t name_len = strlen(name);

    if (root_len + name_len + 1 > sizeof(path))
        return 0;

    memmove(path, HTTPD_VFS_ROOT, root_len);
    memmove(path + root_len, name, name_len);
    path[root_len + name_len] = '\0';

    struct vfs_inode *inode = vfs_namei(path, strlen(path));
    if (IS_ERR_OR_NULL(inode))
        return 0; /* Not found — fall back to embedded fsdata */

    struct vfs_file *vf = vfs_fileopen(inode, O_RDONLY);
    vfs_iput(inode);
    if (IS_ERR(vf))
        return 0;

    struct httpd_vfs_state *state = alloc_vfs_slot();
    if (!state) {
        vfs_fput(vf);
        return 0;
    }
    state->vf = vf;

    /* Set up fs_file for dynamic reading */
    memset(file, 0, sizeof(*file));
    file->pextension = state;
    file->flags = FS_FILE_FLAGS_CUSTOM;
    /* len = 0 signals dynamic-length file; httpd will use fs_read_custom */
    file->len = 0;
    file->index = 0;
    file->data = NULL;

    return 1;
}

/*
 * fs_close_custom — Release VFS resources for a custom-opened file.
 */
void fs_close_custom(struct fs_file *file)
{
    struct httpd_vfs_state *state = (struct httpd_vfs_state *)file->pextension;
    if (state) {
        if (state->vf)
            vfs_fput(state->vf);
        free_vfs_slot(state);
        file->pextension = NULL;
    }
}

/*
 * fs_read_custom — Read data from a VFS-backed file.
 *
 * Called by httpd when LWIP_HTTPD_DYNAMIC_FILE_READ is enabled.
 * Returns number of bytes read, or FS_READ_EOF when done.
 */
int fs_read_custom(struct fs_file *file, char *buffer, int count)
{
    struct httpd_vfs_state *state = (struct httpd_vfs_state *)file->pextension;
    if (!state || !state->vf)
        return FS_READ_EOF;

    ssize_t n = vfs_fileread(state->vf, buffer, (size_t)count, false);
    if (n <= 0)
        return FS_READ_EOF;

    return (int)n;
}

/* ──────────────────────────────────────────────────────────────────────────── */
/* Module init                                                                 */
/* ──────────────────────────────────────────────────────────────────────────── */

void httpd_daemon_init(void)
{
    httpd_init();
    printf("httpd: server started on port 80 (root: %s)\n", HTTPD_VFS_ROOT);
}
