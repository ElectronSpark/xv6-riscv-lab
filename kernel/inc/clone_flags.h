/**
 * @file clone_flags.h
 * @brief Clone flags for thread creation
 *
 * Defines flags used with clone() system call to control process/thread
 * creation. Most flags are inspired by Linux but with different values. Most
 * are currently unused - defined for future compatibility.
 *
 * @see https://man7.org/linux/man-pages/man2/clone.2.html
 */

#ifndef __KERNEL_CLONE_FLAGS_H
#define __KERNEL_CLONE_FLAGS_H

#include "uabi/clone_flags.h"

#endif // __KERNEL_CLONE_FLAGS_H
