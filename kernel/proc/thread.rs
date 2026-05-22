//! Pure-Rust port of `kernel/proc/thread.c`.
//!
//! Owns the canonical public ABI symbols: tcb_lock/unlock,
//! proc_assert_holding, thread_init, attach_child, detach_child,
//! thread_create, kthread_create, idle_thread_init, thread_destroy,
//! userinit, install_user_root, and the `xv6_thport_*` aliases used by
//! sibling Rust/C ports.

#![allow(non_camel_case_types, non_upper_case_globals, non_snake_case, dead_code)]


macro_rules! u {
    ($($tokens:tt)*) => {
        unsafe { $($tokens)* }
    };
}

use core::ffi::{c_char, c_int, c_void};
use core::mem::offset_of;
use core::ptr;
use core::sync::atomic::{AtomicPtr, Ordering};

use crate::bindings::{
    context, fs_struct, list_node_t, hlist_entry_t, page_t, rcu_head_t,
    rq, sched_attr, sched_entity, sigacts as sigacts_t, spinlock_t, thread,
    thread_group, thread_state, utrapframe, vfs_fdtable, vm,
};
use crate::lock::rcu::rcu_check_callbacks;
use crate::machine::{cpu_local_ptr, cpuid, intr_on, read_sp};
use crate::proc::access::{
    err_ptr, is_err, is_err_or_null, list_node_init_raw, CpuLocalRef, SchedEntityRef,
    SpinLockRef, ThreadAccess,
};
use crate::proc::__proctab_init;
use crate::proc::pid_assert_wholding;
use crate::proc::sigacts_init;
use crate::proc::xv6_rqport_rq_lock_current;
use crate::proc::xv6_rqport_rq_unlock_current;
use crate::proc::xv6_rqport_sched_attr_init;
use crate::proc::xv6_rqport_sched_entity_init;
use crate::proc::xv6_rqport_sched_setattr;
use crate::proc::xv6_sigport_sigacts_put;
use crate::proc::xv6_sigport_sigpending_destroy;
use crate::proc::xv6_sigport_sigpending_init;

// vm_t typedef
type vm_t = vm;

// ---------------- constants ----------------------------------------------
const PAGE_SHIFT: u32 = 12;
const PAGE_SIZE: u64 = 1 << PAGE_SHIFT;
const PAGE_MASK: u64 = PAGE_SIZE - 1;
const PAGE_BUDDY_MAX_ORDER: c_int = 10;
const CACHELINE_SIZE: u64 = 64;
const CACHELINE_MASK: u64 = CACHELINE_SIZE - 1;
const KERNEL_STACK_ORDER: c_int = 2;
const KERNEL_STACK_SIZE: u64 = 1u64 << (PAGE_SHIFT + KERNEL_STACK_ORDER as u32);

const IDLE_MAJOR_PRIORITY: c_int = 63;
// MAKE_PRIORITY(63, default_minor) — default minor is 0 in xv6, so IDLE_PRIORITY = 63<<2 = 252.
// But we just pass IDLE_MAJOR_PRIORITY into attr.priority via the rqport helper.
// Looking at the C: attr.priority = IDLE_PRIORITY. We must compute the same value.
// MAKE_PRIORITY(major, minor) = (major << 2) | (minor & 0x3); DEFAULT_MINOR_PRIORITY = 0.
const DEFAULT_MINOR_PRIORITY: c_int = 0;
const IDLE_PRIORITY: c_int = (IDLE_MAJOR_PRIORITY << 2) | (DEFAULT_MINOR_PRIORITY & 0x3);

const PAGE_TYPE_ANON: u64 = 1; // matches mm/page_type.h enum value

const KSTACK_ARRANGE_FLAGS_TF: u64 = 0x1;
const KSTACK_ARRANGE_FLAGS_ALL: u64 = KSTACK_ARRANGE_FLAGS_TF;

const KERNEL_HIERARCHY_ID: c_int = 0;

const EINVAL: c_int = 22;
const ENOMEM: c_int = 12;
const EAGAIN: c_int = 11;

// Thread states (matches enum thread_state in thread_types.h)
const THREAD_UNUSED: thread_state = 0;
const THREAD_USED: thread_state = 1;
const THREAD_INTERRUPTIBLE: thread_state = 2;
const THREAD_KIILABLE: thread_state = 3;
const THREAD_TIMER: thread_state = 4;
const THREAD_KIILABLE_TIMER: thread_state = 5;
const THREAD_UNINTERRUPTIBLE: thread_state = 6;
const THREAD_WAKENING: thread_state = 7;
const THREAD_RUNNING: thread_state = 8;
const THREAD_STOPPED: thread_state = 9;
const THREAD_EXITING: thread_state = 10;
const THREAD_ZOMBIE: thread_state = 11;

const THREAD_FLAG_USER_SPACE: u64 = 5;

// ---------------- extern C primitives -----------------------------------
unsafe extern "C" {
    fn memset(s: *mut c_void, c: c_int, n: usize) -> *mut c_void;
    fn strncpy(d: *mut c_char, s: *const c_char, n: usize) -> *mut c_char;
    fn safestrcpy(d: *mut c_char, s: *const c_char, n: usize) -> *mut c_char;

    fn printf(fmt: *const c_char, ...) -> c_int;
    pub safe fn xv6_panic(msg: *const c_char) -> !;

    // pid hierarchy
    fn pid_wlock();
    fn pid_wunlock();
    fn proctab_proc_add(p: *mut thread);
    fn __alloc_pid() -> c_int;
    fn __free_pid();
    fn __proctab_get_initproc() -> *mut thread;
    fn __proctab_set_initproc(p: *mut thread);

    // group/session
    safe fn thread_group_alloc_kernel(out: *mut *mut thread_group, hier_id: c_int) -> c_int;
    fn thread_group_init(p: *mut thread);
    safe fn thread_group_add(tg: *mut thread_group, p: *mut thread);
    fn thread_group_put(tg: *mut thread_group);
    safe fn pgroup_alloc(hier_id: c_int, parent: *mut c_void) -> *mut c_void;
    fn pgroup_init(p: *mut thread);
    safe fn pgroup_add_tg(pg: *mut c_void, tg: *mut thread_group) -> c_int;
    safe fn pgroup_add_thread(pg: *mut c_void, p: *mut thread) -> c_int;
    safe fn session_alloc(hier_id: c_int) -> *mut c_void;
    fn session_init(p: *mut thread);
    safe fn session_add_pg(s: *mut c_void, pg: *mut c_void) -> c_int;
    safe fn session_add_thread(s: *mut c_void, p: *mut thread) -> c_int;

    // signal subsystem
    fn xv6_sigport_sigpending_empty(p: *mut thread, locked: c_int);
    fn xv6_sigport_sigstack_init(stk: *mut c_void);

    // scheduler / rq
    fn xv6_schport_context_switch_finish(prev: *mut thread, next: *mut thread, flags: c_int);
    fn xv6_schport_scheduler_wakeup(p: *mut thread);
    fn xv6_rqport_rq_cpu_activate(cpuid: c_int);
    fn xv6_rqport_rq_enqueue_task(rq: *mut rq, se: *mut sched_entity);
    fn get_rq_for_cpu(cls_id: c_int, cpuid: c_int) -> *mut rq;

    // memory
    fn page_alloc(order: u64, flags: u64) -> *mut c_void;
    safe fn page_free(ptr: *mut c_void, order: c_int);
    fn vm_init() -> *mut vm_t;
    fn vm_put(vm: *mut vm_t);

    // VFS
    fn vfs_struct_clone(old_fs: *mut fs_struct, flags: u64) -> *mut fs_struct;
    fn vfs_struct_put(fs: *mut fs_struct);
    fn vfs_struct_lock(fs: *mut fs_struct);
    fn vfs_struct_unlock(fs: *mut fs_struct);
    fn vfs_namei(path: *const c_char, path_len: usize) -> *mut c_void; // vfs_inode opaque
    fn vfs_inode_get_ref(inode: *mut c_void, out: *mut c_void) -> c_int;
    fn vfs_iput(inode: *mut c_void);
    fn vfs_fdtable_put(fdt: *mut vfs_fdtable);

    // exec / trap
    fn exec(path: *mut c_char, argv: *mut *mut c_char, envp: *mut *mut c_char) -> c_int;
    fn usertrapret();
    fn start_kernel_post_init();
    fn exit(code: c_int) -> !;

    // CPU / interrupts (only the cpuid trampoline exists as a real symbol)

    // spinlock
    fn spin_init(lock: *mut spinlock_t, name: *mut c_char);
    fn spin_lock(lock: *mut spinlock_t);
    fn spin_unlock(lock: *mut spinlock_t);
    fn spin_holding(lock: *mut spinlock_t) -> c_int;

    // RCU
    fn call_rcu(head: *mut rcu_head_t, func: Option<unsafe extern "C" fn(*mut c_void)>, data: *mut c_void);
}

// ---------------- inline helpers ----------------------------------------
#[inline]
fn hlist_entry_init_rs(e: *mut hlist_entry_t) { u! {
    // hlist_entry_init zeroes the inner list_entry; struct hlist_entry has
    // a list_node_t list_entry plus other fields. The C inline does
    // list_entry_init(&entry->list_entry). list_entry is at offset 0.
    list_node_init_raw(e as *mut list_node_t);
}}

#[inline]
fn ta_of<'a>(p: *mut thread) -> Option<ThreadAccess<'a>> {
    ThreadAccess::from_ptr(p)
}

#[inline]
fn current_thread() -> *mut thread {
    CpuLocalRef::assume(cpu_local_ptr()).proc_ptr()
}

#[inline]
fn thread_state_get(p: *mut thread) -> thread_state {
    match ta_of(p) {
        Some(t) => t.state_load() as thread_state,
        None => THREAD_UNUSED,
    }
}
#[inline]
fn thread_state_set(p: *mut thread, s: thread_state) {
    if let Some(t) = ta_of(p) { t.state_store(s as u32); }
}
#[inline]
fn thread_is_awoken(p: *mut thread) -> bool {
    let s = thread_state_get(p);
    s == THREAD_RUNNING || s == THREAD_WAKENING
}
#[inline]
fn thread_is_sleeping(p: *mut thread) -> bool {
    let s = thread_state_get(p);
    s == THREAD_INTERRUPTIBLE || s == THREAD_UNINTERRUPTIBLE
        || s == THREAD_KIILABLE || s == THREAD_TIMER || s == THREAD_KIILABLE_TIMER
}
#[inline]
fn thread_set_user_space(p: *mut thread) {
    if let Some(t) = ta_of(p) {
        t.flags_set_bit(THREAD_FLAG_USER_SPACE);
        core::sync::atomic::fence(Ordering::SeqCst);
    }
}

#[inline]
fn thread_from_context(ctx: *mut context)-> *mut thread  { u! {
    let off = offset_of!(sched_entity, context);
    let se = (ctx as *mut u8).wrapping_sub(off) as *mut sched_entity;
    SchedEntityRef::assume(se).thread_ptr()
}}

// ---------------- panic macro -------------------------------------------
macro_rules! kpanic {
    ($msg:expr) => {{ xv6_panic(concat!($msg, "\0").as_ptr() as *const c_char) }};
}
macro_rules! kassert {
    ($cond:expr, $msg:expr) => {{ if !($cond) { kpanic!($msg); } }};
}
macro_rules! thport_unsafe_call { ($e:expr) => {{ u! { $e } }}; }

macro_rules! thread_raw_layout {
    ($($body:tt)*) => {{
        #[allow(unused_unsafe)]
        u! { $($body)* }
    }};
}

// ---------------- kernel hierarchy statics ------------------------------
static KERNEL_TG: AtomicPtr<thread_group> = AtomicPtr::new(ptr::null_mut());
static KERNEL_PG: AtomicPtr<c_void> = AtomicPtr::new(ptr::null_mut());
static KERNEL_SESSION: AtomicPtr<c_void> = AtomicPtr::new(ptr::null_mut());

fn kthread_hierarchy_init_locked() { u! {
    pid_assert_wholding();
    if !KERNEL_TG.load(Ordering::Acquire).is_null() {
        return;
    }

    let mut tg: *mut thread_group = ptr::null_mut();
    let ret = thread_group_alloc_kernel(&mut tg as *mut _, KERNEL_HIERARCHY_ID);
    kassert!(ret == 0 && !tg.is_null(), "kthread hierarchy: thread_group_alloc_kernel failed");

    let pg = pgroup_alloc(KERNEL_HIERARCHY_ID, ptr::null_mut());
    kassert!(!pg.is_null(), "kthread hierarchy: pgroup_alloc failed");
    // pgroup_t->is_kernel = 1: we don't have the struct layout, so use a setter via extern.
    // Fallback: rely on C helper. Add extern:
    pgroup_mark_kernel(pg);

    let s = session_alloc(KERNEL_HIERARCHY_ID);
    kassert!(!s.is_null(), "kthread hierarchy: session_alloc failed");
    session_mark_kernel(s);

    let ret = session_add_pg(s, pg);
    kassert!(ret == 0, "kthread hierarchy: session_add_pg failed");
    let ret = pgroup_add_tg(pg, tg);
    kassert!(ret == 0, "kthread hierarchy: pgroup_add_tg failed");

    KERNEL_TG.store(tg, Ordering::Release);
    KERNEL_PG.store(pg, Ordering::Release);
    KERNEL_SESSION.store(s, Ordering::Release);
}}

unsafe extern "C" {
    safe fn pgroup_mark_kernel(pg: *mut c_void);
    safe fn session_mark_kernel(s: *mut c_void);
}

fn kthread_join_hierarchy_locked(p: *mut thread) { u! {
    pid_assert_wholding();
    kassert!(!p.is_null(), "kthread hierarchy: thread is NULL");
    kthread_hierarchy_init_locked();

    thread_group_add(KERNEL_TG.load(Ordering::Acquire), p);
    let ret = pgroup_add_thread(KERNEL_PG.load(Ordering::Acquire), p);
    kassert!(ret == 0, "kthread hierarchy: pgroup_add_thread failed");
    let ret = session_add_thread(KERNEL_SESSION.load(Ordering::Acquire), p);
    kassert!(ret == 0, "kthread hierarchy: session_add_thread failed");
}}

// ---------------- pcb init / kstack arrange -----------------------------
fn pcb_init(p: *mut thread, fdtable: *mut vfs_fdtable) { u! {
    thread_raw_layout! {
        let ta = ThreadAccess::assume(p);
        thread_state_set(p, THREAD_UNUSED);
        xv6_sigport_sigpending_init(p);
        // sig_stack lives at &p->signal.sig_stack; we can't access nested
        // fields easily, but the C used `&p->signal.sig_stack`. The bindgen
        // `signal` field is opaque thread_signal_t. We rely on sigport to
        // know its layout via this helper:
        sigstack_init_for_thread(p);
        ta.init_embedded_lists_and_lock(c"thread".as_ptr() as *mut c_char);
        ta.set_thread_group(ptr::null_mut());
        ta.set_fs(ptr::null_mut());
        ta.set_fdtable(fdtable);
        if !ta.sched_entity_ptr().is_null() {
            ta.zero_sched_entity_storage();
            xv6_rqport_sched_entity_init(ta.sched_entity_ptr(), p);
        }
    }
}}

unsafe extern "C" {
    fn sigstack_init_for_thread(p: *mut thread);
}

fn kstack_arrange(kstack: *mut c_void, kstack_size: u64, flags: u64)-> *mut thread  { u! {
    thread_raw_layout! {
        let p = (kstack as *mut u8).add((kstack_size as usize) - core::mem::size_of::<thread>()) as *mut thread;
        let mut next_addr = p as u64;

        let mut trapframe: *mut utrapframe = ptr::null_mut();
        let fdtable: *mut vfs_fdtable = ptr::null_mut();

        if (flags & KSTACK_ARRANGE_FLAGS_TF) != 0 {
            next_addr = (p as u64) - (core::mem::size_of::<utrapframe>() as u64) - 16;
            next_addr &= !0x7u64;
            trapframe = next_addr as *mut utrapframe;
        }

        next_addr = next_addr - (core::mem::size_of::<sched_entity>() as u64);
        next_addr &= !CACHELINE_MASK;
        let ta = ThreadAccess::assume(p);
        ta.set_sched_entity_ptr(next_addr as *mut sched_entity);

        pcb_init(p, fdtable);

        ta.set_trapframe(trapframe);

        let mut ksp = next_addr - 16;
        ksp &= !0x7u64;
        ta.set_ksp(ksp);

        p
    }
}}

// ---------------- public canonical ABI ----------------------------------
fn thread_lock_ref<'a>(p: *mut thread) -> Option<SpinLockRef<'a>> {
    ta_of(p).map(|t| t.lock_ref())
}

fn tcb_lock_impl(p: *mut thread) {
    kassert!(!p.is_null(), "tcb_lock: thread is NULL");
    thread_lock_ref(p).unwrap().lock();
}
fn tcb_unlock_impl(p: *mut thread) {
    kassert!(!p.is_null(), "tcb_unlock: thread is NULL");
    thread_lock_ref(p).unwrap().unlock();
}
fn proc_assert_holding_impl(p: *mut thread) {
    kassert!(!p.is_null(), "proc_assert_holding: thread is NULL");
    kassert!(thread_lock_ref(p).unwrap().holding(), "proc_assert_holding: thread lock not held");
}

#[no_mangle]
pub extern "C" fn tcb_lock(p: *mut thread) { tcb_lock_impl(p) }
#[no_mangle]
pub extern "C" fn tcb_unlock(p: *mut thread) { tcb_unlock_impl(p) }
#[no_mangle]
pub extern "C" fn proc_assert_holding(p: *mut thread) { proc_assert_holding_impl(p) }

#[no_mangle]
pub extern "C" fn thread_init() { __proctab_init(); }

#[no_mangle]
pub extern "C" fn attach_child(parent: *mut thread, child: *mut thread) { u! {
    thread_raw_layout! {
        kassert!(!parent.is_null(), "attach_child: parent is NULL");
        kassert!(!child.is_null(), "attach_child: child is NULL");
        let pta = ThreadAccess::assume(parent);
        let cta = ThreadAccess::assume(child);
        kassert!(child != __proctab_get_initproc(), "attach_child: child is init process");
        pid_assert_wholding();
        kassert!(cta.siblings_ref().is_detached(), "attach_child: child is attached to a parent");
        kassert!(cta.parent_ptr().is_null(), "attach_child: child has a parent");

        cta.set_parent(parent);
        cta.siblings_ref().push_back(pta.children_ptr());
        pta.inc_children_count();
    }
}}

#[no_mangle]
pub extern "C" fn detach_child(parent: *mut thread, child: *mut thread) { u! {
    thread_raw_layout! {
        kassert!(!parent.is_null(), "detach_child: parent is NULL");
        kassert!(!child.is_null(), "detach_child: child is NULL");
        let pta = ThreadAccess::assume(parent);
        let cta = ThreadAccess::assume(child);
        pid_assert_wholding();
        kassert!(pta.children_count() > 0, "detach_child: parent has no children");
        kassert!(!cta.siblings_ref().is_empty(), "detach_child: child is not a sibling of parent");
        kassert!(!cta.siblings_ref().is_detached(), "detach_child: child is already detached");
        kassert!(cta.parent_ptr() == parent, "detach_child: child is not a child of parent");

        cta.siblings_ref().detach();
        pta.dec_children_count();
        cta.set_parent(ptr::null_mut());

        kassert!(pta.children_count() > 0 || pta.children_ref().is_empty(),
                 "detach_child: parent has no children after detaching child");
    }
}}

#[no_mangle]
pub extern "C" fn thread_create(entry: *mut c_void, arg1: u64, arg2: u64, kstack_order: c_int)-> *mut thread  { u! {
    if kstack_order < 0 || kstack_order > PAGE_BUDDY_MAX_ORDER {
        return err_ptr(-EINVAL);
    }
    let kstack_size = 1u64 << (PAGE_SHIFT + kstack_order as u32);
    thread_raw_layout! {
        let kstack = page_alloc(kstack_order as u64, PAGE_TYPE_ANON);
        if kstack.is_null() {
            return err_ptr(-ENOMEM);
        }
        memset((kstack as *mut u8).add((kstack_size - PAGE_SIZE) as usize) as *mut c_void,
               0, PAGE_SIZE as usize);

        let p = kstack_arrange(kstack, kstack_size, KSTACK_ARRANGE_FLAGS_ALL);

         let ta = ThreadAccess::assume(p);
         ta.set_kstack_order(kstack_order);
         ta.set_kstack_addr(kstack as u64);
         let se = ta.sched_entity_ptr();
         let se_ref = SchedEntityRef::assume(se);
         se_ref.zero_context();
         se_ref.init_context(entry as u64, ta.ksp());
         ta.set_kentry(entry as u64);
         ta.set_arg(0, arg1);
         ta.set_arg(1, arg2);
        ta.set_pid(-1);
        ta.set_tgid(-1);
        ta.set_pgid(-1);
        ta.set_sid(-1);

        xv6_rqport_sched_entity_init(se, p);
        p
    }
}}

extern "C" fn kthread_entry(prev: *mut context) { u! {
    thread_raw_layout! {
        kassert!(!prev.is_null(), "kthread_entry: prev context is NULL");
        let cur = current_thread();
        xv6_schport_context_switch_finish(thread_from_context(prev), cur, 0);
        CpuLocalRef::assume(cpu_local_ptr()).set_noff(0);
        intr_on();
        u! { rcu_check_callbacks(); }

        let cur = current_thread();
        let cur_ref = ThreadAccess::assume(cur);
        let entry_fn: extern "C" fn(u64, u64) -> c_int = core::mem::transmute(cur_ref.kentry() as *const ());
        let ret = entry_fn(cur_ref.arg(0), cur_ref.arg(1));
        exit(ret);
    }
}}

#[no_mangle]
pub extern "C" fn kthread_create(name: *const c_char, entry: *mut c_void,
                                         arg1: u64, arg2: u64, stack_order: c_int)-> *mut thread  { u! {
    thread_raw_layout! {
        let _rcu = crate::lock::rcu::KRcuRead::new();
        let initproc = __proctab_get_initproc();
        kassert!(!initproc.is_null(), "kthread_create: initproc is NULL");

        if __alloc_pid() < 0 {
            return err_ptr(-EAGAIN);
        }

        let p = thread_create(entry, arg1, arg2, stack_order);
        if is_err_or_null(p) {
            __free_pid();
            return if is_err(p) { p } else { err_ptr(-ENOMEM) };
        }

        let mut fs_clone: *mut fs_struct = ptr::null_mut();
        if !(*initproc).fs.is_null() {
            fs_clone = vfs_struct_clone((*initproc).fs, 0);
            if is_err_or_null(fs_clone) {
                __free_pid();
                thread_destroy(p);
                return if is_err(fs_clone) { fs_clone as *mut thread } else { err_ptr(-ENOMEM) };
            }
        }

        let se = (*p).sched_entity;
        (*se).context.ra = kthread_entry as usize as u64;
        (*p).kentry = entry as u64;
        (*p).arg[0] = arg1;
        (*p).arg[1] = arg2;
        (*p).fs = fs_clone;
        let name_use = if name.is_null() { c"kthread".as_ptr() } else { name };
        safestrcpy((*p).name.as_mut_ptr(), name_use, (*p).name.len());
        thread_state_set(p, THREAD_UNINTERRUPTIBLE);

        pid_wlock();
        attach_child(initproc, p);
        proctab_proc_add(p);
        kthread_join_hierarchy_locked(p);
        pid_wunlock();

        p
    }
}}

#[no_mangle]
pub extern "C" fn idle_thread_init() { u! {
    let kstack_size = KERNEL_STACK_SIZE;
    let kstack = (read_sp() & !(kstack_size - 1)) as *mut c_void;
    kassert!((PAGE_SIZE << KERNEL_STACK_ORDER) == kstack_size, "idle_thread_init: invalid KERNEL_STACK_ORDER");
    thread_raw_layout! {
        let p = kstack_arrange(kstack, kstack_size, 0);
        kassert!(!p.is_null(), "idle_thread_init: failed to arrange kstack");

        (*p).kstack_order = KERNEL_STACK_ORDER;
        (*p).kstack = kstack as u64;
        strncpy((*p).name.as_mut_ptr(), c"idle".as_ptr(), (*p).name.len());
        thread_state_set(p, THREAD_RUNNING);
        let cpu = CpuLocalRef::assume(cpu_local_ptr());
        cpu.set_proc(p);
        cpu.set_idle_thread(p);

        xv6_rqport_rq_cpu_activate(cpuid());

        let mut attr: sched_attr = core::mem::zeroed();
        xv6_rqport_sched_attr_init(&mut attr);
        attr.priority = IDLE_PRIORITY;
        attr.affinity_mask = 1u64 << cpuid();
        xv6_rqport_sched_setattr((*p).sched_entity, &attr);

        xv6_rqport_rq_lock_current();
        let idle_rq = get_rq_for_cpu(IDLE_MAJOR_PRIORITY, cpuid());
        xv6_rqport_rq_enqueue_task(idle_rq, (*p).sched_entity);
        xv6_rqport_rq_unlock_current();
        // smp_store_release on on_cpu
        let se = (*p).sched_entity;
        core::sync::atomic::fence(Ordering::Release);
        core::ptr::write_volatile(&mut (*se).on_cpu as *mut c_int, 1);

        printf(c"CPU %ld idle process initialized at kstack 0x%lx\n".as_ptr(),
               cpuid() as u64, kstack as u64);
    }
}}

extern "C" fn thread_destroy_rcu_callback(data: *mut c_void) { u! {
    let t = ThreadAccess::assume(data as *mut thread);
    page_free(t.kstack_addr() as *mut c_void, t.kstack_order());
}}

#[no_mangle]
pub extern "C" fn thread_destroy(p: *mut thread) { u! {
    thread_raw_layout! {
        kassert!(!p.is_null(), "thread_destroy called with NULL thread");
        kassert!(!thread_is_awoken(p), "thread_destroy called with a runnable thread");
        kassert!(!thread_is_sleeping(p), "thread_destroy called with a sleeping thread");
        kassert!((*p).kstack_order >= 0 && (*p).kstack_order <= PAGE_BUDDY_MAX_ORDER,
                 "thread_destroy: invalid kstack_order");

        let ta_d = crate::proc::access::ThreadAccess::from_raw(p).unwrap_unchecked();
        if !ta_d.sigacts_ptr().is_null() {
            xv6_sigport_sigacts_put(ta_d.sigacts_ptr());
            ta_d.set_sigacts(ptr::null_mut());
        }
        if !ta_d.vm_ptr().is_null() {
            vm_put(ta_d.vm_ptr());
            ta_d.set_vm(ptr::null_mut());
        }
        if !ta_d.fdtable_ptr().is_null() {
            vfs_fdtable_put(ta_d.fdtable_ptr());
            ta_d.set_fdtable(ptr::null_mut());
        }
        if !(*p).fs.is_null() {
            vfs_struct_put((*p).fs);
            (*p).fs = ptr::null_mut();
        }

        xv6_sigport_sigpending_empty(p, 0);
        xv6_sigport_sigpending_destroy(p);

        let ta = crate::proc::access::ThreadAccess::from_raw(p).unwrap_unchecked();
        if !ta.thread_group_ptr().is_null() {
            thread_group_put(ta.thread_group_ptr());
            ta.set_thread_group(ptr::null_mut());
        }

        call_rcu(&mut (*p).rcu_head, Some(thread_destroy_rcu_callback), p as *mut c_void);
    }
}}

extern "C" fn init_entry(prev: *mut context) { u! {
    thread_raw_layout! {
        let cur = current_thread();
        xv6_schport_context_switch_finish(thread_from_context(prev), cur, 0);
        CpuLocalRef::assume(cpu_local_ptr()).set_noff(0);
        intr_on();

        start_kernel_post_init();

        let init_path = c"/bin/init".as_ptr() as *mut c_char;
        let argv0 = c"init".as_ptr() as *mut c_char;
        let mut argv: [*mut c_char; 2] = [argv0, ptr::null_mut()];
        let ret = exec(init_path, argv.as_mut_ptr(), ptr::null_mut());
        kassert!(ret >= 0, "init_entry: exec /bin/init failed");

        core::sync::atomic::fence(Ordering::SeqCst);
        usertrapret();
    }
}}

#[no_mangle]
pub extern "C" fn userinit() { u! {
    thread_raw_layout! {
        kassert!(__alloc_pid() == 0, "userinit: __alloc_pid failed");

        let p = thread_create(init_entry as *mut c_void, 0, 0, KERNEL_STACK_ORDER);
        kassert!(!is_err_or_null(p), "userinit: thread_create failed");

        pid_wlock();
        proctab_proc_add(p);
        thread_group_init(p);
        pgroup_init(p);
        session_init(p);
        pid_wunlock();

        printf(c"Init process kernel stack size order: %d\n".as_ptr(), (*p).kstack_order);

        let vm = vm_init();
        kassert!(!is_err_or_null(vm), "userinit: vm_init failed");
        let ta_u = crate::proc::access::ThreadAccess::from_raw(p).unwrap_unchecked();
        ta_u.set_vm(vm);

        __proctab_set_initproc(p);

        ta_u.set_sigacts(sigacts_init());
        kassert!(!ta_u.sigacts_ptr().is_null(), "userinit: sigacts_init failed");

        safestrcpy((*p).name.as_mut_ptr(), c"initcode".as_ptr(), (*p).name.len());

        thread_set_user_space(p);

        let mut attr: sched_attr = core::mem::zeroed();
        xv6_rqport_sched_attr_init(&mut attr);
        xv6_rqport_sched_setattr((*p).sched_entity, &attr);

        thread_state_set(p, THREAD_UNINTERRUPTIBLE);
        xv6_schport_scheduler_wakeup(p);
    }
}}

#[no_mangle]
pub extern "C" fn install_user_root() { u! {
    thread_raw_layout! {
        let p = current_thread();

        let root_inode = vfs_namei(c"/".as_ptr(), 1);
        if root_inode.is_null() {
            kpanic!("install_user_root: cannot find root directory");
        }

        kassert!(!(*p).fs.is_null(), "install_user_root: thread fs_struct is NULL");

        tcb_lock(p);
        thread_set_user_space(p);
        tcb_unlock(p);

        // vfs_inode_ref is a struct of {inode*, idx, gen} or similar; we
        // can't introspect its layout here. Delegate to a tiny C helper:
        install_user_root_finish(p, root_inode);
    }
}}

unsafe extern "C" {
    fn install_user_root_finish(p: *mut thread, root_inode: *mut c_void);
}

// ---------------- xv6_thport_* aliases ----------------------------------
macro_rules! thport_alias {
    ($alias:ident => $target:ident ( $($pn:ident : $pt:ty),* ) -> $ret:ty) => {
        #[no_mangle]
        pub extern "C" fn $alias($($pn: $pt),*) -> $ret { $target($($pn),*) }
    };
    ($alias:ident => $target:ident ( $($pn:ident : $pt:ty),* )) => {
        #[no_mangle]
        pub extern "C" fn $alias($($pn: $pt),*) { $target($($pn),*) }
    };
}

macro_rules! thport_unsafe_alias {
    ($alias:ident => $target:ident ( $($pn:ident : $pt:ty),* ) -> $ret:ty) => {
        #[no_mangle]
        pub extern "C" fn $alias($($pn: $pt),*) -> $ret { thport_unsafe_call!($target($($pn),*)) }
    };
    ($alias:ident => $target:ident ( $($pn:ident : $pt:ty),* )) => {
        #[no_mangle]
        pub extern "C" fn $alias($($pn: $pt),*) { thport_unsafe_call!($target($($pn),*)) }
    };
}

thport_alias!(xv6_thport_tcb_lock => tcb_lock(p: *mut thread));
thport_alias!(xv6_thport_tcb_unlock => tcb_unlock(p: *mut thread));
thport_alias!(xv6_thport_proc_assert_holding => proc_assert_holding(p: *mut thread));
thport_alias!(xv6_thport_thread_init => thread_init());
thport_unsafe_alias!(xv6_thport_attach_child => attach_child(parent: *mut thread, child: *mut thread));
thport_unsafe_alias!(xv6_thport_detach_child => detach_child(parent: *mut thread, child: *mut thread));
thport_unsafe_alias!(xv6_thport_thread_create => thread_create(entry: *mut c_void, a1: u64, a2: u64, ord: c_int) -> *mut thread);
thport_unsafe_alias!(xv6_thport_kthread_create => kthread_create(name: *const c_char, entry: *mut c_void, a1: u64, a2: u64, ord: c_int) -> *mut thread);
thport_unsafe_alias!(xv6_thport_idle_thread_init => idle_thread_init());
thport_unsafe_alias!(xv6_thport_thread_destroy => thread_destroy(p: *mut thread));
thport_unsafe_alias!(xv6_thport_userinit => userinit());
thport_unsafe_alias!(xv6_thport_install_user_root => install_user_root());