#include "kernel/inc/types.h"
#include "kernel/inc/param.h"
#include "kernel/inc/errno.h"
#include "kernel/inc/vfs/fcntl.h"
#include "kernel/inc/vfs/stat.h"
#include "user/user.h"
#include "fsutil.h"

#define FSUTIL_DIRENT_BUFSZ 1024
#define FSUTIL_COPY_BUFSZ 4096
#define AT_FDCWD (-100)

struct linux_dirent64 {
    uint64 d_ino;
    int64 d_off;
    uint16 d_reclen;
    uint8 d_type;
    char d_name[];
};

static int fsutil_same_file(const struct stat *a, const struct stat *b) {
    return a->st_dev == b->st_dev && a->st_ino == b->st_ino;
}

static int fsutil_chmod_path(const char *path, int mode) {
    return fchmodat(AT_FDCWD, path, mode & ~S_IFMT, 0);
}

static int fsutil_copy_basename(char *dst, int dstsz, const char *path) {
    int len = strlen(path);

    while (len > 1 && path[len - 1] == '/') {
        len--;
    }

    int start = len;
    while (start > 0 && path[start - 1] != '/') {
        start--;
    }

    int base_len = len - start;
    if (base_len == 0) {
        if (dstsz < 2) {
            return -ENAMETOOLONG;
        }
        dst[0] = '/';
        dst[1] = '\0';
        return 0;
    }
    if (base_len + 1 > dstsz) {
        return -ENAMETOOLONG;
    }

    memmove(dst, path + start, base_len);
    dst[base_len] = '\0';
    return 0;
}

static int fsutil_is_dot_or_dotdot(const char *name) {
    return strcmp(name, ".") == 0 || strcmp(name, "..") == 0;
}

static int fsutil_join_path(char *dst, int dstsz, const char *dir,
                            const char *name) {
    int dir_len = strlen(dir);
    int name_len = strlen(name);

    while (dir_len > 1 && dir[dir_len - 1] == '/') {
        dir_len--;
    }

    int need_slash = !(dir_len == 1 && dir[0] == '/');
    int total = dir_len + (need_slash ? 1 : 0) + name_len;
    if (total + 1 > dstsz) {
        return -ENAMETOOLONG;
    }

    memmove(dst, dir, dir_len);
    int off = dir_len;
    if (need_slash) {
        dst[off++] = '/';
    }
    memmove(dst + off, name, name_len);
    dst[off + name_len] = '\0';
    return 0;
}

int fsutil_resolve_target(char *dstbuf, int bufsz, const char *src,
                          const char *dst, int dst_must_be_dir) {
    struct stat st;
    int ret = lstat(dst, &st);

    if (dst_must_be_dir) {
        if (ret < 0) {
            return -ENOTDIR;
        }
        if (!S_ISDIR(st.st_mode)) {
            return -ENOTDIR;
        }
    }

    if (ret >= 0 && S_ISDIR(st.st_mode)) {
        char base[MAXPATH];
        ret = fsutil_copy_basename(base, sizeof(base), src);
        if (ret < 0) {
            return ret;
        }
        return fsutil_join_path(dstbuf, bufsz, dst, base);
    }

    if ((int)strlen(dst) + 1 > bufsz) {
        return -ENAMETOOLONG;
    }
    strcpy(dstbuf, dst);
    return 0;
}

static int fsutil_copy_regular(const char *src, const char *dst,
                               const struct stat *src_st) {
    struct stat dst_st;
    int ret = lstat(dst, &dst_st);
    if (ret >= 0 && fsutil_same_file(src_st, &dst_st)) {
        return -EINVAL;
    }

    int sfd = open(src, O_RDONLY);
    if (sfd < 0) {
        return sfd;
    }

    int dfd = open(dst, O_WRONLY | O_CREAT | O_TRUNC);
    if (dfd < 0) {
        close(sfd);
        return dfd;
    }

    char buf[FSUTIL_COPY_BUFSZ];
    ret = 0;
    for (;;) {
        int n = read(sfd, buf, sizeof(buf));
        if (n < 0) {
            ret = n;
            break;
        }
        if (n == 0) {
            break;
        }

        int off = 0;
        while (off < n) {
            int w = write(dfd, buf + off, n - off);
            if (w < 0) {
                ret = w;
                goto out;
            }
            if (w == 0) {
                ret = -EIO;
                goto out;
            }
            off += w;
        }
    }

out:
    close(dfd);
    close(sfd);
    if (ret == 0) {
        int chmod_ret = fsutil_chmod_path(dst, src_st->st_mode);
        if (chmod_ret < 0) {
            return chmod_ret;
        }
    }
    return ret;
}

static int fsutil_copy_symlink(const char *src, const char *dst) {
    char target[MAXPATH];
    int ret = readlink(src, target, sizeof(target) - 1);
    if (ret < 0) {
        return ret;
    }
    if (ret >= (int)sizeof(target) - 1) {
        return -ENAMETOOLONG;
    }
    target[ret] = '\0';

    struct stat dst_st;
    ret = lstat(dst, &dst_st);
    if (ret >= 0) {
        if (S_ISDIR(dst_st.st_mode)) {
            return -EISDIR;
        }
        ret = unlink(dst);
        if (ret < 0) {
            return ret;
        }
    } else if (ret != -ENOENT) {
        return ret;
    }

    return symlink(target, dst);
}

static int fsutil_copy_directory(const char *src, const char *dst,
                                 const struct stat *src_st, int recursive) {
    if (!recursive) {
        return -EISDIR;
    }

    struct stat dst_st;
    int ret = lstat(dst, &dst_st);
    if (ret >= 0) {
        if (!S_ISDIR(dst_st.st_mode)) {
            return -ENOTDIR;
        }
        if (fsutil_same_file(src_st, &dst_st)) {
            return -EINVAL;
        }
    } else if (ret == -ENOENT) {
        ret = mkdir(dst);
        if (ret < 0) {
            return ret;
        }
    } else {
        return ret;
    }

    int fd = open(src, O_RDONLY);
    if (fd < 0) {
        return fd;
    }

    char dbuf[FSUTIL_DIRENT_BUFSZ];
    ret = 0;

    for (;;) {
        int nread = getdents(fd, dbuf, sizeof(dbuf));
        if (nread < 0) {
            ret = nread;
            break;
        }
        if (nread == 0) {
            break;
        }

        int bpos = 0;
        while (bpos < nread) {
            struct linux_dirent64 *de = (struct linux_dirent64 *)(dbuf + bpos);
            if (de->d_reclen == 0) {
                ret = -EIO;
                goto out;
            }
            if (de->d_ino != 0 && !fsutil_is_dot_or_dotdot(de->d_name)) {
                char src_child[MAXPATH];
                char dst_child[MAXPATH];

                ret = fsutil_join_path(src_child, sizeof(src_child), src,
                                       de->d_name);
                if (ret < 0) {
                    goto out;
                }
                ret = fsutil_join_path(dst_child, sizeof(dst_child), dst,
                                       de->d_name);
                if (ret < 0) {
                    goto out;
                }

                ret = fsutil_copy_path(src_child, dst_child, recursive);
                if (ret < 0) {
                    goto out;
                }
            }
            bpos += de->d_reclen;
        }
    }

out:
    close(fd);
    return ret;
}

int fsutil_copy_path(const char *src, const char *dst, int recursive) {
    struct stat src_st;
    int ret = lstat(src, &src_st);
    if (ret < 0) {
        return ret;
    }

    if (S_ISREG(src_st.st_mode)) {
        return fsutil_copy_regular(src, dst, &src_st);
    }
    if (S_ISLNK(src_st.st_mode)) {
        ret = fsutil_copy_symlink(src, dst);
        if (ret == 0) {
            return 0;
        }

        struct stat follow_st;
        if (stat(src, &follow_st) < 0) {
            return ret;
        }
        if (S_ISREG(follow_st.st_mode)) {
            return fsutil_copy_regular(src, dst, &follow_st);
        }
        if (S_ISDIR(follow_st.st_mode)) {
            return fsutil_copy_directory(src, dst, &follow_st, recursive);
        }
        return ret;
    }
    if (S_ISDIR(src_st.st_mode)) {
        ret = fsutil_copy_directory(src, dst, &src_st, recursive);
        if (ret == 0) {
            int chmod_ret = fsutil_chmod_path(dst, src_st.st_mode);
            if (chmod_ret < 0) {
                return chmod_ret;
            }
        }
        return ret;
    }

    return -EOPNOTSUPP;
}

int fsutil_remove_path(const char *path, int recursive, int force) {
    struct stat st;
    int ret = lstat(path, &st);
    if (ret < 0) {
        return (force && ret == -ENOENT) ? 0 : ret;
    }

    char base[MAXPATH];
    ret = fsutil_copy_basename(base, sizeof(base), path);
    if (ret < 0) {
        return ret;
    }

    if (fsutil_is_dot_or_dotdot(base)) {
        return -EINVAL;
    }

    if (!S_ISDIR(st.st_mode)) {
        ret = unlink(path);
        return (force && ret == -ENOENT) ? 0 : ret;
    }

    if (!recursive) {
        return -EISDIR;
    }

    for (;;) {
        int fd = open(path, O_RDONLY);
        if (fd < 0) {
            return fd;
        }

        char dbuf[FSUTIL_DIRENT_BUFSZ];
        char child[MAXPATH];
        int found_child = 0;
        ret = 0;

        for (;;) {
            int nread = getdents(fd, dbuf, sizeof(dbuf));
            if (nread < 0) {
                ret = nread;
                break;
            }
            if (nread == 0) {
                break;
            }

            int bpos = 0;
            while (bpos < nread) {
                struct linux_dirent64 *de = (struct linux_dirent64 *)(dbuf + bpos);
                if (de->d_reclen == 0) {
                    ret = -EIO;
                    break;
                }
                if (!fsutil_is_dot_or_dotdot(de->d_name)) {
                    ret = fsutil_join_path(child, sizeof(child), path, de->d_name);
                    if (ret < 0) {
                        break;
                    }
                    found_child = 1;
                    break;
                }
                bpos += de->d_reclen;
            }

            if (ret < 0 || found_child) {
                break;
            }
        }

        close(fd);
        if (ret < 0) {
            return ret;
        }
        if (!found_child) {
            break;
        }

        ret = fsutil_remove_path(child, 1, force);
        if (ret < 0) {
            return ret;
        }
    }

    ret = unlink(path);
    return (force && ret == -ENOENT) ? 0 : ret;
}

int fsutil_move_path(const char *src, const char *dst) {
    int ret = rename(src, dst);
    if (ret == 0 || ret != -EXDEV) {
        return ret;
    }

    struct stat st;
    ret = lstat(src, &st);
    if (ret < 0) {
        return ret;
    }

    ret = fsutil_copy_path(src, dst, 1);
    if (ret < 0) {
        return ret;
    }

    if (S_ISDIR(st.st_mode)) {
        return fsutil_remove_path(src, 1, 0);
    }
    return unlink(src);
}