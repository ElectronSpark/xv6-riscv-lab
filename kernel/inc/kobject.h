// Kernel Objects (kobjects) provide a common mechanism for reference
// counting and object lifetime management in the kernel.
#ifndef KERNEL_OBJECT_H
#define KERNEL_OBJECT_H

#include "types.h"
#include "list.h"
#include "spinlock.h"

struct kobject;
struct kobject_ops {
  // Called after the object is detached and its refcount reaches zero.
  // The implementation should free the memory associated with the object.
  // If NULL, the object memory will be freed using kmm_free().
  void (*release)(struct kobject *obj);
};

// There's no lock protecting kobject.
// Users of kobject must ensure proper synchronization.
struct kobject {
  list_node_t list_entry;
  int refcount;
  const char *name;
  struct kobject_ops ops;
};

void kobject_global_init(void);
// When initializing a kobject, its refcount field must be zero.
void kobject_init(struct kobject *obj);
void kobject_get(struct kobject *obj);
void kobject_put(struct kobject *obj);
int64 kobject_refcount(struct kobject *obj);
int64 kobject_count(void);

#endif // KERNEL_OBJECT_H
