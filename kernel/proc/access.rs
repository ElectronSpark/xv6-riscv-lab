#![allow(dead_code)]

use core::ffi::{c_char, c_int, c_void};
use core::marker::PhantomData;
use core::ptr::NonNull;
use core::sync::atomic::{fence, AtomicI32, AtomicPtr, AtomicU32, AtomicU64, Ordering};

use crate::bindings::{
    context, cpu_local, hlist_entry_t, ksiginfo, pgroup, rq, rq_percpu, sched_attr,
    sched_class, sched_entity, session, sigaction as sigaction_t, spinlock_t, thread, thread_group,
    thread_state,
};
use crate::proc::proc_shims;

macro_rules! shim_call {
    ($self:expr, $fn:ident $(, $arg:expr)* $(,)?) => {{
        #[allow(unused_unsafe)]
        unsafe { proc_shims::$fn(($self).raw.as_ptr(), $($arg),*) }
    }};
}

macro_rules! raw_get {
    ($self:expr, $field:ident) => {{ unsafe { (*($self).raw.as_ptr()).$field } }};
    ($self:expr, $field:ident.$subfield:ident) => {{ unsafe { (*($self).raw.as_ptr()).$field.$subfield } }};
    ($self:expr, $field:ident.$subfield:ident.$leaf:ident) => {{ unsafe { (*($self).raw.as_ptr()).$field.$subfield.$leaf } }};
}

macro_rules! raw_set {
    ($self:expr, $field:ident, $value:expr) => {{ unsafe { (*($self).raw.as_ptr()).$field = $value; } }};
    ($self:expr, $field:ident.$subfield:ident, $value:expr) => {{ unsafe { (*($self).raw.as_ptr()).$field.$subfield = $value; } }};
    ($self:expr, $field:ident.$subfield:ident.$leaf:ident, $value:expr) => {{ unsafe { (*($self).raw.as_ptr()).$field.$subfield.$leaf = $value; } }};
}

macro_rules! raw_mut_ptr {
    ($self:expr, $field:ident) => {{ unsafe { &raw mut (*($self).raw.as_ptr()).$field } }};
    ($self:expr, $field:ident.$subfield:ident) => {{ unsafe { &raw mut (*($self).raw.as_ptr()).$field.$subfield } }};
    ($self:expr, $field:ident.$subfield:ident.$leaf:ident) => {{ unsafe { &raw mut (*($self).raw.as_ptr()).$field.$subfield.$leaf } }};
}

macro_rules! raw_const_ptr {
    ($self:expr, $field:ident) => {{ unsafe { &raw const (*($self).raw.as_ptr()).$field } }};
    ($self:expr, $field:ident.$subfield:ident) => {{ unsafe { &raw const (*($self).raw.as_ptr()).$field.$subfield } }};
    ($self:expr, $field:ident.$subfield:ident.$leaf:ident) => {{ unsafe { &raw const (*($self).raw.as_ptr()).$field.$subfield.$leaf } }};
}

macro_rules! raw_add_assign {
    ($self:expr, $field:ident, $value:expr) => {{ unsafe { (*($self).raw.as_ptr()).$field += $value; } }};
}

macro_rules! raw_sub_assign {
    ($self:expr, $field:ident, $value:expr) => {{ unsafe { (*($self).raw.as_ptr()).$field -= $value; } }};
}

macro_rules! raw_bitand_assign {
    ($self:expr, $field:ident, $value:expr) => {{ unsafe { (*($self).raw.as_ptr()).$field &= $value; } }};
}

macro_rules! raw_bitor_assign {
    ($self:expr, $field:ident, $value:expr) => {{ unsafe { (*($self).raw.as_ptr()).$field |= $value; } }};
}

macro_rules! raw_index_get {
    ($self:expr, $field:ident, $idx:expr) => {{ unsafe { (*($self).raw.as_ptr()).$field[$idx] } }};
}

macro_rules! raw_index_set {
    ($self:expr, $field:ident, $idx:expr, $value:expr) => {{ unsafe { (*($self).raw.as_ptr()).$field[$idx] = $value; } }};
}

macro_rules! raw_index_ptr_mut {
    ($self:expr, $field:ident, $idx:expr) => {{ unsafe { &raw mut (*($self).raw.as_ptr()).$field[$idx] } }};
}

macro_rules! raw_method {
    ($self:expr, $field:ident.$method:ident()) => {{ unsafe { (*($self).raw.as_ptr()).$field.$method() } }};
}

macro_rules! atomic_i32_from_field {
    ($self:expr, $($field:tt)+) => {{ raw_mut_ptr!($self, $($field)+) as *mut AtomicI32 }};
}

macro_rules! atomic_u64_from_field {
    ($self:expr, $($field:tt)+) => {{ raw_mut_ptr!($self, $($field)+) as *mut AtomicU64 }};
}

macro_rules! atomic_ptr_from_field {
    ($self:expr, $($field:tt)+, $ty:ty) => {{ raw_mut_ptr!($self, $($field)+) as *mut AtomicPtr<$ty> }};
}

macro_rules! bit_get {
    ($self:expr, $anon:ident, $method:ident) => {{ unsafe { (*($self).raw.as_ptr()).$anon.$method() } }};
}

macro_rules! bit_set {
    ($self:expr, $anon:ident, $method:ident, $value:expr) => {{ unsafe { (*($self).raw.as_ptr()).$anon.$method($value); } }};
}

#[inline]
pub fn write_out<T>(p: *mut T, v: T) {
    unsafe { *p = v; }
}

#[inline]
pub fn zeroed<T>() -> T {
    unsafe { core::mem::zeroed() }
}

#[inline]
pub fn zero_obj<T>(p: *mut T) {
    if !p.is_null() {
        unsafe { core::ptr::write_bytes(p, 0, 1); }
    }
}

#[inline]
pub fn copy_out<T: Copy>(dst: *mut T, src: *const T) {
    if !dst.is_null() && !src.is_null() {
        unsafe { *dst = *src; }
    }
}

pub const MAX_ERRNO_ABS: usize = 4095;

#[inline]
pub fn is_err_value(p: usize) -> bool {
    p >= (-(MAX_ERRNO_ABS as isize)) as usize
}

#[inline]
pub fn is_err<T>(p: *mut T) -> bool {
    is_err_value(p as usize)
}

#[inline]
pub fn is_err_const<T>(p: *const T) -> bool {
    is_err_value(p as usize)
}

#[inline]
pub fn is_err_or_null<T>(p: *mut T) -> bool {
    p.is_null() || is_err(p)
}

#[inline]
pub fn err_ptr<T>(err: c_int) -> *mut T {
    err as isize as *mut T
}

#[inline]
pub fn err_ptr_errno<T>(errno: c_int) -> *mut T {
    err_ptr(-errno)
}

#[inline]
pub fn ptr_err<T>(p: *mut T) -> c_int {
    p as isize as c_int
}

#[inline]
pub fn ptr_err_errno<T>(p: *mut T) -> c_int {
    -ptr_err(p)
}

#[inline]
pub fn ptr_err_or<T>(p: *mut T, err: c_int) -> c_int {
    if is_err(p) { ptr_err(p) } else { err }
}

pub use crate::machine::{
    atomic_load_acquire_i32, atomic_store_release_i32, atomic_inc_i32, atomic_dec_unless_i32,
    atomic_store_release_u64, smp_load_acquire_i32, smp_store_release_i32,
    smp_load_acquire_state, smp_store_release_state, smp_store_release_u64,
    smp_load_acquire_u64, smp_rmb,
};

#[derive(Clone, Copy)]
pub struct SpinLockRef<'a> {
    raw: NonNull<spinlock_t>,
    _m: PhantomData<&'a mut spinlock_t>,
}

impl<'a> SpinLockRef<'a> {
    #[inline(always)]
    pub unsafe fn from_raw(p: *mut spinlock_t) -> Option<Self> {
        NonNull::new(p).map(|n| Self { raw: n, _m: PhantomData })
    }
    #[inline(always)] pub fn from_ptr(p: *mut spinlock_t) -> Option<Self> { unsafe { Self::from_raw(p) } }
    #[inline(always)] pub fn assume(p: *mut spinlock_t) -> Self { unsafe { Self::from_raw(p).unwrap_unchecked() } }
    #[inline(always)]
    pub fn as_ptr(&self) -> *mut spinlock_t { self.raw.as_ptr() }
    #[inline] pub fn lock(&self) { unsafe { crate::lock::spinlock::spin_lock(self.raw.as_ptr()); } }
    #[inline] pub fn init(&self, name: *mut c_char) { unsafe { crate::lock::spinlock::spin_init(self.raw.as_ptr(), name); } }
    #[inline] pub fn trylock(&self) -> c_int { unsafe { crate::lock::spinlock::spin_trylock(self.raw.as_ptr()) } }
    #[inline] pub fn unlock(&self) { unsafe { crate::lock::spinlock::spin_unlock(self.raw.as_ptr()); } }
    #[inline] pub fn holding(&self) -> bool { unsafe { crate::lock::spinlock::spin_holding(self.raw.as_ptr()) != 0 } }
    #[inline] pub fn lock_irqsave(&self) -> c_int { unsafe { crate::lock::spinlock::spin_lock_irqsave(self.raw.as_ptr()) } }
    #[inline] pub fn unlock_irqrestore(&self, intr: c_int) { unsafe { crate::lock::spinlock::spin_unlock_irqrestore(self.raw.as_ptr(), intr); } }
    #[inline] pub fn scoped_lock(&self) -> SpinLockGuard<'a> { self.lock(); SpinLockGuard { lock: *self } }
}

pub struct SpinLockGuard<'a> { lock: SpinLockRef<'a> }

impl Drop for SpinLockGuard<'_> {
    #[inline]
    fn drop(&mut self) { self.lock.unlock(); }
}

#[derive(Clone, Copy)]
pub struct ThreadAccess<'a> {
    raw: NonNull<thread>,
    _m: PhantomData<&'a mut thread>,
}

impl<'a> ThreadAccess<'a> {
    #[inline(always)]
    pub unsafe fn from_raw(p: *mut thread) -> Option<Self> {
        NonNull::new(p).map(|n| Self { raw: n, _m: PhantomData })
    }
    #[inline(always)] pub fn from_ptr(p: *mut thread) -> Option<Self> { unsafe { Self::from_raw(p) } }
    #[inline(always)] pub fn assume(p: *mut thread) -> Self { unsafe { Self::from_raw(p).unwrap_unchecked() } }
    #[inline(always)]
    pub fn as_ptr(&self) -> *mut thread { self.raw.as_ptr() }

    #[inline] pub fn pid(&self) -> c_int { shim_call!(self, t_pid) }
    #[inline] pub fn tgid(&self) -> c_int { shim_call!(self, t_tgid) }
    #[inline] pub fn pgid(&self) -> c_int { shim_call!(self, t_pgid) }
    #[inline] pub fn sid(&self) -> c_int { shim_call!(self, t_sid) }
    #[inline] pub fn parent_ptr(&self) -> *mut thread { shim_call!(self, t_parent) }
    #[inline] pub fn pgroup_ptr(&self) -> *mut pgroup { shim_call!(self, t_pgroup) }
    #[inline] pub fn session_ptr(&self) -> *mut session { shim_call!(self, t_session) }
    #[inline] pub fn thread_group_ptr(&self) -> *mut thread_group { shim_call!(self, t_thread_group) }
    #[inline] pub fn set_pid(&self, v: c_int) { shim_call!(self, t_set_pid, v) }
    #[inline] pub fn set_pgid(&self, v: c_int) { shim_call!(self, t_set_pgid, v) }
    #[inline] pub fn set_pgroup(&self, pg: *mut pgroup) { shim_call!(self, t_set_pgroup, pg) }
    #[inline] pub fn vm_ptr(&self) -> *mut crate::bindings::vm_t { shim_call!(self, t_vm) }
    #[inline] pub fn fdtable_ptr(&self) -> *mut crate::bindings::vfs_fdtable { shim_call!(self, t_fdtable) }
    #[inline] pub fn sigacts_ptr(&self) -> *mut crate::bindings::sigacts_t { shim_call!(self, t_sigacts) }
    #[inline] pub fn trapframe_ptr(&self) -> *mut crate::bindings::utrapframe { shim_call!(self, t_trapframe) }
    #[inline] pub fn set_vm(&self, v: *mut crate::bindings::vm_t) { shim_call!(self, t_set_vm, v) }
    #[inline] pub fn set_fdtable(&self, v: *mut crate::bindings::vfs_fdtable) { shim_call!(self, t_set_fdtable, v) }
    #[inline] pub fn set_sigacts(&self, v: *mut crate::bindings::sigacts_t) { shim_call!(self, t_set_sigacts, v) }
    #[inline] pub fn set_trapframe(&self, v: *mut crate::bindings::utrapframe) { shim_call!(self, t_set_trapframe, v) }
    #[inline] pub fn set_parent(&self, par: *mut thread) { shim_call!(self, t_set_parent, par) }
    #[inline] pub fn set_thread_group(&self, tg: *mut thread_group) { shim_call!(self, t_set_thread_group, tg) }
    #[inline] pub fn set_session(&self, s: *mut session) { shim_call!(self, t_set_session, s) }
    #[inline] pub fn set_tgid(&self, v: c_int) { shim_call!(self, t_set_tgid, v) }
    #[inline] pub fn set_sid(&self, v: c_int) { shim_call!(self, t_set_sid, v) }

    #[inline]
    pub fn trapframe_a0(&self) -> u64 {
        let tf = self.trapframe_ptr();
        if tf.is_null() {
            0
        } else {
            unsafe { (*tf).trapframe.a0 }
        }
    }

    #[inline]
    pub fn get_heap_addr(&self) -> Option<u64> {
        let vm = self.vm_ptr();
        if vm.is_null() {
            return None;
        }
        unsafe {
            let heap = (*vm).heap;
            if heap.is_null() {
                return None;
            }
            Some((*heap).start + (*vm).heap_size as u64)
        }
    }
}

#[derive(Clone, Copy)]
pub struct PgroupAccess<'a> {
    raw: NonNull<pgroup>,
    _m: PhantomData<&'a mut pgroup>,
}

impl<'a> PgroupAccess<'a> {
    #[inline(always)]
    pub unsafe fn from_raw(p: *mut pgroup) -> Option<Self> {
        NonNull::new(p).map(|n| Self { raw: n, _m: PhantomData })
    }
    #[inline(always)]
    pub fn as_ptr(&self) -> *mut pgroup { self.raw.as_ptr() }

    #[inline] pub fn pgid(&self) -> c_int { shim_call!(self, pg_pgid) }
    #[inline] pub fn t_cnt(&self) -> c_int { shim_call!(self, pg_t_cnt) }
    #[inline] pub fn p_cnt(&self) -> c_int { shim_call!(self, pg_p_cnt) }
    #[inline] pub fn exited(&self) -> c_int { shim_call!(self, pg_exited) }
    #[inline] pub fn is_kernel(&self) -> c_int { shim_call!(self, pg_is_kernel) }
    #[inline] pub fn session_ptr(&self) -> *mut session { shim_call!(self, pg_session) }
    #[inline] pub fn set_pgid(&self, v: c_int) { shim_call!(self, pg_set_pgid, v) }
    #[inline] pub fn set_leader(&self, tg: *mut thread_group) { shim_call!(self, pg_set_leader, tg) }
    #[inline] pub fn set_session(&self, s: *mut session) { shim_call!(self, pg_set_session, s) }
    #[inline] pub fn set_exited(&self, v: c_int) { shim_call!(self, pg_set_exited, v) }
    #[inline] pub fn set_t_cnt(&self, v: c_int) { shim_call!(self, pg_set_t_cnt, v) }
    #[inline] pub fn set_p_cnt(&self, v: c_int) { shim_call!(self, pg_set_p_cnt, v) }
    #[inline] pub fn inc_t_cnt(&self) { shim_call!(self, pg_inc_t_cnt) }
    #[inline] pub fn dec_t_cnt(&self) { shim_call!(self, pg_dec_t_cnt) }
    #[inline] pub fn inc_p_cnt(&self) { shim_call!(self, pg_inc_p_cnt) }
    #[inline] pub fn dec_p_cnt(&self) { shim_call!(self, pg_dec_p_cnt) }
}

#[derive(Clone, Copy)]
pub struct ThreadGroupAccess<'a> {
    raw: NonNull<thread_group>,
    _m: PhantomData<&'a mut thread_group>,
}

impl<'a> ThreadGroupAccess<'a> {
    #[inline(always)]
    pub unsafe fn from_raw(p: *mut thread_group) -> Option<Self> {
        NonNull::new(p).map(|n| Self { raw: n, _m: PhantomData })
    }
    #[inline(always)] pub fn from_ptr(p: *mut thread_group) -> Option<Self> { unsafe { Self::from_raw(p) } }
    #[inline(always)] pub fn assume(p: *mut thread_group) -> Self { unsafe { Self::from_raw(p).unwrap_unchecked() } }
    #[inline(always)]
    pub fn as_ptr(&self) -> *mut thread_group { self.raw.as_ptr() }

    #[inline] pub fn pgroup_ptr(&self) -> *mut pgroup { shim_call!(self, tg_pgroup) }
    #[inline] pub fn tgid(&self) -> c_int { shim_call!(self, tg_tgid) }
    #[inline] pub fn set_pgroup(&self, pg: *mut pgroup) { shim_call!(self, tg_set_pgroup, pg) }
    #[inline] pub fn group_leader_ptr(&self) -> *mut thread { shim_call!(self, tg_group_leader) }
    #[inline] pub fn set_group_leader(&self, p: *mut thread) { raw_set!(self, group_leader, p) }
    #[inline] pub fn set_tgid(&self, v: c_int) { shim_call!(self, tg_set_tgid, v) }
    #[inline] pub fn thread_list_ptr(&self) -> *mut list_node_t { raw_mut_ptr!(self, thread_list) }
    #[inline] pub fn thread_list_ref(&self) -> ListNodeRef<'a> { ListNodeRef::assume(self.thread_list_ptr()) }
    #[inline] pub fn list_entry_ptr(&self) -> *mut list_node_t { raw_mut_ptr!(self, list_entry) }
    #[inline] pub fn list_entry_ref(&self) -> ListNodeRef<'a> { ListNodeRef::assume(self.list_entry_ptr()) }
    #[inline] pub fn shared_pending_ptr(&self) -> *mut crate::bindings::tg_shared_pending {
        raw_mut_ptr!(self, shared_pending)
    }
    #[inline] pub fn shared_pending_ref(&self) -> TgSharedPendingRef<'a> {
        TgSharedPendingRef::assume(self.shared_pending_ptr())
    }
    #[inline] pub fn refcount_store(&self, v: c_int) {
        let ap = raw_mut_ptr!(self, refcount) as *mut AtomicI32;
        unsafe { (*ap).store(v, Ordering::SeqCst); }
    }
    #[inline] pub fn refcount_fetch_add(&self, v: c_int) -> c_int {
        let ap = raw_mut_ptr!(self, refcount) as *mut AtomicI32;
        unsafe { (*ap).fetch_add(v, Ordering::SeqCst) }
    }
    #[inline] pub fn refcount_dec_unless(&self, floor: c_int) -> bool {
        let p = raw_mut_ptr!(self, refcount);
        atomic_dec_unless_i32(p, floor)
    }
    #[inline] pub fn live_threads_load_acquire(&self) -> c_int {
        let ap = raw_mut_ptr!(self, live_threads) as *mut AtomicI32;
        unsafe { (*ap).load(Ordering::Acquire) }
    }
    #[inline] pub fn live_threads_store(&self, v: c_int) {
        let ap = raw_mut_ptr!(self, live_threads) as *mut AtomicI32;
        unsafe { (*ap).store(v, Ordering::SeqCst); }
    }
    #[inline] pub fn live_threads_fetch_add(&self, v: c_int) -> c_int {
        let ap = raw_mut_ptr!(self, live_threads) as *mut AtomicI32;
        unsafe { (*ap).fetch_add(v, Ordering::SeqCst) }
    }
    #[inline] pub fn live_threads_fetch_sub(&self, v: c_int) -> c_int {
        let ap = raw_mut_ptr!(self, live_threads) as *mut AtomicI32;
        unsafe { (*ap).fetch_sub(v, Ordering::SeqCst) }
    }
    #[inline] pub fn group_exit_store(&self, v: c_int) {
        let ap = raw_mut_ptr!(self, group_exit) as *mut AtomicI32;
        unsafe { (*ap).store(v, Ordering::SeqCst); }
    }
    #[inline] pub fn group_exit_task_compare_exchange_null(&self, p: *mut thread) -> bool {
        let ap = raw_mut_ptr!(self, group_exit_task) as *mut AtomicPtr<thread>;
        unsafe { (*ap).compare_exchange(core::ptr::null_mut(), p, Ordering::SeqCst, Ordering::SeqCst).is_ok() }
    }
    #[inline] pub fn set_group_exit_task(&self, p: *mut thread) { raw_set!(self, group_exit_task, p) }
    #[inline] pub fn set_group_exit_code(&self, code: c_int) { raw_set!(self, group_exit_code, code) }
    #[inline] pub fn set_group_stop_count(&self, v: c_int) { raw_set!(self, group_stop_count, v) }
    #[inline] pub fn set_group_stop_signo(&self, v: c_int) { raw_set!(self, group_stop_signo, v) }
    #[inline] pub fn is_kernel(&self) -> c_int { bit_get!(self, __bindgen_anon_1, is_kernel) as c_int }
    #[inline] pub fn set_is_kernel(&self, v: u64) { bit_set!(self, __bindgen_anon_1, set_is_kernel, v) }
}

#[derive(Clone, Copy)]
pub struct SessionAccess<'a> {
    raw: NonNull<session>,
    _m: PhantomData<&'a mut session>,
}

impl<'a> SessionAccess<'a> {
    #[inline(always)]
    pub unsafe fn from_raw(p: *mut session) -> Option<Self> {
        NonNull::new(p).map(|n| Self { raw: n, _m: PhantomData })
    }
    #[inline(always)]
    pub fn as_ptr(&self) -> *mut session { self.raw.as_ptr() }

    #[inline] pub fn sid(&self) -> c_int { shim_call!(self, session_sid) }
    #[inline] pub fn t_cnt(&self) -> c_int { shim_call!(self, session_t_cnt) }
    #[inline] pub fn pg_cnt(&self) -> c_int { shim_call!(self, session_pg_cnt) }
    #[inline] pub fn fg_pgrp_ptr(&self) -> *mut pgroup { shim_call!(self, session_fg_pgrp) }
}

// =========================================================================
// Extended ThreadAccess: atomic state/flag accessors + safe spin lock ref +
// signal-substruct pointer.
// =========================================================================
impl<'a> ThreadAccess<'a> {
    #[inline]
    pub fn state_load(&self) -> u32 {
        let p = self.raw.as_ptr();
        let ap = unsafe { &raw mut (*p).state } as *mut AtomicU32;
        unsafe { (*ap).load(Ordering::SeqCst) }
    }
    #[inline]
    pub fn state_store(&self, v: u32) {
        let p = self.raw.as_ptr();
        let ap = unsafe { &raw mut (*p).state } as *mut AtomicU32;
        unsafe { (*ap).store(v, Ordering::SeqCst); }
    }
    #[inline]
    pub fn flags_load(&self) -> u64 {
        let p = self.raw.as_ptr();
        let ap = unsafe { &raw mut (*p).flags } as *mut AtomicU64;
        unsafe { (*ap).load(Ordering::Acquire) }
    }
    #[inline]
    pub fn flags_test_bit(&self, bit: u64) -> bool {
        (self.flags_load() & (1u64 << bit)) != 0
    }
    #[inline]
    pub fn flags_set_bit(&self, bit: u64) {
        let p = self.raw.as_ptr();
        let ap = unsafe { &raw mut (*p).flags } as *mut AtomicU64;
        unsafe { (*ap).fetch_or(1u64 << bit, Ordering::AcqRel); }
    }
    #[inline]
    pub fn flags_clear_bit(&self, bit: u64) {
        let p = self.raw.as_ptr();
        let ap = unsafe { &raw mut (*p).flags } as *mut AtomicU64;
        unsafe { (*ap).fetch_and(!(1u64 << bit), Ordering::AcqRel); }
    }
    #[inline]
    pub fn lock_ref(&self) -> SpinLockRef<'a> {
        let p = self.raw.as_ptr();
        let lp = unsafe { &raw mut (*p).lock };
        unsafe { SpinLockRef::from_raw(lp).unwrap_unchecked() }
    }
    #[inline]
    pub fn signal_ptr(&self) -> *mut crate::bindings::thread_signal_t {
        let p = self.raw.as_ptr();
        unsafe { &raw mut (*p).signal }
    }
    #[inline] pub fn sched_entity_ptr(&self) -> *mut sched_entity { raw_get!(self, sched_entity) }
    #[inline] pub fn set_sched_entity_ptr(&self, se: *mut sched_entity) { raw_set!(self, sched_entity, se) }
    #[inline] pub fn name_ptr(&self) -> *const c_char { raw_method!(self, name.as_ptr()) }
    #[inline] pub fn chan_ptr(&self) -> *mut c_void { raw_get!(self, chan) }
    #[inline] pub fn set_chan(&self, chan: *mut c_void) { raw_set!(self, chan, chan) }
    #[inline] pub fn kstack_addr(&self) -> u64 { raw_get!(self, kstack) }
    #[inline] pub fn set_kstack_addr(&self, v: u64) { raw_set!(self, kstack, v) }
    #[inline] pub fn kstack_order(&self) -> c_int { raw_get!(self, kstack_order) }
    #[inline] pub fn set_kstack_order(&self, v: c_int) { raw_set!(self, kstack_order, v) }
    #[inline] pub fn ksp(&self) -> u64 { raw_get!(self, ksp) }
    #[inline] pub fn set_ksp(&self, v: u64) { raw_set!(self, ksp, v) }
    #[inline] pub fn kentry(&self) -> u64 { raw_get!(self, kentry) }
    #[inline] pub fn set_kentry(&self, v: u64) { raw_set!(self, kentry, v) }
    #[inline] pub fn arg(&self, idx: usize) -> u64 { raw_index_get!(self, arg, idx) }
    #[inline] pub fn set_arg(&self, idx: usize, v: u64) { raw_index_set!(self, arg, idx, v) }
    #[inline] pub fn fs_ptr(&self) -> *mut crate::bindings::fs_struct { raw_get!(self, fs) }
    #[inline] pub fn set_fs(&self, v: *mut crate::bindings::fs_struct) { raw_set!(self, fs, v) }
    #[inline] pub fn name_buf_ptr(&self) -> *mut c_char { raw_method!(self, name.as_mut_ptr()) }
    #[inline] pub fn name_len(&self) -> usize { raw_method!(self, name.len()) }
    #[inline] pub fn children_count(&self) -> c_int { raw_get!(self, children_count) }
    #[inline] pub fn inc_children_count(&self) { raw_add_assign!(self, children_count, 1) }
    #[inline] pub fn dec_children_count(&self) { raw_sub_assign!(self, children_count, 1) }
    #[inline] pub fn tg_entry_ptr(&self) -> *mut list_node_t { raw_mut_ptr!(self, tg_entry) }
    #[inline] pub fn tg_entry_ref(&self) -> ListNodeRef<'a> { ListNodeRef::assume(self.tg_entry_ptr()) }
    #[inline] pub fn sched_entry_ptr(&self) -> *mut list_node_t { raw_mut_ptr!(self, sched_entry) }
    #[inline] pub fn dmp_list_entry_ptr(&self) -> *mut list_node_t { raw_mut_ptr!(self, dmp_list_entry) }
    #[inline] pub fn siblings_ptr(&self) -> *mut list_node_t { raw_mut_ptr!(self, siblings) }
    #[inline] pub fn children_ptr(&self) -> *mut list_node_t { raw_mut_ptr!(self, children) }
    #[inline] pub fn pg_entry_ptr(&self) -> *mut list_node_t { raw_mut_ptr!(self, pg_entry) }
    #[inline] pub fn sid_entry_ptr(&self) -> *mut list_node_t { raw_mut_ptr!(self, sid_entry) }
    #[inline] pub fn sched_entry_ref(&self) -> ListNodeRef<'a> { ListNodeRef::assume(self.sched_entry_ptr()) }
    #[inline] pub fn dmp_list_entry_ref(&self) -> ListNodeRef<'a> { ListNodeRef::assume(self.dmp_list_entry_ptr()) }
    #[inline] pub fn siblings_ref(&self) -> ListNodeRef<'a> { ListNodeRef::assume(self.siblings_ptr()) }
    #[inline] pub fn children_ref(&self) -> ListNodeRef<'a> { ListNodeRef::assume(self.children_ptr()) }
    #[inline] pub fn pg_entry_ref(&self) -> ListNodeRef<'a> { ListNodeRef::assume(self.pg_entry_ptr()) }
    #[inline] pub fn sid_entry_ref(&self) -> ListNodeRef<'a> { ListNodeRef::assume(self.sid_entry_ptr()) }
    #[inline] pub fn proctab_entry_ptr(&self) -> *mut hlist_entry_t { raw_mut_ptr!(self, proctab_entry) }
    #[inline] pub fn rcu_head_ptr(&self) -> *mut crate::bindings::rcu_head_t { raw_mut_ptr!(self, rcu_head) }
    #[inline] pub fn zero_sched_entity_storage(&self) {
        let se = self.sched_entity_ptr();
        if !se.is_null() { zero_obj(se); }
    }
    #[inline]
    pub fn init_embedded_lists_and_lock(&self, lock_name: *mut c_char) {
        self.sched_entry_ref().init();
        self.dmp_list_entry_ref().init();
        self.siblings_ref().init();
        self.children_ref().init();
        ListNodeRef::assume(self.proctab_entry_ptr() as *mut list_node_t).init();
        self.tg_entry_ref().init();
        self.pg_entry_ref().init();
        self.sid_entry_ref().init();
        self.wq_entry_ref().init();
        self.lock_ref().init(lock_name);
    }
}

// =========================================================================
// scheduler/runqueue boundary wrappers
// =========================================================================

#[derive(Clone, Copy)]
pub struct CpuLocalRef<'a> {
    raw: NonNull<cpu_local>,
    _m: PhantomData<&'a mut cpu_local>,
}

impl<'a> CpuLocalRef<'a> {
    #[inline(always)]
    pub unsafe fn from_raw(p: *mut cpu_local) -> Option<Self> {
        NonNull::new(p).map(|n| Self { raw: n, _m: PhantomData })
    }
    #[inline(always)] pub fn from_ptr(p: *mut cpu_local) -> Option<Self> { unsafe { Self::from_raw(p) } }
    #[inline(always)] pub fn assume(p: *mut cpu_local) -> Self { unsafe { Self::from_raw(p).unwrap_unchecked() } }
    #[inline(always)]
    pub fn as_ptr(&self) -> *mut cpu_local { self.raw.as_ptr() }
    #[inline] pub fn proc_ptr(&self) -> *mut thread { raw_get!(self, proc_) }
    #[inline] pub fn set_proc(&self, p: *mut thread) { raw_set!(self, proc_, p) }
    #[inline] pub fn idle_thread_ptr(&self) -> *mut thread { raw_get!(self, idle_thread) }
    #[inline] pub fn set_idle_thread(&self, p: *mut thread) { raw_set!(self, idle_thread, p) }
    #[inline] pub fn flags(&self) -> u64 { raw_get!(self, flags) }
    #[inline] pub fn flag_set(&self, bit: u64) -> bool { (self.flags() & bit) != 0 }
    #[inline] pub fn intena(&self) -> c_int { raw_get!(self, intena) }
    #[inline] pub fn set_intena(&self, v: c_int) { raw_set!(self, intena, v) }
    #[inline] pub fn noff(&self) -> c_int { raw_get!(self, noff) }
    #[inline] pub fn set_noff(&self, v: c_int) { raw_set!(self, noff, v) }
    #[inline] pub fn spin_depth(&self) -> c_int { raw_get!(self, spin_depth) }
    #[inline]
    pub fn store_rcu_timestamp_release(&self, v: u64) {
        let ap = raw_mut_ptr!(self, rcu_timestamp) as *mut AtomicU64;
        unsafe { (*ap).store(v, Ordering::Release); }
    }
}

#[derive(Clone, Copy)]
pub struct SchedEntityRef<'a> {
    raw: NonNull<sched_entity>,
    _m: PhantomData<&'a mut sched_entity>,
}

impl<'a> SchedEntityRef<'a> {
    #[inline(always)]
    pub unsafe fn from_raw(p: *mut sched_entity) -> Option<Self> {
        NonNull::new(p).map(|n| Self { raw: n, _m: PhantomData })
    }
    #[inline(always)] pub fn from_ptr(p: *mut sched_entity) -> Option<Self> { unsafe { Self::from_raw(p) } }
    #[inline(always)] pub fn assume(p: *mut sched_entity) -> Self { unsafe { Self::from_raw(p).unwrap_unchecked() } }
    #[inline(always)] pub fn as_ptr(&self) -> *mut sched_entity { self.raw.as_ptr() }

    #[inline] pub fn thread_ptr(&self) -> *mut thread { raw_get!(self, thread) }
    #[inline] pub fn set_thread(&self, p: *mut thread) { raw_set!(self, thread, p) }
    #[inline] pub fn rq_ptr(&self) -> *mut rq { raw_get!(self, rq) }
    #[inline] pub fn set_rq(&self, r: *mut rq) { raw_set!(self, rq, r) }
    #[inline] pub fn priority(&self) -> c_int { raw_get!(self, priority) }
    #[inline] pub fn set_priority(&self, v: c_int) { raw_set!(self, priority, v) }
    #[inline] pub fn sched_class_ptr(&self) -> *mut sched_class { raw_get!(self, sched_class) }
    #[inline] pub fn set_sched_class(&self, cls: *mut sched_class) { raw_set!(self, sched_class, cls) }
    #[inline] pub fn affinity_mask(&self) -> u64 { raw_get!(self, affinity_mask) }
    #[inline] pub fn set_affinity_mask(&self, mask: u64) { raw_set!(self, affinity_mask, mask) }
    #[inline] pub fn set_start_time(&self, v: u64) { raw_set!(self, start_time, v) }
    #[inline] pub fn set_exec_start(&self, v: u64) { raw_set!(self, exec_start, v) }
    #[inline] pub fn set_exec_end(&self, v: u64) { raw_set!(self, exec_end, v) }

    #[inline] pub fn pi_lock_ptr(&self) -> *mut spinlock_t { raw_mut_ptr!(self, pi_lock) }
    #[inline] pub fn pi_lock_ref(&self) -> SpinLockRef<'a> {
        unsafe { SpinLockRef::from_raw(self.pi_lock_ptr()).unwrap_unchecked() }
    }
    #[inline] pub fn context_ptr(&self) -> *mut context { raw_mut_ptr!(self, context) }
    #[inline] pub fn zero_context(&self) { zero_obj(self.context_ptr()); }
    #[inline]
    pub fn init_context(&self, ra: u64, sp: u64) {
        unsafe {
            (*self.raw.as_ptr()).context.ra = ra;
            (*self.raw.as_ptr()).context.sp = sp;
            (*self.raw.as_ptr()).context.s0 = 0;
        }
    }

    #[inline]
    pub fn on_cpu_load_acquire(&self) -> c_int {
        let ap = raw_mut_ptr!(self, on_cpu) as *mut AtomicI32;
        unsafe { (*ap).load(Ordering::Acquire) }
    }
    #[inline]
    pub fn on_cpu_store_release(&self, v: c_int) {
        let ap = raw_mut_ptr!(self, on_cpu) as *mut AtomicI32;
        unsafe { (*ap).store(v, Ordering::Release); }
    }
    #[inline] pub fn on_cpu_plain(&self) -> c_int { raw_get!(self, on_cpu) }
    #[inline] pub fn set_on_cpu_plain(&self, v: c_int) { raw_set!(self, on_cpu, v) }

    #[inline]
    pub fn on_rq_load_acquire(&self) -> c_int {
        let ap = raw_mut_ptr!(self, on_rq) as *mut AtomicI32;
        unsafe { (*ap).load(Ordering::Acquire) }
    }
    #[inline]
    pub fn on_rq_store_release(&self, v: c_int) {
        let ap = raw_mut_ptr!(self, on_rq) as *mut AtomicI32;
        unsafe { (*ap).store(v, Ordering::Release); }
    }
    #[inline] pub fn on_rq_plain(&self) -> c_int { raw_get!(self, on_rq) }
    #[inline] pub fn set_on_rq_plain(&self, v: c_int) { raw_set!(self, on_rq, v) }

    #[inline]
    pub fn cpu_id_load_acquire(&self) -> c_int {
        let ap = raw_mut_ptr!(self, cpu_id) as *mut AtomicI32;
        unsafe { (*ap).load(Ordering::Acquire) }
    }
    #[inline]
    pub fn cpu_id_store_release(&self, v: c_int) {
        let ap = raw_mut_ptr!(self, cpu_id) as *mut AtomicI32;
        unsafe { (*ap).store(v, Ordering::Release); }
    }
    #[inline] pub fn cpu_id(&self) -> c_int { raw_get!(self, cpu_id) }
    #[inline] pub fn set_cpu_id(&self, v: c_int) { raw_set!(self, cpu_id, v) }

    #[inline] pub fn wake_next(&self) -> *mut sched_entity { raw_get!(self, wake_next) }
    #[inline] pub fn set_wake_next(&self, next: *mut sched_entity) { raw_set!(self, wake_next, next) }

    #[inline]
    pub fn call_sched_class_task_dead(&self) {
        let cls = self.sched_class_ptr();
        if cls.is_null() { return; }
        unsafe {
            if let Some(fp) = (*cls).task_dead { fp(self.rq_ptr(), self.raw.as_ptr()); }
        }
    }
}

#[derive(Clone, Copy)]
pub struct SchedClassRef<'a> {
    raw: NonNull<sched_class>,
    _m: PhantomData<&'a mut sched_class>,
}

impl<'a> SchedClassRef<'a> {
    #[inline(always)]
    pub unsafe fn from_raw(p: *mut sched_class) -> Option<Self> {
        NonNull::new(p).map(|n| Self { raw: n, _m: PhantomData })
    }
    #[inline(always)] pub fn from_ptr(p: *mut sched_class) -> Option<Self> { unsafe { Self::from_raw(p) } }
    #[inline(always)] pub fn assume(p: *mut sched_class) -> Self { unsafe { Self::from_raw(p).unwrap_unchecked() } }
    #[inline(always)] pub fn as_ptr(&self) -> *mut sched_class { self.raw.as_ptr() }
    #[inline] pub fn has_pick_next_task(&self) -> bool { unsafe { (*self.raw.as_ptr()).pick_next_task.is_some() } }
    #[inline]
    pub fn select_task_rq(&self, current_rq: *mut rq, se: *mut sched_entity, mask: u64) -> Option<*mut rq> {
        unsafe { (*self.raw.as_ptr()).select_task_rq.map(|fp| fp(current_rq, se, mask)) }
    }
    #[inline]
    pub fn task_fork(&self, r: *mut rq, se: *mut sched_entity) -> bool {
        unsafe {
            if let Some(fp) = (*self.raw.as_ptr()).task_fork {
                fp(r, se);
                true
            } else {
                false
            }
        }
    }
}

#[derive(Clone, Copy)]
pub struct RqRef<'a> {
    raw: NonNull<rq>,
    _m: PhantomData<&'a mut rq>,
}

impl<'a> RqRef<'a> {
    #[inline(always)]
    pub unsafe fn from_raw(p: *mut rq) -> Option<Self> {
        NonNull::new(p).map(|n| Self { raw: n, _m: PhantomData })
    }
    #[inline(always)] pub fn from_ptr(p: *mut rq) -> Option<Self> { unsafe { Self::from_raw(p) } }
    #[inline(always)] pub fn assume(p: *mut rq) -> Self { unsafe { Self::from_raw(p).unwrap_unchecked() } }
    #[inline(always)] pub fn as_ptr(&self) -> *mut rq { self.raw.as_ptr() }
    #[inline] pub fn cpu_id(&self) -> c_int { raw_get!(self, cpu_id) }
    #[inline] pub fn set_cpu_id(&self, v: c_int) { raw_set!(self, cpu_id, v) }
    #[inline] pub fn class_id(&self) -> c_int { raw_get!(self, class_id) }
    #[inline] pub fn set_class_id(&self, v: c_int) { raw_set!(self, class_id, v) }
    #[inline] pub fn task_count(&self) -> c_int { raw_get!(self, task_count) }
    #[inline] pub fn set_task_count(&self, v: c_int) { raw_set!(self, task_count, v) }
    #[inline] pub fn inc_task_count(&self) { raw_add_assign!(self, task_count, 1) }
    #[inline] pub fn dec_task_count(&self) { raw_sub_assign!(self, task_count, 1) }
    #[inline] pub fn sched_class_ptr(&self) -> *mut sched_class { raw_get!(self, sched_class) }
    #[inline] pub fn set_sched_class(&self, cls: *mut sched_class) { raw_set!(self, sched_class, cls) }

    #[inline] pub fn enqueue_task(&self, se: *mut sched_entity) {
        unsafe { if let Some(fp) = (*self.sched_class_ptr()).enqueue_task { fp(self.raw.as_ptr(), se); } }
    }
    #[inline] pub fn dequeue_task(&self, se: *mut sched_entity) {
        unsafe { if let Some(fp) = (*self.sched_class_ptr()).dequeue_task { fp(self.raw.as_ptr(), se); } }
    }
    #[inline] pub fn pick_next_task(&self) -> *mut sched_entity {
        unsafe { (*self.sched_class_ptr()).pick_next_task.map_or(core::ptr::null_mut(), |fp| fp(self.raw.as_ptr())) }
    }
    #[inline] pub fn put_prev_task(&self, se: *mut sched_entity) {
        unsafe { if let Some(fp) = (*self.sched_class_ptr()).put_prev_task { fp(self.raw.as_ptr(), se); } }
    }
    #[inline] pub fn set_next_task(&self, se: *mut sched_entity) {
        unsafe { if let Some(fp) = (*self.sched_class_ptr()).set_next_task { fp(self.raw.as_ptr(), se); } }
    }
    #[inline] pub fn task_tick(&self, se: *mut sched_entity) {
        unsafe { if let Some(fp) = (*self.sched_class_ptr()).task_tick { fp(self.raw.as_ptr(), se); } }
    }
    #[inline] pub fn yield_task(&self) {
        unsafe { if let Some(fp) = (*self.sched_class_ptr()).yield_task { fp(self.raw.as_ptr()); } }
    }
}

#[derive(Clone, Copy)]
pub struct RqPercpuRef<'a> {
    raw: NonNull<rq_percpu>,
    _m: PhantomData<&'a mut rq_percpu>,
}

impl<'a> RqPercpuRef<'a> {
    #[inline(always)]
    pub unsafe fn from_raw(p: *mut rq_percpu) -> Option<Self> {
        NonNull::new(p).map(|n| Self { raw: n, _m: PhantomData })
    }
    #[inline(always)] pub fn from_ptr(p: *mut rq_percpu) -> Option<Self> { unsafe { Self::from_raw(p) } }
    #[inline(always)] pub fn assume(p: *mut rq_percpu) -> Self { unsafe { Self::from_raw(p).unwrap_unchecked() } }
    #[inline(always)] pub fn as_ptr(&self) -> *mut rq_percpu { self.raw.as_ptr() }
    #[inline] pub fn lock_ptr(&self) -> *mut spinlock_t { raw_mut_ptr!(self, rq_lock) }
    #[inline] pub fn lock_ref(&self) -> SpinLockRef<'a> {
        unsafe { SpinLockRef::from_raw(self.lock_ptr()).unwrap_unchecked() }
    }
    #[inline] pub fn zero_storage(&self) { unsafe { core::ptr::write_bytes(self.raw.as_ptr(), 0, 1); } }
    #[inline] pub fn init_lock(&self, name: *mut c_char) { unsafe { crate::lock::spinlock::spin_init(self.lock_ptr(), name); } }
    #[inline] pub fn ready_mask(&self) -> u64 { raw_get!(self, ready_mask) }
    #[inline] pub fn ready_mask_secondary(&self) -> u64 { raw_get!(self, ready_mask_secondary) }
    #[inline] pub fn set_ready_mask(&self, v: u64) { raw_set!(self, ready_mask, v) }
    #[inline] pub fn set_ready_mask_secondary(&self, v: u64) { raw_set!(self, ready_mask_secondary, v) }
    #[inline] pub fn or_ready_mask(&self, v: u64) { self.set_ready_mask(self.ready_mask() | v); }
    #[inline] pub fn or_ready_mask_secondary(&self, v: u64) { self.set_ready_mask_secondary(self.ready_mask_secondary() | v); }
    #[inline] pub fn and_ready_mask(&self, v: u64) { self.set_ready_mask(self.ready_mask() & v); }
    #[inline] pub fn and_ready_mask_secondary(&self, v: u64) { self.set_ready_mask_secondary(self.ready_mask_secondary() & v); }
    #[inline] pub fn clear_wake_list_head(&self) { raw_set!(self, wake_list_head, core::ptr::null_mut()) }
    #[inline] pub fn rq_at(&self, idx: usize) -> *mut rq { raw_index_get!(self, rqs, idx) }
    #[inline] pub fn set_rq_at(&self, idx: usize, r: *mut rq) { raw_index_set!(self, rqs, idx, r) }
    #[inline]
    pub fn current_se_load_acquire(&self) -> *mut sched_entity {
        let ap = raw_mut_ptr!(self, current_se) as *mut AtomicPtr<sched_entity>;
        unsafe { (*ap).load(Ordering::Acquire) }
    }
    #[inline]
    pub fn current_se_store_release(&self, se: *mut sched_entity) {
        let ap = raw_mut_ptr!(self, current_se) as *mut AtomicPtr<sched_entity>;
        unsafe { (*ap).store(se, Ordering::Release); }
    }
    #[inline] pub fn wake_list_push(&self, node: *mut sched_entity) {
        unsafe { sched_entity_llist_push(&raw mut (*self.raw.as_ptr()).wake_list_head, node); }
    }
    #[inline] pub fn wake_list_migrate(&self) -> *mut sched_entity {
        unsafe { sched_entity_llist_migrate(&raw mut (*self.raw.as_ptr()).wake_list_head) }
    }
}

#[inline]
pub fn sched_entity_llist_push_head(head: &mut *mut sched_entity, node: *mut sched_entity) {
    unsafe { sched_entity_llist_push(head as *mut *mut sched_entity, node); }
}

#[inline]
unsafe fn sched_entity_llist_push(head_p: *mut *mut sched_entity, node: *mut sched_entity) {
    let head_atomic = head_p as *mut AtomicPtr<sched_entity>;
    let mut old = unsafe { (*head_atomic).load(Ordering::Acquire) };
    loop {
        unsafe {
            (*node).wake_next = old;
            match (*head_atomic).compare_exchange_weak(old, node, Ordering::Release, Ordering::Acquire) {
                Ok(_) => return,
                Err(prev) => old = prev,
            }
        }
    }
}

#[inline]
unsafe fn sched_entity_llist_migrate(head_p: *mut *mut sched_entity) -> *mut sched_entity {
    let head_atomic = head_p as *mut AtomicPtr<sched_entity>;
    let mut old = unsafe { (*head_atomic).load(Ordering::Acquire) };
    loop {
        match unsafe { (*head_atomic).compare_exchange_weak(old, core::ptr::null_mut(), Ordering::Release, Ordering::Acquire) } {
            Ok(_) => return old,
            Err(prev) => old = prev,
        }
    }
}

#[derive(Clone, Copy)]
pub struct SchedAttrRef<'a> {
    raw: NonNull<sched_attr>,
    _m: PhantomData<&'a mut sched_attr>,
}

impl<'a> SchedAttrRef<'a> {
    #[inline(always)]
    pub unsafe fn from_raw(p: *mut sched_attr) -> Option<Self> {
        NonNull::new(p).map(|n| Self { raw: n, _m: PhantomData })
    }
    #[inline(always)] pub fn from_ptr(p: *mut sched_attr) -> Option<Self> { unsafe { Self::from_raw(p) } }
    #[inline(always)] pub fn assume(p: *mut sched_attr) -> Self { unsafe { Self::from_raw(p).unwrap_unchecked() } }
    #[inline] pub fn set_size(&self, v: u32) { raw_set!(self, size, v) }
    #[inline] pub fn affinity_mask(&self) -> u64 { raw_get!(self, affinity_mask) }
    #[inline] pub fn set_affinity_mask(&self, v: u64) { raw_set!(self, affinity_mask, v) }
    #[inline] pub fn priority(&self) -> c_int { raw_get!(self, priority) }
    #[inline] pub fn set_priority(&self, v: c_int) { raw_set!(self, priority, v) }
    #[inline] pub fn set_time_slice(&self, v: u32) { raw_set!(self, time_slice, v) }
    #[inline] pub fn set_flags(&self, v: u32) { raw_set!(self, flags, v) }
}

#[derive(Clone, Copy)]
pub struct SchedAttrConstRef<'a> {
    raw: NonNull<sched_attr>,
    _m: PhantomData<&'a sched_attr>,
}

impl<'a> SchedAttrConstRef<'a> {
    #[inline(always)]
    pub unsafe fn from_raw(p: *const sched_attr) -> Option<Self> {
        NonNull::new(p as *mut sched_attr).map(|n| Self { raw: n, _m: PhantomData })
    }
    #[inline(always)] pub fn from_ptr(p: *const sched_attr) -> Option<Self> { unsafe { Self::from_raw(p) } }
    #[inline(always)] pub fn assume(p: *const sched_attr) -> Self { unsafe { Self::from_raw(p).unwrap_unchecked() } }
    #[inline] pub fn affinity_mask(&self) -> u64 { raw_get!(self, affinity_mask) }
    #[inline] pub fn priority(&self) -> c_int { raw_get!(self, priority) }
}

// =========================================================================
// SigactsAccess
// =========================================================================
#[derive(Clone, Copy)]
pub struct SigactsAccess<'a> {
    raw: NonNull<crate::bindings::sigacts_t>,
    _m: PhantomData<&'a mut crate::bindings::sigacts_t>,
}

impl<'a> SigactsAccess<'a> {
    #[inline(always)]
    pub unsafe fn from_raw(p: *mut crate::bindings::sigacts_t) -> Option<Self> {
        NonNull::new(p).map(|n| Self { raw: n, _m: PhantomData })
    }
    #[inline(always)] pub fn from_ptr(p: *mut crate::bindings::sigacts_t) -> Option<Self> { unsafe { Self::from_raw(p) } }
    #[inline(always)] pub fn assume(p: *mut crate::bindings::sigacts_t) -> Self { unsafe { Self::from_raw(p).unwrap_unchecked() } }
    #[inline(always)]
    pub fn as_ptr(&self) -> *mut crate::bindings::sigacts_t { self.raw.as_ptr() }

    #[inline] pub fn refcount(&self) -> c_int { shim_call!(self, sa_refcount) }
    #[inline] pub fn set_refcount(&self, v: c_int) { shim_call!(self, sa_set_refcount, v) }
    #[inline] pub fn refcount_inc(&self) { atomic_inc_i32(raw_mut_ptr!(self, refcount)); }
    #[inline] pub fn refcount_dec_unless(&self, floor: c_int) -> bool {
        atomic_dec_unless_i32(raw_mut_ptr!(self, refcount), floor)
    }
    #[inline] pub fn sigterm(&self) -> u64 { shim_call!(self, sa_sigterm) }
    #[inline] pub fn set_sigterm(&self, v: u64) { shim_call!(self, sa_set_sigterm, v) }
    #[inline] pub fn sigstop(&self) -> u64 { shim_call!(self, sa_sigstop) }
    #[inline] pub fn set_sigstop(&self, v: u64) { shim_call!(self, sa_set_sigstop, v) }
    #[inline] pub fn sigcont(&self) -> u64 { shim_call!(self, sa_sigcont) }
    #[inline] pub fn set_sigcont(&self, v: u64) { shim_call!(self, sa_set_sigcont, v) }
    #[inline] pub fn sigignore(&self) -> u64 { shim_call!(self, sa_sigignore) }
    #[inline] pub fn set_sigignore(&self, v: u64) { shim_call!(self, sa_set_sigignore, v) }
    #[inline] pub fn lock_ref(&self) -> SpinLockRef<'a> {
        let p = self.raw.as_ptr();
        let lp = unsafe { &raw mut (*p).lock };
        unsafe { SpinLockRef::from_raw(lp).unwrap_unchecked() }
    }
    #[inline] pub fn action_ptr(&self, signo: c_int) -> *mut crate::bindings::sigaction {
        raw_index_ptr_mut!(self, sa, signo as usize)
    }
    #[inline] pub fn action_ref(&self, signo: c_int) -> SigActionAccess<'a> {
        SigActionAccess::assume(self.action_ptr(signo))
    }
    #[inline] pub fn action_copy(&self, signo: c_int) -> crate::bindings::sigaction {
        self.action_ref(signo).copy()
    }
    #[inline] pub fn copy_action_to(&self, signo: c_int, out: *mut crate::bindings::sigaction) {
        self.action_ref(signo).copy_to(out);
    }
    #[inline] pub fn copy_action_from(&self, signo: c_int, src: *const crate::bindings::sigaction) {
        self.action_ref(signo).copy_from(src);
    }
}

#[derive(Clone, Copy)]
pub struct SigActionAccess<'a> {
    raw: NonNull<sigaction_t>,
    _m: PhantomData<&'a mut sigaction_t>,
}

impl<'a> SigActionAccess<'a> {
    #[inline(always)]
    pub unsafe fn from_raw(p: *mut sigaction_t) -> Option<Self> {
        NonNull::new(p).map(|n| Self { raw: n, _m: PhantomData })
    }
    #[inline(always)] pub fn from_ptr(p: *mut sigaction_t) -> Option<Self> { unsafe { Self::from_raw(p) } }
    #[inline(always)] pub fn assume(p: *mut sigaction_t) -> Self { unsafe { Self::from_raw(p).unwrap_unchecked() } }
    #[inline(always)] pub fn as_ptr(&self) -> *mut sigaction_t { self.raw.as_ptr() }
    #[inline] pub fn copy(&self) -> sigaction_t { unsafe { *self.raw.as_ptr() } }
    #[inline] pub fn copy_to(&self, out: *mut sigaction_t) { if !out.is_null() { unsafe { *out = *self.raw.as_ptr(); } } }
    #[inline] pub fn copy_from(&self, src: *const sigaction_t) { if !src.is_null() { unsafe { *self.raw.as_ptr() = *src; } } }
    #[inline] pub fn flags(&self) -> c_int { raw_get!(self, sa_flags) }
    #[inline] pub fn set_flags(&self, v: c_int) { raw_set!(self, sa_flags, v) }
    #[inline] pub fn mask(&self) -> u64 { raw_get!(self, sa_mask) }
    #[inline] pub fn set_mask(&self, v: u64) { raw_set!(self, sa_mask, v) }
    #[inline] pub fn and_mask(&self, v: u64) { self.set_mask(self.mask() & v); }
    #[inline] pub fn handler_addr(&self) -> usize {
        unsafe {
            match (*self.raw.as_ptr()).__bindgen_anon_1.sa_handler {
                Some(f) => f as *const () as usize,
                None => 0,
            }
        }
    }
    #[inline] pub fn is_default(&self) -> bool { self.handler_addr() == 0 }
    #[inline] pub fn is_ignore(&self) -> bool { self.handler_addr() == 1 }
    #[inline] pub fn has_user_handler(&self) -> bool { !self.is_default() && !self.is_ignore() }
    #[inline]
    pub fn clear_handler_and_metadata(&self) {
        raw_set!(self, __bindgen_anon_1.sa_handler, None);
        self.set_flags(0);
        self.set_mask(0);
    }
}

#[inline]
pub fn make_ksiginfo(signo: c_int, sender: *mut thread, si_pid: c_int) -> ksiginfo {
    let mut info: ksiginfo = zeroed();
    info.signo = signo;
    info.sender = sender;
    info.info.si_pid = si_pid;
    info
}

// =========================================================================
// KsigInfoAccess
// =========================================================================
#[derive(Clone, Copy)]
pub struct KsigInfoAccess<'a> {
    raw: NonNull<crate::bindings::ksiginfo>,
    _m: PhantomData<&'a mut crate::bindings::ksiginfo>,
}

impl<'a> KsigInfoAccess<'a> {
    #[inline(always)]
    pub unsafe fn from_raw(p: *mut crate::bindings::ksiginfo) -> Option<Self> {
        NonNull::new(p).map(|n| Self { raw: n, _m: PhantomData })
    }
    #[inline(always)] pub fn assume(p: *mut crate::bindings::ksiginfo) -> Self { unsafe { Self::from_raw(p).unwrap_unchecked() } }
    #[inline(always)]
    pub fn as_ptr(&self) -> *mut crate::bindings::ksiginfo { self.raw.as_ptr() }

    #[inline] pub fn signo(&self) -> c_int { shim_call!(self, ksi_signo) }
    #[inline] pub fn set_signo(&self, v: c_int) { shim_call!(self, ksi_set_signo, v) }
    #[inline] pub fn receiver(&self) -> *mut thread { shim_call!(self, ksi_receiver) }
    #[inline] pub fn set_receiver(&self, v: *mut thread) { shim_call!(self, ksi_set_receiver, v) }
    #[inline] pub fn sender(&self) -> *mut thread { shim_call!(self, ksi_sender) }
    #[inline] pub fn set_sender(&self, v: *mut thread) { shim_call!(self, ksi_set_sender, v) }
    #[inline] pub fn list_entry_ptr(&self) -> *mut list_node_t { raw_mut_ptr!(self, list_entry) }
    #[inline] pub fn list_entry_ref(&self) -> ListNodeRef<'a> { ListNodeRef::assume(self.list_entry_ptr()) }
    #[inline] pub fn copy_from(&self, src: *const crate::bindings::ksiginfo) {
        if !src.is_null() { unsafe { *self.raw.as_ptr() = *src; } }
    }
}

#[derive(Clone, Copy)]
pub struct SigPendingRef<'a> {
    raw: NonNull<crate::bindings::sigpending_t>,
    _m: PhantomData<&'a mut crate::bindings::sigpending_t>,
}

impl<'a> SigPendingRef<'a> {
    #[inline(always)]
    pub unsafe fn from_raw(p: *mut crate::bindings::sigpending_t) -> Option<Self> {
        NonNull::new(p).map(|n| Self { raw: n, _m: PhantomData })
    }
    #[inline(always)] pub fn assume(p: *mut crate::bindings::sigpending_t) -> Self { unsafe { Self::from_raw(p).unwrap_unchecked() } }
    #[inline(always)] pub fn as_ptr(&self) -> *mut crate::bindings::sigpending_t { self.raw.as_ptr() }
    #[inline] pub fn queue_ptr(&self) -> *mut list_node_t { raw_mut_ptr!(self, queue) }
    #[inline] pub fn queue_ref(&self) -> ListNodeRef<'a> { ListNodeRef::assume(self.queue_ptr()) }
    #[inline]
    pub fn queue_len(&self) -> c_int {
        let head = self.queue_ptr();
        let mut n = 0;
        $crate::list_for_each!(head, core::mem::offset_of!(crate::bindings::ksiginfo, list_entry), _node, {
            n += 1;
        });
        n
    }
}

#[derive(Clone, Copy)]
pub struct ThreadSignalPtrRef<'a> {
    raw: NonNull<crate::bindings::thread_signal_t>,
    _m: PhantomData<&'a mut crate::bindings::thread_signal_t>,
}

impl<'a> ThreadSignalPtrRef<'a> {
    #[inline(always)]
    pub unsafe fn from_raw(p: *mut crate::bindings::thread_signal_t) -> Option<Self> {
        NonNull::new(p).map(|n| Self { raw: n, _m: PhantomData })
    }
    #[inline(always)] pub fn assume(p: *mut crate::bindings::thread_signal_t) -> Self { unsafe { Self::from_raw(p).unwrap_unchecked() } }
    #[inline(always)] pub fn as_ptr(&self) -> *mut crate::bindings::thread_signal_t { self.raw.as_ptr() }
    #[inline] pub fn sig_mask(&self) -> u64 { raw_get!(self, sig_mask) }
    #[inline] pub fn set_sig_mask(&self, v: u64) { raw_set!(self, sig_mask, v) }
    #[inline] pub fn sig_saved_mask(&self) -> u64 { raw_get!(self, sig_saved_mask) }
    #[inline] pub fn set_sig_saved_mask(&self, v: u64) { raw_set!(self, sig_saved_mask, v) }
    #[inline] pub fn set_esignal(&self, v: u64) { raw_set!(self, esignal, v) }
    #[inline] pub fn sig_pending_mask(&self) -> u64 { raw_get!(self, sig_pending_mask) }
    #[inline] pub fn set_sig_pending_mask(&self, v: u64) { raw_set!(self, sig_pending_mask, v) }
    #[inline] pub fn sig_pending_mask_atomic_load(&self) -> u64 {
        let ap = raw_mut_ptr!(self, sig_pending_mask) as *mut AtomicU64;
        unsafe { (*ap).load(Ordering::Acquire) }
    }
    #[inline] pub fn and_sig_pending_mask(&self, mask: u64) { raw_bitand_assign!(self, sig_pending_mask, mask) }
    #[inline] pub fn or_sig_pending_mask(&self, mask: u64) { raw_bitor_assign!(self, sig_pending_mask, mask) }
    #[inline] pub fn sig_pending_ptr_index(&self, idx: usize) -> *mut crate::bindings::sigpending_t {
        raw_index_ptr_mut!(self, sig_pending, idx)
    }
    #[inline] pub fn sig_pending_ref_index(&self, idx: usize) -> SigPendingRef<'a> {
        SigPendingRef::assume(self.sig_pending_ptr_index(idx))
    }
}

#[derive(Clone, Copy)]
pub struct TgSharedPendingRef<'a> {
    raw: NonNull<crate::bindings::tg_shared_pending>,
    _m: PhantomData<&'a mut crate::bindings::tg_shared_pending>,
}

impl<'a> TgSharedPendingRef<'a> {
    #[inline(always)]
    pub unsafe fn from_raw(p: *mut crate::bindings::tg_shared_pending) -> Option<Self> {
        NonNull::new(p).map(|n| Self { raw: n, _m: PhantomData })
    }
    #[inline(always)] pub fn assume(p: *mut crate::bindings::tg_shared_pending) -> Self { unsafe { Self::from_raw(p).unwrap_unchecked() } }
    #[inline(always)] pub fn as_ptr(&self) -> *mut crate::bindings::tg_shared_pending { self.raw.as_ptr() }
    #[inline] pub fn sig_pending_mask(&self) -> u64 { raw_get!(self, sig_pending_mask) }
    #[inline] pub fn set_sig_pending_mask(&self, v: u64) { raw_set!(self, sig_pending_mask, v) }
    #[inline] pub fn sig_pending_mask_atomic_load(&self) -> u64 {
        let ap = raw_mut_ptr!(self, sig_pending_mask) as *mut AtomicU64;
        unsafe { (*ap).load(Ordering::Acquire) }
    }
    #[inline] pub fn and_sig_pending_mask(&self, mask: u64) { raw_bitand_assign!(self, sig_pending_mask, mask) }
    #[inline] pub fn or_sig_pending_mask(&self, mask: u64) { raw_bitor_assign!(self, sig_pending_mask, mask) }
    #[inline] pub fn sig_pending_ptr_index(&self, idx: usize) -> *mut crate::bindings::sigpending_t {
        raw_index_ptr_mut!(self, sig_pending, idx)
    }
    #[inline] pub fn sig_pending_ref_index(&self, idx: usize) -> SigPendingRef<'a> {
        SigPendingRef::assume(self.sig_pending_ptr_index(idx))
    }
}

// =========================================================================
// ThreadSignalAccess — accesses the embedded thread_signal sub-struct via
// the owning thread pointer.
// =========================================================================
#[derive(Clone, Copy)]
pub struct ThreadSignalAccess<'a> {
    raw: NonNull<thread>,
    _m: PhantomData<&'a mut thread>,
}

impl<'a> ThreadSignalAccess<'a> {
    #[inline(always)]
    pub unsafe fn from_thread(p: *mut thread) -> Option<Self> {
        NonNull::new(p).map(|n| Self { raw: n, _m: PhantomData })
    }
    #[inline(always)] pub fn assume_thread(p: *mut thread) -> Self { unsafe { Self::from_thread(p).unwrap_unchecked() } }
    #[inline(always)]
    pub fn thread_ptr(&self) -> *mut thread { self.raw.as_ptr() }

    #[inline] pub fn sig_mask(&self) -> u64 { shim_call!(self, ts_sig_mask) }
    #[inline] pub fn set_sig_mask(&self, v: u64) { shim_call!(self, ts_set_sig_mask, v) }
    #[inline] pub fn sig_saved_mask(&self) -> u64 { shim_call!(self, ts_sig_saved_mask) }
    #[inline] pub fn set_sig_saved_mask(&self, v: u64) { shim_call!(self, ts_set_sig_saved_mask, v) }
    #[inline] pub fn sig_pending_mask(&self) -> u64 { shim_call!(self, ts_sig_pending_mask) }
    #[inline] pub fn set_sig_pending_mask(&self, v: u64) { shim_call!(self, ts_set_sig_pending_mask, v) }
    #[inline] pub fn sig_stack(&self) -> crate::bindings::stack { raw_get!(self, signal.sig_stack) }
    #[inline] pub fn set_sig_stack(&self, v: crate::bindings::stack) { raw_set!(self, signal.sig_stack, v) }
    #[inline] pub fn sig_pending_mask_atomic_load(&self) -> u64 {
        let p = self.raw.as_ptr();
        let ap = unsafe { &raw const (*p).signal.sig_pending_mask } as *const AtomicU64;
        unsafe { (*ap).load(Ordering::Acquire) }
    }
    #[inline] pub fn sig_ucontext(&self) -> u64 { shim_call!(self, ts_sig_ucontext) }
    #[inline] pub fn set_sig_ucontext(&self, v: u64) { shim_call!(self, ts_set_sig_ucontext, v) }
    #[inline] pub fn esignal(&self) -> u64 { shim_call!(self, ts_esignal) }
    #[inline] pub fn set_esignal(&self, v: u64) { shim_call!(self, ts_set_esignal, v) }
    #[inline] pub fn stop_signal(&self) -> c_int { shim_call!(self, ts_stop_signal) }
    #[inline] pub fn set_stop_signal(&self, v: c_int) { shim_call!(self, ts_set_stop_signal, v) }
    #[inline] pub fn ptr_ref(&self) -> ThreadSignalPtrRef<'a> {
        let p = raw_mut_ptr!(self, signal);
        ThreadSignalPtrRef::assume(p)
    }
    #[inline] pub fn sig_pending_ref_index(&self, idx: usize) -> SigPendingRef<'a> {
        self.ptr_ref().sig_pending_ref_index(idx)
    }
    #[inline] pub fn and_sig_pending_mask(&self, mask: u64) { self.ptr_ref().and_sig_pending_mask(mask); }
    #[inline] pub fn or_sig_pending_mask(&self, mask: u64) { self.ptr_ref().or_sig_pending_mask(mask); }
}

// =========================================================================
// list_node_t — intrusive doubly-linked list node
// =========================================================================
use crate::bindings::list_node_t;

#[derive(Clone, Copy)]
pub struct ListNodeRef<'a> {
    raw: NonNull<list_node_t>,
    _m: PhantomData<&'a mut list_node_t>,
}
impl<'a> ListNodeRef<'a> {
    #[inline(always)]
    pub unsafe fn from_raw(p: *mut list_node_t) -> Option<Self> {
        NonNull::new(p).map(|n| Self { raw: n, _m: PhantomData })
    }
    #[inline(always)]
    pub fn from_ptr(p: *mut list_node_t) -> Option<Self> { unsafe { Self::from_raw(p) } }
    #[inline(always)]
    pub fn assume(p: *mut list_node_t) -> Self { unsafe { Self::from_raw(p).unwrap_unchecked() } }
    #[inline(always)]
    pub fn as_ptr(&self) -> *mut list_node_t { self.raw.as_ptr() }
    #[inline] pub fn next_ptr(&self) -> *mut list_node_t { raw_get!(self, next) }
    #[inline] pub fn prev_ptr(&self) -> *mut list_node_t { raw_get!(self, prev) }
    #[inline] pub fn is_detached(&self) -> bool { self.next_ptr() == self.raw.as_ptr() }
    #[inline] pub fn is_empty(&self) -> bool { self.next_ptr() == self.raw.as_ptr() }
    #[inline]
    pub fn init(&self) {
        let p = self.raw.as_ptr();
        unsafe { (*p).next = p; (*p).prev = p; }
    }
    #[inline]
    pub fn detach(&self) {
        let n = self.raw.as_ptr();
        unsafe {
            let p = (*n).prev; let nx = (*n).next;
            (*p).next = nx; (*nx).prev = p;
            (*n).next = n; (*n).prev = n;
        }
    }
    #[inline]
    pub fn insert_after(&self, prev: *mut list_node_t) {
        let entry = self.raw.as_ptr();
        unsafe {
            let next = (*prev).next;
            (*entry).prev = prev;
            (*entry).next = next;
            (*next).prev = entry;
            (*prev).next = entry;
        }
    }
    #[inline]
    pub fn push_back(&self, head: *mut list_node_t) {
        let tail = unsafe { (*head).prev };
        self.insert_after(tail);
    }
    #[inline]
    pub fn push_front(&self, head: *mut list_node_t) { self.insert_after(head); }
    #[inline]
    pub fn insert_bulk_from_head(&self, src_head: *mut list_node_t) {
        let prev = self.raw.as_ptr();
        unsafe {
            if (*src_head).next == src_head { return; }
            let src_first = (*src_head).next;
            let src_last = (*src_head).prev;
            (*src_first).prev = prev;
            (*src_last).next = (*prev).next;
            (*(*prev).next).prev = src_last;
            (*prev).next = src_first;
            (*src_head).next = src_head; (*src_head).prev = src_head;
        }
    }
}

#[inline]
pub fn list_node_init_raw(p: *mut list_node_t) {
    if let Some(r) = unsafe { ListNodeRef::from_raw(p) } { r.init(); }
}
#[inline]
pub fn list_node_is_detached_raw(p: *mut list_node_t) -> bool {
    match unsafe { ListNodeRef::from_raw(p) } {
        Some(r) => r.is_detached(),
        None => true,
    }
}
#[inline]
pub fn list_node_is_empty_raw(p: *mut list_node_t) -> bool {
    match unsafe { ListNodeRef::from_raw(p) } {
        Some(r) => r.is_empty(),
        None => true,
    }
}
#[inline]
pub fn list_node_next_raw(p: *mut list_node_t) -> *mut list_node_t {
    unsafe { (*p).next }
}
#[inline]
pub fn list_container_of<T>(n: *mut list_node_t, member_off: usize) -> *mut T {
    (n as *mut u8).wrapping_sub(member_off) as *mut T
}
#[inline]
pub fn list_node_detach_raw(p: *mut list_node_t) {
    if let Some(r) = unsafe { ListNodeRef::from_raw(p) } { r.detach(); }
}
#[inline]
pub fn list_node_push_front_raw(head: *mut list_node_t, n: *mut list_node_t) {
    if let Some(r) = unsafe { ListNodeRef::from_raw(n) } { r.push_front(head); }
}
#[inline]
pub fn list_node_push_back_raw(head: *mut list_node_t, n: *mut list_node_t) {
    if let Some(r) = unsafe { ListNodeRef::from_raw(n) } { r.push_back(head); }
}
#[inline]
pub fn list_node_pop_back_raw(head: *mut list_node_t) -> *mut list_node_t {
    let last = unsafe { (*head).prev };
    if last == head { return core::ptr::null_mut(); }
    list_node_detach_raw(last);
    last
}

// =========================================================================
// tq_t / ttree_t / tnode_t / workqueue / work_struct
// =========================================================================
use crate::bindings::{
    rb_node, rb_root, tnode as tnode_t, tq as tq_t, ttree as ttree_t,
    work_struct, workqueue, workqueue_callbacks,
};

const TYPE_NONE_U32: u32 = 0;
const TYPE_LIST_U32: u32 = 1;
const TYPE_TREE_U32: u32 = 2;

#[inline]
pub fn tnode_entry_offset() -> usize {
    core::mem::offset_of!(tnode_t, __bindgen_anon_1)
}

#[derive(Clone, Copy)]
pub struct TqRef<'a> {
    raw: NonNull<tq_t>,
    _m: PhantomData<&'a mut tq_t>,
}
impl<'a> TqRef<'a> {
    #[inline(always)]
    pub unsafe fn from_raw(p: *mut tq_t) -> Option<Self> {
        NonNull::new(p).map(|n| Self { raw: n, _m: PhantomData })
    }
    #[inline(always)]
    pub fn from_ptr(p: *mut tq_t) -> Option<Self> { unsafe { Self::from_raw(p) } }
    #[inline(always)]
    pub fn as_ptr(&self) -> *mut tq_t { self.raw.as_ptr() }
    #[inline] pub fn head_ptr(&self) -> *mut list_node_t { raw_mut_ptr!(self, head) }
    #[inline] pub fn head_ref(&self) -> ListNodeRef<'a> {
        unsafe { ListNodeRef::from_raw(self.head_ptr()).unwrap_unchecked() }
    }
    #[inline] pub fn counter(&self) -> c_int { raw_get!(self, counter) }
    #[inline] pub fn set_counter(&self, v: c_int) { raw_set!(self, counter, v) }
    #[inline] pub fn inc_counter(&self) { raw_add_assign!(self, counter, 1) }
    #[inline] pub fn dec_counter(&self) { raw_sub_assign!(self, counter, 1) }
    #[inline] pub fn add_counter(&self, v: c_int) { raw_add_assign!(self, counter, v) }
    #[inline] pub fn set_name(&self, n: *const core::ffi::c_char) { raw_set!(self, name, n) }
    #[inline] pub fn set_lock_ptr(&self, l: *mut spinlock_t) { raw_set!(self, lock, l) }
}

#[derive(Clone, Copy)]
pub struct TtreeRef<'a> {
    raw: NonNull<ttree_t>,
    _m: PhantomData<&'a mut ttree_t>,
}
impl<'a> TtreeRef<'a> {
    #[inline(always)]
    pub unsafe fn from_raw(p: *mut ttree_t) -> Option<Self> {
        NonNull::new(p).map(|n| Self { raw: n, _m: PhantomData })
    }
    #[inline(always)]
    pub fn from_ptr(p: *mut ttree_t) -> Option<Self> { unsafe { Self::from_raw(p) } }
    #[inline(always)]
    pub fn as_ptr(&self) -> *mut ttree_t { self.raw.as_ptr() }
    #[inline] pub fn root_ptr(&self) -> *mut rb_root { raw_mut_ptr!(self, root) }
    #[inline] pub fn root_copy(&self) -> rb_root { raw_get!(self, root) }
    #[inline] pub fn set_root_node(&self, n: *mut rb_node) { raw_set!(self, root.node, n) }
    #[inline] pub fn set_root_opts(&self, o: *mut crate::bindings::rb_root_opts) { raw_set!(self, root.opts, o) }
    #[inline] pub fn counter(&self) -> c_int { raw_get!(self, counter) }
    #[inline] pub fn set_counter(&self, v: c_int) { raw_set!(self, counter, v) }
    #[inline] pub fn inc_counter(&self) { raw_add_assign!(self, counter, 1) }
    #[inline] pub fn dec_counter(&self) { raw_sub_assign!(self, counter, 1) }
    #[inline] pub fn set_name(&self, n: *const core::ffi::c_char) { raw_set!(self, name, n) }
    #[inline] pub fn set_lock_ptr(&self, l: *mut spinlock_t) { raw_set!(self, lock, l) }
}

#[derive(Clone, Copy)]
pub struct TnodeRef<'a> {
    raw: NonNull<tnode_t>,
    _m: PhantomData<&'a mut tnode_t>,
}
impl<'a> TnodeRef<'a> {
    #[inline(always)]
    pub unsafe fn from_raw(p: *mut tnode_t) -> Option<Self> {
        NonNull::new(p).map(|n| Self { raw: n, _m: PhantomData })
    }
    #[inline(always)]
    pub fn from_ptr(p: *mut tnode_t) -> Option<Self> { unsafe { Self::from_raw(p) } }
    #[inline(always)]
    pub fn as_ptr(&self) -> *mut tnode_t { self.raw.as_ptr() }
    #[inline] pub fn type_get(&self) -> u32 { raw_get!(self, type_) }
    #[inline] pub fn set_type(&self, t: u32) { raw_set!(self, type_, t) }
    #[inline] pub fn thread_ptr(&self) -> *mut thread { raw_get!(self, thread) }
    #[inline] pub fn set_thread_ptr(&self, t: *mut thread) { raw_set!(self, thread, t) }
    #[inline] pub fn error_no(&self) -> c_int { raw_get!(self, error_no) }
    #[inline] pub fn set_error_no(&self, v: c_int) { raw_set!(self, error_no, v) }
    #[inline] pub fn data(&self) -> u64 { raw_get!(self, data) }
    #[inline] pub fn set_data(&self, v: u64) { raw_set!(self, data, v) }
    #[inline] pub fn list_entry_ptr(&self) -> *mut list_node_t {
        raw_mut_ptr!(self, __bindgen_anon_1.list.entry)
    }
    #[inline] pub fn list_entry_ref(&self) -> ListNodeRef<'a> {
        unsafe { ListNodeRef::from_raw(self.list_entry_ptr()).unwrap_unchecked() }
    }
    #[inline] pub fn tree_entry_ptr(&self) -> *mut rb_node {
        raw_mut_ptr!(self, __bindgen_anon_1.tree.entry)
    }
    #[inline] pub fn list_queue_ptr(&self) -> *mut tq_t {
        raw_get!(self, __bindgen_anon_1.list.queue)
    }
    #[inline] pub fn set_list_queue(&self, q: *mut tq_t) {
        raw_set!(self, __bindgen_anon_1.list.queue, q)
    }
    #[inline] pub fn tree_queue_ptr(&self) -> *mut ttree_t {
        raw_get!(self, __bindgen_anon_1.tree.queue)
    }
    #[inline] pub fn set_tree_queue(&self, q: *mut ttree_t) {
        raw_set!(self, __bindgen_anon_1.tree.queue, q)
    }
    #[inline] pub fn tree_key(&self) -> u64 {
        raw_get!(self, __bindgen_anon_1.tree.key)
    }
    #[inline] pub fn set_tree_key(&self, k: u64) {
        raw_set!(self, __bindgen_anon_1.tree.key, k)
    }
    #[inline] pub fn is_enqueued(&self) -> bool {
        match self.type_get() {
            TYPE_LIST_U32 => !self.list_queue_ptr().is_null(),
            TYPE_TREE_U32 => !self.tree_queue_ptr().is_null(),
            _ => false,
        }
    }
    #[inline]
    pub fn to_none(&self) { self.set_type(TYPE_NONE_U32); }
    #[inline]
    pub fn to_list(&self) {
        self.set_type(TYPE_LIST_U32);
        self.list_entry_ref().init();
        self.set_list_queue(core::ptr::null_mut());
    }
    #[inline]
    pub fn to_tree(&self) {
        self.set_type(TYPE_TREE_U32);
        let entry = self.tree_entry_ptr();
        unsafe { (*entry).__parent_color = entry as u64; }
        self.set_tree_queue(core::ptr::null_mut());
    }
}

#[inline]
pub fn zero_tnode_ptr(p: *mut tnode_t) {
    if !p.is_null() {
        unsafe { core::ptr::write_bytes(p, 0, 1); }
    }
}

#[inline]
pub fn zeroed_tnode() -> tnode_t {
    unsafe { core::mem::zeroed() }
}

#[inline]
pub fn tnode_from_list_entry(entry: *mut list_node_t) -> *mut tnode_t {
    (entry as *mut u8).wrapping_sub(tnode_entry_offset()) as *mut tnode_t
}
#[inline]
pub fn tnode_from_tree_entry(entry: *mut rb_node) -> *mut tnode_t {
    (entry as *mut u8).wrapping_sub(tnode_entry_offset()) as *mut tnode_t
}

#[derive(Clone, Copy)]
pub struct WorkqueueRef<'a> {
    raw: NonNull<workqueue>,
    _m: PhantomData<&'a mut workqueue>,
}
impl<'a> WorkqueueRef<'a> {
    #[inline(always)]
    pub unsafe fn from_raw(p: *mut workqueue) -> Option<Self> {
        NonNull::new(p).map(|n| Self { raw: n, _m: PhantomData })
    }
    #[inline(always)]
    pub fn from_ptr(p: *mut workqueue) -> Option<Self> { unsafe { Self::from_raw(p) } }
    #[inline(always)]
    pub fn as_ptr(&self) -> *mut workqueue { self.raw.as_ptr() }

    #[inline] pub fn lock_ptr(&self) -> *mut spinlock_t { raw_mut_ptr!(self, lock) }
    #[inline] pub fn lock_ref(&self) -> SpinLockRef<'a> {
        unsafe { SpinLockRef::from_raw(self.lock_ptr()).unwrap_unchecked() }
    }
    #[inline] pub fn idle_queue_ptr(&self) -> *mut tq_t { raw_mut_ptr!(self, idle_queue) }
    #[inline] pub fn idle_queue_ref(&self) -> TqRef<'a> {
        unsafe { TqRef::from_raw(self.idle_queue_ptr()).unwrap_unchecked() }
    }
    #[inline] pub fn worker_list_ptr(&self) -> *mut list_node_t {
        raw_mut_ptr!(self, worker_list)
    }
    #[inline] pub fn work_list_ptr(&self) -> *mut list_node_t {
        raw_mut_ptr!(self, work_list)
    }
    #[inline] pub fn name_ptr(&self) -> *const core::ffi::c_char {
        raw_method!(self, name.as_ptr())
    }
    #[inline] pub fn name_buf_ptr(&self) -> *mut core::ffi::c_char {
        raw_method!(self, name.as_mut_ptr())
    }
    #[inline] pub fn manager_ptr(&self) -> *mut thread { raw_get!(self, manager) }
    #[inline] pub fn set_manager(&self, m: *mut thread) { raw_set!(self, manager, m) }
    #[inline] pub fn nr_workers(&self) -> c_int { raw_get!(self, nr_workers) }
    #[inline] pub fn set_nr_workers(&self, v: c_int) { raw_set!(self, nr_workers, v) }
    #[inline] pub fn inc_nr_workers(&self) { raw_add_assign!(self, nr_workers, 1) }
    #[inline] pub fn dec_nr_workers(&self) { raw_sub_assign!(self, nr_workers, 1) }
    #[inline] pub fn pending_works(&self) -> c_int { raw_get!(self, pending_works) }
    #[inline] pub fn inc_pending_works(&self) { raw_add_assign!(self, pending_works, 1) }
    #[inline] pub fn dec_pending_works(&self) { raw_sub_assign!(self, pending_works, 1) }
    #[inline] pub fn min_active(&self) -> c_int { raw_get!(self, min_active) }
    #[inline] pub fn max_active(&self) -> c_int { raw_get!(self, max_active) }
    #[inline] pub fn set_min_active(&self, v: c_int) { raw_set!(self, min_active, v) }
    #[inline] pub fn set_max_active(&self, v: c_int) { raw_set!(self, max_active, v) }
    #[inline] pub fn callbacks_ptr(&self) -> *mut workqueue_callbacks {
        raw_mut_ptr!(self, callbacks)
    }
    #[inline] pub fn copy_callbacks(&self, src: *const workqueue_callbacks) {
        if !src.is_null() { raw_set!(self, callbacks, *src) }
    }
    #[inline] pub fn cb_workqueue_ctor(&self) -> Option<unsafe extern "C" fn(*mut workqueue)> {
        raw_get!(self, callbacks.workqueue_ctor)
    }
    #[inline] pub fn cb_workqueue_dtor(&self) -> Option<unsafe extern "C" fn(*mut workqueue)> {
        raw_get!(self, callbacks.workqueue_dtor)
    }
    #[inline] pub fn cb_manager_ctor(&self) -> Option<unsafe extern "C" fn(*mut workqueue, *mut thread)> {
        raw_get!(self, callbacks.manager_ctor)
    }
    #[inline] pub fn cb_manager_dtor(&self) -> Option<unsafe extern "C" fn(*mut workqueue, *mut thread)> {
        raw_get!(self, callbacks.manager_dtor)
    }
    #[inline] pub fn cb_worker_ctor(&self) -> Option<unsafe extern "C" fn(*mut workqueue, *mut thread)> {
        raw_get!(self, callbacks.worker_ctor)
    }
    #[inline] pub fn cb_worker_dtor(&self) -> Option<unsafe extern "C" fn(*mut workqueue, *mut thread)> {
        raw_get!(self, callbacks.worker_dtor)
    }
    #[inline] pub fn is_active(&self) -> bool {
        bit_get!(self, __bindgen_anon_1, active) != 0
    }
    #[inline] pub fn set_active(&self, v: u64) {
        bit_set!(self, __bindgen_anon_1, set_active, v)
    }
    #[inline] pub fn dtor_called(&self) -> bool {
        bit_get!(self, __bindgen_anon_1, dtor_called) != 0
    }
    #[inline] pub fn set_dtor_called(&self, v: u64) {
        bit_set!(self, __bindgen_anon_1, set_dtor_called, v)
    }
}

#[derive(Clone, Copy)]
pub struct WorkStructRef<'a> {
    raw: NonNull<work_struct>,
    _m: PhantomData<&'a mut work_struct>,
}
impl<'a> WorkStructRef<'a> {
    #[inline(always)]
    pub unsafe fn from_raw(p: *mut work_struct) -> Option<Self> {
        NonNull::new(p).map(|n| Self { raw: n, _m: PhantomData })
    }
    #[inline(always)]
    pub fn from_ptr(p: *mut work_struct) -> Option<Self> { unsafe { Self::from_raw(p) } }
    #[inline(always)]
    pub fn as_ptr(&self) -> *mut work_struct { self.raw.as_ptr() }
    #[inline] pub fn entry_ptr(&self) -> *mut list_node_t { raw_mut_ptr!(self, entry) }
    #[inline] pub fn entry_ref(&self) -> ListNodeRef<'a> {
        unsafe { ListNodeRef::from_raw(self.entry_ptr()).unwrap_unchecked() }
    }
    #[inline] pub fn flags(&self) -> u32 { raw_get!(self, flags) }
    #[inline] pub fn set_flags(&self, v: u32) { raw_set!(self, flags, v) }
    #[inline] pub fn flag_set(&self, f: u32) -> bool { (self.flags() & f) != 0 }
    #[inline] pub fn data(&self) -> u64 { raw_get!(self, data) }
    #[inline] pub fn set_data(&self, v: u64) { raw_set!(self, data, v) }
    #[inline] pub fn func(&self) -> Option<unsafe extern "C" fn(*mut work_struct)> {
        raw_get!(self, func)
    }
    #[inline] pub fn fault(&self) -> Option<unsafe extern "C" fn(*mut work_struct)> {
        raw_get!(self, fault)
    }
    #[inline] pub fn set_func(&self, f: Option<unsafe extern "C" fn(*mut work_struct)>) {
        raw_set!(self, func, f)
    }
    #[inline] pub fn set_fault(&self, f: Option<unsafe extern "C" fn(*mut work_struct)>) {
        raw_set!(self, fault, f)
    }
}

// Thread fields used by workqueue.rs that aren't yet exposed on ThreadAccess.
impl<'a> ThreadAccess<'a> {
    #[inline] pub fn wq_ptr(&self) -> *mut workqueue { raw_get!(self, wq) }
    #[inline] pub fn set_wq_ptr(&self, v: *mut workqueue) { raw_set!(self, wq, v) }
    #[inline] pub fn wq_entry_ptr(&self) -> *mut list_node_t { raw_mut_ptr!(self, wq_entry) }
    #[inline] pub fn wq_entry_ref(&self) -> ListNodeRef<'a> {
        unsafe { ListNodeRef::from_raw(self.wq_entry_ptr()).unwrap_unchecked() }
    }
}
