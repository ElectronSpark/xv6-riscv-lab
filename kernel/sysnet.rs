//! UDP socket queues and I/O support.
//!
//! VFS delegates socket file operations here. Packet ownership, queue locking
//! and buffer validation are handled independently of the DMA boundary.

use core::ffi::{c_int, c_void};
use core::ptr;

use crate::kstd::{Errno, KResult};
use crate::mm::{either_copyin, either_copyout};
use crate::net::{MbufQueue, Net, Packet, MBUF_DEFAULT_HEADROOM};
use crate::proc::access::ThreadAccess;
use crate::proc::proc_shims::xv6_current_thread;
use crate::proc::Scheduler;
use crate::sync::SpinLock;

pub(crate) struct SysNet;

/// VFS's raw socket allocator uses the same layout. Endpoint fields are set
/// before publication and stay immutable until the socket is unlinked.
#[repr(C)]
pub struct Socket {
    pub next: *mut Socket,
    pub raddr: u32,
    pub lport: u16,
    pub rport: u16,
    pub inner: SpinLock<SockInner>,
}

#[repr(C)]
pub struct SockInner {
    pub rxq: MbufQueue,
}

pub(crate) struct SocketTable {
    pub head: *mut Socket,
}

// SAFETY: the list owns live socket nodes. The table lock serializes traversal,
// insertion and removal; a removed socket is freed only after releasing it.
unsafe impl Send for SocketTable {}

pub(crate) static SOCKETS: SpinLock<SocketTable> =
    SpinLock::new(c"socktbl", SocketTable { head: ptr::null_mut() });

impl SysNet {
    pub(crate) fn sockinit() {}

    /// Unlink and release a socket, including all queued packets.
    ///
    /// # Safety
    /// `socket` must be a live socket allocated by VFS with no concurrent file
    /// users. The socket table lock excludes receive delivery during removal.
    #[allow(dead_code)]
    pub(crate) unsafe fn sockclose(socket: *mut Socket) {
        {
            let mut table = SOCKETS.lock();
            // SAFETY: the locked table contains live socket nodes and the
            // caller owns socket. Link mutation is serialized by this guard.
            unsafe {
                let mut link = &raw mut table.head;
                while !(*link).is_null() {
                    if *link == socket {
                        *link = (*socket).next;
                        break;
                    }
                    link = &raw mut (*(*link)).next;
                }
            }
        }
        // SAFETY: the unlinked socket has no concurrent users or receivers.
        // Its owned queue's Drop releases every packet before the backing
        // page is returned to the allocator.
        unsafe { ptr::drop_in_place(socket) };
        // SAFETY: the socket has been dropped and this page is exclusively owned.
        unsafe { crate::mm::kalloc::Kmem::kfree(socket.cast()) };
    }

    /// Read one datagram, sleeping interruptibly while the queue is empty.
    ///
    /// # Safety
    /// `socket` must stay live for the call, on behalf of the current thread.
    /// If `user` is false, `address` must be valid for `count` kernel bytes.
    #[allow(dead_code)]
    pub(crate) unsafe fn sockread(socket: *mut Socket, address: u64, count: usize, user: bool) -> KResult<usize> {
        if count == 0 {
            return Ok(0);
        }
        let thread = xv6_current_thread();
        let killed = || ThreadAccess::from_ptr(thread).is_some_and(|thread| thread.killed() != 0);
        let packet = {
            // SAFETY: the caller keeps socket alive throughout this operation.
            let mut inner = unsafe { (*socket).inner.lock() };
            let channel = &raw mut inner.rxq as *mut c_void;
            while inner.rxq.is_empty() && !killed() {
                if inner.sleep_on_interruptible(channel) != 0 {
                    return Err(Errno::Intr);
                }
            }
            if killed() {
                return Err(Errno::Intr);
            }
            let packet = inner.rxq.pop().ok_or(Errno::Fault)?;
            packet
        };
        let bytes = &packet.as_ref()[..count.min(packet.len())];
        // The socket caller supplies a valid kernel destination when user is
        // false; otherwise either_copyout validates the user address space.
        if either_copyout(c_int::from(user), address, bytes.as_ptr().cast_mut().cast(), bytes.len() as u64) < 0 {
            return Err(Errno::Fault);
        }
        Ok(bytes.len())
    }

    /// Copy a user payload into a packet and send it to the configured peer.
    ///
    /// # Safety
    /// `socket` must stay live for the call, on behalf of the current thread.
    /// If `user` is false, `address` must be valid for `count` kernel bytes.
    #[allow(dead_code)]
    pub(crate) unsafe fn sockwrite(socket: *mut Socket, address: u64, count: usize, user: bool) -> KResult<usize> {
        // Validate before allocating or copying user memory: there is no IP
        // fragmentation support, and headroom occupies real packet storage.
        let capacity = Net::udp_payload_capacity().ok_or(Errno::NoDev)?;
        if count > capacity {
            return Err(Errno::MsgSize);
        }
        let mut packet = Packet::allocate(MBUF_DEFAULT_HEADROOM).ok_or(Errno::NoMem)?;
        let bytes = packet.append(count).ok_or(Errno::MsgSize)?;
        // The socket caller supplies a valid kernel source when user is false;
        // bytes is an exclusive initialized destination owned by packet.
        if either_copyin(bytes.as_mut_ptr().cast(), c_int::from(user), address, bytes.len() as u64) < 0 {
            return Err(Errno::Fault);
        }
        // SAFETY: the caller keeps the socket live; endpoints are immutable.
        let (address, local, remote) = unsafe { ((*socket).raddr, (*socket).lport, (*socket).rport) };
        Net::net_tx_udp(packet, address, local, remote);
        Ok(count)
    }

    /// Deliver an owned UDP payload to a matching socket, or drop it if none
    /// exists. Table and receive queue locks retain their established order.
    pub(crate) fn sockrecvudp(packet: Packet, address: u32, local: u16, remote: u16) {
        let table = SOCKETS.lock();
        let mut socket = table.head;
        // SAFETY: nodes remain live while the table lock prevents removal.
        while let Some(current) = unsafe { socket.as_ref() } {
            if current.raddr == address && current.lport == local && current.rport == remote {
                let mut inner = current.inner.lock();
                let channel = &raw mut inner.rxq as *mut c_void;
                inner.rxq.push(packet);
                Scheduler::wakeup_on_chan(channel);
                return;
            }
            socket = current.next;
        }
        // Release the table lock before Drop returns the unclaimed packet.
        drop(table);
    }
}
