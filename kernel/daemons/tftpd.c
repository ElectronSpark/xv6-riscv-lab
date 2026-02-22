/*
 * tftpd — Kernel-resident TFTP server
 *
 * Uses lwIP's built-in TFTP application (RFC 1350, raw UDP API) with
 * xv6 VFS callbacks to serve and receive files from the root filesystem.
 *
 * The lwIP TFTP app runs entirely within the tcpip thread, so no
 * separate kthread is needed — we just call tftp_init_server() with
 * our context callbacks.
 *
 * Requested filenames are resolved relative to TFTPD_ROOT (default "/").
 * Both read (RRQ) and write (WRQ) operations are supported.
 *
 * QEMU network: guest port 69 (standard TFTP), reachable from host
 *   as localhost:6969 via QEMU SLIRP hostfwd.
 *
 * Usage from host:
 *   tftp localhost 6969
 *   tftp> get /etc/hostname
 *   tftp> put localfile /tmp/remotefile
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

#include "lwip/apps/tftp_server.h"

/* ──────────────────────────────────────────────────────────────────────────── */
/* Configuration                                                               */
/* ──────────────────────────────────────────────────────────────────────────── */

/* Root directory for TFTP file access.  All requested paths are resolved
 * relative to this directory.  Set to "/" to expose the entire filesystem. */
#define TFTPD_ROOT "/"

/* Maximum full path length (root prefix + filename from client) */
#define TFTPD_PATH_MAX 256

/* ──────────────────────────────────────────────────────────────────────────── */
/* VFS-backed TFTP context callbacks                                           */
/* ──────────────────────────────────────────────────────────────────────────── */

/*
 * Build a full path from the TFTP root and the client-supplied filename.
 * Returns 0 on success, -1 if the resulting path would overflow the buffer.
 */
static int build_path(char *dst, size_t dst_size, const char *fname)
{
    size_t root_len = strlen(TFTPD_ROOT);
    size_t fname_len = strlen(fname);

    /* Skip leading '/' in fname if root already ends with '/' */
    if (root_len > 0 && TFTPD_ROOT[root_len - 1] == '/' && fname[0] == '/')
        fname++, fname_len--;

    if (root_len + fname_len + 1 > dst_size)
        return -1;

    memmove(dst, TFTPD_ROOT, root_len);
    memmove(dst + root_len, fname, fname_len);
    dst[root_len + fname_len] = '\0';
    return 0;
}

/*
 * open — Called by lwIP TFTP when a client requests a file.
 *
 * For reads  (write == 0): opens an existing file.
 * For writes (write != 0): creates the file if it doesn't exist,
 *                          or truncates it if it does.
 *
 * Returns the vfs_file* as the opaque handle, or NULL on error.
 */
static void *tftpd_open(const char *fname, const char *mode, u8_t write)
{
    (void)mode; /* Always treat as octet/binary */

    char path[TFTPD_PATH_MAX];
    if (build_path(path, sizeof(path), fname) < 0) {
        printf("tftpd: path too long: %s\n", fname);
        return NULL;
    }

    struct vfs_inode *inode;
    struct vfs_file *f;

    if (!write) {
        /* ── Read request ── */
        inode = vfs_namei(path, strlen(path));
        if (IS_ERR_OR_NULL(inode)) {
            printf("tftpd: file not found: %s\n", path);
            return NULL;
        }
        f = vfs_fileopen(inode, O_RDONLY);
        vfs_iput(inode);
        if (IS_ERR(f)) {
            printf("tftpd: cannot open for read: %s\n", path);
            return NULL;
        }
        printf("tftpd: RRQ %s\n", path);
    } else {
        /* ── Write request ── */
        /* Try to open existing file first */
        inode = vfs_namei(path, strlen(path));
        if (!IS_ERR_OR_NULL(inode)) {
            f = vfs_fileopen(inode, O_WRONLY | O_TRUNC);
            vfs_iput(inode);
            if (IS_ERR(f)) {
                printf("tftpd: cannot open for write: %s\n", path);
                return NULL;
            }
        } else {
            /* File doesn't exist — create it.
             * Split path into parent directory + filename component. */
            char name[TFTPD_PATH_MAX];
            struct vfs_inode *dir = vfs_nameiparent(path, strlen(path),
                                                     name, sizeof(name));
            if (IS_ERR_OR_NULL(dir)) {
                printf("tftpd: parent dir not found: %s\n", path);
                return NULL;
            }
            inode = vfs_create(dir, S_IFREG | 0644, name, strlen(name));
            vfs_iput(dir);
            if (IS_ERR_OR_NULL(inode)) {
                printf("tftpd: cannot create: %s\n", path);
                return NULL;
            }
            f = vfs_fileopen(inode, O_WRONLY);
            vfs_iput(inode);
            if (IS_ERR(f)) {
                printf("tftpd: cannot open new file: %s\n", path);
                return NULL;
            }
        }
        printf("tftpd: WRQ %s\n", path);
    }

    return f;
}

/*
 * close — Release the VFS file handle.
 */
static void tftpd_close(void *handle)
{
    if (handle)
        vfs_fput((struct vfs_file *)handle);
}

/*
 * read — Read up to `bytes` from the file into buf.
 *
 * Returns the number of bytes actually read, or negative on error.
 * lwIP expects < 512 to signal end-of-file.
 */
static int tftpd_read(void *handle, void *buf, int bytes)
{
    struct vfs_file *f = (struct vfs_file *)handle;
    ssize_t n = vfs_fileread(f, buf, (size_t)bytes, false);
    return (int)n;
}

/*
 * write — Write data from a pbuf chain to the file.
 *
 * lwIP strips the TFTP header before calling this, so p->payload
 * points directly at file data.
 */
static int tftpd_write(void *handle, struct pbuf *p)
{
    struct vfs_file *f = (struct vfs_file *)handle;
    struct pbuf *q;
    int total = 0;

    /* Walk the pbuf chain (may be >1 pbuf for large payloads) */
    for (q = p; q != NULL; q = q->next) {
        ssize_t n = vfs_filewrite(f, q->payload, q->len, false);
        if (n < 0)
            return (int)n;
        total += (int)n;
    }

    return total;
}

/*
 * error — Log TFTP error messages.
 */
static void tftpd_error(void *handle, int err, const char *msg, int size)
{
    (void)handle;
    (void)size;
    printf("tftpd: error %d: %s\n", err, msg ? msg : "(null)");
}

/* ──────────────────────────────────────────────────────────────────────────── */
/* TFTP context (function pointers for lwIP)                                   */
/* ──────────────────────────────────────────────────────────────────────────── */

static const struct tftp_context tftpd_ctx = {
    .open  = tftpd_open,
    .close = tftpd_close,
    .read  = tftpd_read,
    .write = tftpd_write,
    .error = tftpd_error,
};

/* ──────────────────────────────────────────────────────────────────────────── */
/* Module init                                                                 */
/* ──────────────────────────────────────────────────────────────────────────── */

void tftpd_init(void)
{
    err_t err = tftp_init_server(&tftpd_ctx);
    if (err != ERR_OK) {
        printf("tftpd: failed to start (err=%d)\n", err);
        return;
    }
    printf("tftpd: server started on port 69 (root: %s)\n", TFTPD_ROOT);
}
