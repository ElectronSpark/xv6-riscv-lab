#include "types.h"
#include "errno.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "defs.h"
#include "printf.h"
#include "slab.h"
#include "list.h"
#include "kobject.h"

static list_node_t __kobject_list = {0};
static int64 __kobject_count = 0;
static struct spinlock kobject_lock = {0};

static void __kobject_attach(struct kobject *obj) {
  spin_acquire(&kobject_lock);
  list_node_push_back(&__kobject_list, obj, list_entry);
  int64 count = __atomic_add_fetch(&__kobject_count, 1, __ATOMIC_SEQ_CST);
  assert(count > 0, "kobject count underflow");
  spin_release(&kobject_lock);
}

static void __kobject_detach(struct kobject *obj) {
  spin_acquire(&kobject_lock);
  list_node_detach(obj, list_entry);
  int64 count = __atomic_sub_fetch(&__kobject_count, 1, __ATOMIC_SEQ_CST);
  assert(count >= 0, "kobject count underflow");
  spin_release(&kobject_lock);
}

void kobject_global_init(void) {
  list_entry_init(&__kobject_list);
  spin_init(&kobject_lock, "kobject_lock");
}

void kobject_init(struct kobject *obj) {
  assert(obj != NULL, "kobject_init: obj is NULL");
  list_entry_init(&obj->list_entry);
  int64 expected = __atomic_fetch_add(&obj->refcount, 1, __ATOMIC_SEQ_CST);
  assert(expected == 0, "kobject_init: obj->refcount is not zero");
  __kobject_attach(obj);
}

void kobject_get(struct kobject *obj) {
  assert(obj != NULL, "kobject_get: obj is NULL");
  int64 count = __atomic_add_fetch(&obj->refcount, 1, __ATOMIC_SEQ_CST);
  assert(count > 0, "kobject_get: refcount underflow");
}

void kobject_put(struct kobject *obj) {
  assert(obj != NULL, "kobject_put: obj is NULL");
  int64 count = __atomic_sub_fetch(&obj->refcount, 1, __ATOMIC_SEQ_CST);
  assert(count >= 0, "kobject_put: refcount underflow");
  if (count == 0) {
    __kobject_detach(obj);
    if (!obj->ops.release) {
      kmm_free(obj);
    } else {
      obj->ops.release(obj);
    }
  }
}

int64 kobject_refcount(struct kobject *obj) {
  assert(obj != NULL, "kobject_refcount: obj is NULL");
  return __atomic_load_n(&obj->refcount, __ATOMIC_SEQ_CST);
}

int64 kobject_count(void) {
  return __atomic_load_n(&__kobject_count, __ATOMIC_SEQ_CST);
}
