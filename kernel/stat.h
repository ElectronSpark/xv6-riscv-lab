#ifndef __KERNEL_STAT_H
#define __KERNEL_STAT_H

#define __XV6_STAT_T_DIR 1      // Directory
#define __XV6_STAT_T_FILE 2     // File
#define __XV6_STAT_T_DEVICE 3   // Device

#ifndef ON_HOST_OS

#define T_DIR     __XV6_STAT_T_DIR      // Directory
#define T_FILE    __XV6_STAT_T_FILE     // File
#define T_DEVICE  __XV6_STAT_T_DEVICE   // Device

struct stat {
  int dev;     // File system's disk device
  uint ino;    // Inode number
  short type;  // Type of file
  short nlink; // Number of links to file
  uint64 size; // Size of file in bytes
};

#else       /* ON_HOST_OS */

#define XV6_T_DIR     __XV6_STAT_T_DIR      // Directory
#define XV6_T_FILE    __XV6_STAT_T_FILE     // File
#define XV6_T_DEVICE  __XV6_STAT_T_DEVICE   // Device

#endif      /* ON_HOST_OS */

#endif      /* __KERNEL_STAT_H */
