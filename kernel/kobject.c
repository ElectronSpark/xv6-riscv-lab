#include "types.h"
#include "errno.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "defs.h"
#include "slab.h"
#include "list.h"
#include "kobject.h"

static list_node_t __kobject_list = {0};
static int64 __kobject_count = 0;
static struct spinlock kobject_lock = {0};

static void __kobject_attach(struct kobject *obj) {
  spin_acquire(&kobject_lock);
  list_node_push_back(&__kobject_list, obj, list_entry);
  __kobject_count++;
  assert(__kobject_count > 0, "kobject count underflow");
  spin_release(&kobject_lock);
}

static void __kobject_detach(struct kobject *obj) {
  spin_acquire(&kobject_lock);
  list_node_detach(obj, list_entry);
  __kobject_count--;
  assert(__kobject_count >= 0, "kobject count underflow");
  spin_release(&kobject_lock);
}

void kobject_global_init(void) {
  list_entry_init(&__kobject_list);
  spin_init(&kobject_lock, "kobject_lock");
}

void kobject_init(struct kobject *obj) {
  assert(obj != NULL, "kobject_init: obj is NULL");
  list_entry_init(&obj->list_entry);
  obj->refcount = 1; // initial refcount
  obj->ops.release = NULL;
  __kobject_attach(obj);
}

void kobject_get(struct kobject *obj) {
  assert(obj != NULL, "kobject_get: obj is NULL");
  obj->refcount++;
  assert(obj->refcount > 0, "kobject_get: refcount underflow");
}

void kobject_put(struct kobject *obj) {
  assert(obj != NULL, "kobject_put: obj is NULL");
  obj->refcount--;
  assert(obj->refcount >= 0, "kobject_put: refcount underflow");
  if (obj->refcount == 0) {
    __kobject_detach(obj);
    if (!obj->ops.release) {
      kmm_free(obj);
    } else {
      obj->ops.release(obj);
    }
  }
}

int64 kobject_count(void) {
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  return __kobject_count;
}
