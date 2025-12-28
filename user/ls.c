#include "kernel/inc/types.h"
#include "kernel/inc/vfs/stat.h"
#include "user/user.h"
#include "kernel/inc/vfs/xv6fs/ondisk.h"
#include "kernel/inc/vfs/fcntl.h"

// Linux-compatible dirent structure for getdents
struct linux_dirent64 {
    uint64 d_ino;      // Inode number
    int64 d_off;       // Offset to next structure
    uint16 d_reclen;   // Size of this dirent
    uint8 d_type;      // File type
    char d_name[];     // Filename (null-terminated)
};

// Increase buffer size for longer filenames
#define NAME_MAX 255
#define FMT_WIDTH 14  // Display width for formatting

char*
fmtname(char *path)
{
  static char buf[FMT_WIDTH+1];
  char *p;

  // Find first character after last slash.
  for(p=path+strlen(path); p >= path && *p != '/'; p--)
    ;
  p++;

  // Return blank-padded name.
  int len = strlen(p);
  if(len >= FMT_WIDTH)
    return p;
  memmove(buf, p, len);
  memset(buf+len, ' ', FMT_WIDTH - len);
  buf[FMT_WIDTH] = 0;
  return buf;
}

void
ls(char *path)
{
  char buf[512], *p;
  int fd;
  struct stat st;
  char dirent_buf[1024];  // Buffer for getdents
  int nread;

  if((fd = open(path, O_RDONLY | O_NOFOLLOW)) < 0){
    fprintf(2, "ls: cannot open %s\n", path);
    return;
  }

  if(fstat(fd, &st) < 0){
    fprintf(2, "ls: cannot stat %s\n", path);
    close(fd);
    return;
  }

  if(S_ISREG(st.mode) || S_ISCHR(st.mode) || S_ISBLK(st.mode)) {
    printf("%s %o %ld %ld\n", fmtname(path), st.mode, st.ino, st.size);
  } else if(S_ISDIR(st.mode)) {
    if(strlen(path) + 1 + NAME_MAX + 1 > sizeof buf){
      printf("ls: path too long\n");
      close(fd);
      return;
    }
    strcpy(buf, path);
    p = buf+strlen(buf);
    *p++ = '/';
    
    // Use getdents to read directory entries
    while((nread = getdents(fd, dirent_buf, sizeof(dirent_buf))) > 0) {
      int pos = 0;
      while(pos < nread) {
        struct linux_dirent64 *de = (struct linux_dirent64 *)(dirent_buf + pos);
        if(de->d_ino == 0) {
          pos += de->d_reclen;
          continue;
        }
        strcpy(p, de->d_name);
        if(stat(buf, &st) < 0){
          printf("ls: cannot stat %s\n", buf);
          pos += de->d_reclen;
          continue;
        }
        printf("%s %o %ld %ld\n", fmtname(buf), st.mode, st.ino, st.size);
        pos += de->d_reclen;
      }
    }
  }
  close(fd);
}

int
main(int argc, char *argv[])
{
  int i;

  if(argc < 2){
    ls(".");
    exit(0);
  }
  for(i=1; i<argc; i++)
    ls(argv[i]);
  exit(0);
}
