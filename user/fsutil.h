#ifndef XV6_USER_FSUTIL_H
#define XV6_USER_FSUTIL_H

int fsutil_copy_path(const char *src, const char *dst, int recursive);
int fsutil_move_path(const char *src, const char *dst);
int fsutil_remove_path(const char *path, int recursive, int force);
int fsutil_resolve_target(char *dstbuf, int bufsz, const char *src,
                          const char *dst, int dst_must_be_dir);

#endif