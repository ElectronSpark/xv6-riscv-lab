#ifndef __KERNEL_STAT_H
#define __KERNEL_STAT_H

#define T_DIR     1   // Directory
#define T_FILE    2   // File
#define T_DEVICE  3   // Device

struct stat {
  int dev;     // File system's disk device
  uint ino;    // Inode number
  short type;  // Type of file
  short nlink; // Number of links to file
  uint64 size; // Size of file in bytes
};

#endif      /* __KERNEL_STAT_H */
