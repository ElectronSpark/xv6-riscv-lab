// Kernel Objects (kobjects) provide a common mechanism for reference
// counting and object lifetime management in the kernel.
#ifndef KERNEL_OBJECT_H
#define KERNEL_OBJECT_H

#include "types.h"
#include "list.h"
#include "spinlock.h"

struct kobject;
struct kobject_ops {
  void (*release)(struct kobject *obj);
};

struct kobject {
  list_node_t list_entry;
  struct spinlock lock;
  int refcount;
  struct kobject_ops ops;
};

void kobject_init(struct kobject *obj);
void kobject_get(struct kobject *obj);
void kobject_put(struct kobject *obj);

#endif // KERNEL_OBJECT_H
