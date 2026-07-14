//! Networking protocol support (IP, UDP, ARP, mbuf management) -- Rust
//! port of `kernel/net.c` (Phase 2 Wave 28, sub-wave B -- see
//! `docs/rustify/phase2_plan.md`; the final porting wave's network
//! drivers).
//!
//! Packet-header structs (`Eth`/`Ip`/`Udp`/`Arp`, mirroring `dev/net.h`'s
//! `struct eth`/`ip`/`udp`/`arp`) are hand-rolled `#[repr(C, packed)]`
//! types rather than real `bindgen` layouts -- unlike this wave's
//! hardware-DMA structs (`virtq_desc` &c.), these are never touched by
//! any device, only read/written by this file at `mbuf` buffer offsets
//! that are frequently **not** naturally aligned for the field types
//! involved (e.g. an IP header pushed right after a 14-byte Ethernet
//! header lands 14 bytes into the buffer, not a multiple of 4). The C
//! original relied on RISC-V/qemu tolerating unaligned ordinary loads/
//! stores for its two *non*-`packed` structs (`struct ip`/`struct udp`,
//! which happen to need no interior padding either way); dereferencing a
//! misaligned pointer through a non-`packed` Rust type is immediate UB
//! regardless of what the target CPU tolerates, so **every** header
//! struct here is marked `packed` (a strict superset of the C layout --
//! `Ip`/`Udp` had zero padding to begin with, so this changes no byte
//! offset, only Rust's alignment *assumption*, which must be 1 to make
//! field access through a misaligned `*mut` pointer sound). Field reads/
//! writes through a packed-struct pointer (`(*hdr).field = x` / `let v =
//! (*hdr).field;`) are safe, ordinary Rust; only *taking a reference* to
//! a packed field is restricted, and every site below that needs a
//! field's address (`memmove`'s src/dst) uses `&raw mut`/`&raw const`
//! (address-of, not reference formation -- always sound regardless of
//! alignment) instead. `struct dns`/`dns_question`/`dns_data`
//! (`dev/net.h`) are dropped: grepped zero call sites in `net.c` or
//! anywhere else in the tree -- dead header declarations, never used.
//!
//! `htons`/`htonl`/`ntohs`/`ntohl` (`dev/net.h`: `#define ntohs bswaps`
//! &c. -- RISC-V is little-endian, network order is big-endian, so all
//! four names are literally the same byte-swap, direction-agnostic) are
//! reimplemented via [`u16::swap_bytes`]/[`u32::swap_bytes`] rather than
//! the C macro's hand-rolled bit-shift formula -- identical result,
//! standard-library primitive. Every call site below keeps the exact C
//! name it originally used (including the two places the C itself calls
//! `htons`/`htonl` on a value being *read*, e.g. `net_rx_ip`'s `htons(
//! iphdr->ip_off)` -- not "corrected" to `ntohs`, since they're the same
//! function and the C's naming choice is preserved for diffability).

#![allow(non_camel_case_types, non_snake_case, non_upper_case_globals)]

use core::ffi::{c_char, c_int, c_uint, c_void};
use core::ptr;

use crate::bindings::mbuf;

// ---------------------------------------------------------------------------
// Externs -- local per-file `unsafe extern "C"` block (this crate's
// established cross-module convention).
// ---------------------------------------------------------------------------
unsafe extern "C" {
    safe fn __panic_start();
    safe fn __panic_end() -> !;
    fn printf(fmt: *const c_char, ...) -> c_int;

    // mm/kalloc.rs, string.rs.
    fn kalloc() -> *mut c_void;
    fn kfree(pa: *mut c_void);
    fn memset(dst: *mut c_void, c: c_int, n: usize) -> *mut c_void;
    fn memmove(dst: *mut c_void, src: *const c_void, n: usize) -> *mut c_void;

}
// P3-1D mesh sweep: dev/netdev.rs/sysnet.rs are in scope for this wave;
// signatures are identical, so these become plain crate-path imports
// instead of `extern "C"` redeclarations.
use crate::dev::netdev::netdev_get_default;
use crate::sysnet::sockrecvudp;

// ===========================================================================
// Constants -- redeclared locally from `kernel/inc/dev/net.h`.
// ===========================================================================

const MBUF_SIZE: c_uint = 2048;
const MBUF_DEFAULT_HEADROOM: c_uint = 128;
const ETHADDR_LEN: usize = 6;
const ETHTYPE_IP: u16 = 0x0800;
const ETHTYPE_ARP: u16 = 0x0806;
const IPPROTO_UDP: u8 = 17;
const ARP_HRD_ETHER: u16 = 1;
const ARP_OP_REQUEST: u16 = 1;
const ARP_OP_REPLY: u16 = 2;

#[inline(always)]
const fn make_ip_addr(a: u32, b: u32, c: u32, d: u32) -> u32 {
    (a << 24) | (b << 16) | (c << 8) | d
}

#[inline(always)]
fn htons(x: u16) -> u16 {
    x.swap_bytes()
}
#[inline(always)]
fn ntohs(x: u16) -> u16 {
    x.swap_bytes()
}
#[inline(always)]
fn htonl(x: u32) -> u32 {
    x.swap_bytes()
}
#[inline(always)]
fn ntohl(x: u32) -> u32 {
    x.swap_bytes()
}

// ===========================================================================
// Packet headers -- see module doc for why every one is `packed`.
// ===========================================================================

/// An Ethernet packet header (start of the packet).
#[repr(C, packed)]
struct Eth {
    dhost: [u8; ETHADDR_LEN],
    shost: [u8; ETHADDR_LEN],
    type_: u16,
}

/// An IP packet header (comes after an Ethernet header).
#[repr(C, packed)]
struct Ip {
    ip_vhl: u8, // version << 4 | header length >> 2
    ip_tos: u8,
    ip_len: u16,
    ip_id: u16,
    ip_off: u16,
    ip_ttl: u8,
    ip_p: u8,
    ip_sum: u16,
    ip_src: u32,
    ip_dst: u32,
}

/// A UDP packet header (comes after an IP header).
#[repr(C, packed)]
struct Udp {
    sport: u16,
    dport: u16,
    ulen: u16,
    sum: u16,
}

/// An ARP packet (comes after an Ethernet header).
#[repr(C, packed)]
struct Arp {
    hrd: u16,
    pro: u16,
    hln: u8,
    pln: u8,
    op: u16,
    sha: [u8; ETHADDR_LEN],
    sip: u32,
    tha: [u8; ETHADDR_LEN],
    tip: u32,
}

// ===========================================================================
// Globals.
// ===========================================================================

static LOCAL_IP: u32 = make_ip_addr(10, 0, 2, 15); // qemu's idea of the guest IP
static LOCAL_MAC: [u8; ETHADDR_LEN] = [0x52, 0x54, 0x00, 0x12, 0x34, 0x56];
static BROADCAST_MAC: [u8; ETHADDR_LEN] = [0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF];

#[cold]
#[inline(never)]
fn panic_fixed(msg: &core::ffi::CStr) -> ! {
    __panic_start();
    // SAFETY: `msg` is 'static, nul-terminated, no format args.
    unsafe { printf(msg.as_ptr()) };
    __panic_end()
}

// ===========================================================================
// mbuf management -- exported (`kernel/inc/dev/net.h`).
// ===========================================================================

/// Strips data from the start of the buffer and returns a pointer to it.
/// Returns null if less than the full requested length is available.
///
/// # Safety
/// `m` must be live.
// P3-1D mesh sweep: no caller anywhere outside this file -- demoted.
pub(crate) unsafe extern "C" fn mbufpull(m: *mut mbuf, len: c_uint) -> *mut c_char {
    // SAFETY: caller contract.
    unsafe {
        let tmp = (*m).head;
        if (*m).len < len {
            return ptr::null_mut();
        }
        (*m).len -= len;
        (*m).head = (*m).head.add(len as usize);
        tmp
    }
}

/// Prepends data to the beginning of the buffer and returns a pointer to it.
///
/// # Safety
/// `m` must be live.
// P3-1D mesh sweep: no caller anywhere outside this file -- demoted.
pub(crate) unsafe extern "C" fn mbufpush(m: *mut mbuf, len: c_uint) -> *mut c_char {
    // SAFETY: caller contract.
    unsafe {
        (*m).head = (*m).head.sub(len as usize);
        if (*m).head < (*m).buf.as_mut_ptr() {
            panic_fixed(c"mbufpush");
        }
        (*m).len += len;
        (*m).head
    }
}

/// Appends data to the end of the buffer and returns a pointer to it.
///
/// # Safety
/// `m` must be live.
// P3-1D mesh sweep: no caller anywhere outside this file -- demoted.
pub(crate) unsafe extern "C" fn mbufput(m: *mut mbuf, len: c_uint) -> *mut c_char {
    // SAFETY: caller contract.
    unsafe {
        let tmp = (*m).head.add((*m).len as usize);
        (*m).len += len;
        if (*m).len > MBUF_SIZE {
            panic_fixed(c"mbufput");
        }
        tmp
    }
}

/// Strips data from the end of the buffer and returns a pointer to it.
/// Returns null if less than the full requested length is available.
///
/// # Safety
/// `m` must be live.
// P3-1D mesh sweep: no caller anywhere outside this file -- demoted.
pub(crate) unsafe extern "C" fn mbuftrim(m: *mut mbuf, len: c_uint) -> *mut c_char {
    // SAFETY: caller contract.
    unsafe {
        if len > (*m).len {
            return ptr::null_mut();
        }
        (*m).len -= len;
        (*m).head.add((*m).len as usize)
    }
}

/// Allocates a packet buffer. Returns null on allocation failure or if
/// `headroom` exceeds the buffer size.
// P3-1D mesh sweep: callers (`e1000.rs`, `sysnet.rs`, `dev/x1_emac.rs`)
// now import this via crate-path `use` instead of an `extern`
// redeclaration -- demoted.
pub(crate) extern "C" fn mbufalloc(headroom: c_uint) -> *mut mbuf {
    if headroom > MBUF_SIZE {
        return ptr::null_mut();
    }
    // SAFETY: `kalloc()` returns a fresh, exclusively-owned page (or
    // null, checked below); `size_of::<mbuf>()` (`2048 + 16`-ish bytes)
    // fits in one page, matching the C original's assumption.
    let m = unsafe { kalloc() } as *mut mbuf;
    if m.is_null() {
        return ptr::null_mut();
    }
    // SAFETY: `m` freshly allocated, exclusively owned.
    unsafe {
        (*m).next = ptr::null_mut();
        (*m).head = (*m).buf.as_mut_ptr().add(headroom as usize);
        (*m).len = 0;
        memset((*m).buf.as_mut_ptr() as *mut c_void, 0, core::mem::size_of_val(&(*m).buf));
    }
    m
}

/// Frees a packet buffer.
///
/// # Safety
/// `m` must be a live `mbuf` previously returned by [`mbufalloc`], not
/// freed since.
// P3-1D mesh sweep: callers (`e1000.rs`, `sysnet.rs`, `dev/x1_emac.rs`)
// now import this via crate-path `use` instead of an `extern`
// redeclaration -- demoted.
pub(crate) unsafe extern "C" fn mbuffree(m: *mut mbuf) {
    // SAFETY: caller contract.
    unsafe { kfree(m as *mut c_void) };
}

/// Mirrors `struct mbufq` (`kernel/inc/dev/net.h`) -- real definition,
/// this file being `mbufq_*`'s owning translation unit (unlike
/// `kernel/vfs/file.rs`'s pre-existing local opaque-sized mirror, kept
/// unchanged/out of this wave's touch scope).
#[repr(C)]
pub struct mbufq {
    pub head: *mut mbuf,
    pub tail: *mut mbuf,
}

/// Pushes an mbuf to the end of the queue.
///
/// # Safety
/// `q` must be live; `m` must be a live, exclusively-owned `mbuf`.
// P3-1D mesh sweep: caller (`sysnet.rs`) now imports this via crate-path
// `use` instead of an `extern` redeclaration -- demoted.
pub(crate) unsafe extern "C" fn mbufq_pushtail(q: *mut mbufq, m: *mut mbuf) {
    // SAFETY: caller contract.
    unsafe {
        (*m).next = ptr::null_mut();
        if (*q).head.is_null() {
            (*q).head = m;
            (*q).tail = m;
            return;
        }
        (*(*q).tail).next = m;
        (*q).tail = m;
    }
}

/// Pops an mbuf from the start of the queue. Returns null if empty.
///
/// # Safety
/// `q` must be live.
// P3-1D mesh sweep: caller (`sysnet.rs`) now imports this via crate-path
// `use` instead of an `extern` redeclaration -- demoted.
pub(crate) unsafe extern "C" fn mbufq_pophead(q: *mut mbufq) -> *mut mbuf {
    // SAFETY: caller contract.
    unsafe {
        let head = (*q).head;
        if head.is_null() {
            return ptr::null_mut();
        }
        (*q).head = (*head).next;
        head
    }
}

/// Returns nonzero if the queue is empty.
///
/// # Safety
/// `q` must be live.
// P3-1D mesh sweep: caller (`sysnet.rs`) now imports this via crate-path
// `use` instead of an `extern` redeclaration -- demoted.
pub(crate) unsafe extern "C" fn mbufq_empty(q: *mut mbufq) -> c_int {
    // SAFETY: caller contract.
    (unsafe { (*q).head }.is_null()) as c_int
}

/// Initializes a queue of mbufs.
///
/// # Safety
/// `q` must be live.
// P3-1D mesh sweep: callers (`vfs/file.rs`, `sysnet.rs`) now import this
// via crate-path `use` instead of an `extern` redeclaration -- demoted.
pub(crate) unsafe extern "C" fn mbufq_init(q: *mut mbufq) {
    // SAFETY: caller contract.
    unsafe { (*q).head = ptr::null_mut() };
}

// ===========================================================================
// Checksum.
// ===========================================================================

/// This code is lifted from FreeBSD's ping.c, and is copyright by the
/// Regents of the University of California.
///
/// # Safety
/// `addr` must be valid for `len` bytes.
unsafe fn in_cksum(addr: *const u8, len: i32) -> u16 {
    let mut nleft = len;
    let mut w = addr as *const u16;
    let mut sum: u32 = 0;
    let mut answer: u16;

    // Our algorithm is simple: using a 32-bit accumulator (sum), add
    // sequential 16-bit words to it, and at the end fold back all the
    // carry bits from the top 16 bits into the lower 16 bits.
    while nleft > 1 {
        // SAFETY: caller contract; `w` stays within `[addr, addr+len)`
        // while `nleft > 1`; unaligned read matches the C's `*(const
        // unsigned short *)addr` cast (may not be 2-byte aligned).
        sum = sum.wrapping_add(unsafe { core::ptr::read_unaligned(w) } as u32);
        w = unsafe { w.add(1) };
        nleft -= 2;
    }

    // Mop up an odd byte, if necessary.
    if nleft == 1 {
        // SAFETY: caller contract; exactly one byte remains in range.
        let last_byte = unsafe { core::ptr::read_unaligned(w as *const u8) };
        answer = last_byte as u16;
        sum = sum.wrapping_add(answer as u32);
    }

    // Add back carry-outs from the top 16 bits to the low 16 bits.
    sum = (sum & 0xffff) + (sum >> 16);
    sum += sum >> 16;
    // Guaranteed now that the lower 16 bits of sum are correct.

    answer = !(sum as u16); // truncate to 16 bits
    answer
}

// ===========================================================================
// Transmit path.
// ===========================================================================

/// Sends an ethernet packet.
fn net_tx_eth(m: *mut mbuf, ethtype: u16) {
    // SAFETY: `m` caller-owned/live; `mbufpush` only returns null via
    // its own panic path (never actually returns for this file's fixed,
    // in-bounds header sizes), so `ethhdr` is always valid here.
    let ethhdr = unsafe { mbufpush(m, core::mem::size_of::<Eth>() as c_uint) as *mut Eth };
    let ndev = netdev_get_default();

    // SAFETY: `ethhdr` live (see above); `ndev` checked for null.
    unsafe {
        if !ndev.is_null() {
            memmove((&raw mut (*ethhdr).shost) as *mut c_void, (&raw const (*ndev).mac) as *const c_void, ETHADDR_LEN);
        } else {
            memmove((&raw mut (*ethhdr).shost) as *mut c_void, LOCAL_MAC.as_ptr() as *const c_void, ETHADDR_LEN);
        }
        // In a real networking stack, dhost would be set to the address
        // discovered through ARP. Because we don't support enough of
        // the ARP protocol, set it to broadcast instead.
        memmove((&raw mut (*ethhdr).dhost) as *mut c_void, BROADCAST_MAC.as_ptr() as *const c_void, ETHADDR_LEN);
        (*ethhdr).type_ = htons(ethtype);
    }

    let should_free = if ndev.is_null() {
        true
    } else {
        // SAFETY: `ndev` non-null; `netdev_register` (`dev/netdev.rs`)
        // guarantees `.ops` non-null and `.ops.transmit` is `Some`
        // before publishing any netdev.
        unsafe { ((*(*ndev).ops).transmit.unwrap())(ndev, m) != 0 }
    };
    if should_free {
        // SAFETY: `m` still caller-owned here (no netdev, or transmit
        // failed) -- matches the C's `mbuffree(m)`.
        unsafe { mbuffree(m) };
    }
}

/// Sends an IP packet.
fn net_tx_ip(m: *mut mbuf, proto: u8, dip: u32) {
    // SAFETY: `m` live; `mbufpush` always succeeds for this fixed,
    // in-bounds header size (see `net_tx_eth`'s identical note).
    let iphdr = unsafe { mbufpush(m, core::mem::size_of::<Ip>() as c_uint) as *mut Ip };

    // SAFETY: `iphdr` live.
    unsafe {
        memset(iphdr as *mut c_void, 0, core::mem::size_of::<Ip>());
        (*iphdr).ip_vhl = (4 << 4) | (20 >> 2);
        (*iphdr).ip_p = proto;
        (*iphdr).ip_src = htonl(LOCAL_IP);
        (*iphdr).ip_dst = htonl(dip);
        (*iphdr).ip_len = htons((*m).len as u16);
        (*iphdr).ip_ttl = 100;
        (*iphdr).ip_sum = in_cksum(iphdr as *const u8, core::mem::size_of::<Ip>() as i32);
    }

    // Now on to the ethernet layer.
    net_tx_eth(m, ETHTYPE_IP);
}

/// Sends a UDP packet.
// P3-1D mesh sweep: caller (`sysnet.rs`) now imports this via crate-path
// `use` instead of an `extern` redeclaration -- demoted.
pub(crate) extern "C" fn net_tx_udp(m: *mut mbuf, dip: u32, sport: u16, dport: u16) {
    // SAFETY: `m` live; `mbufpush` always succeeds for this fixed,
    // in-bounds header size.
    let udphdr = unsafe { mbufpush(m, core::mem::size_of::<Udp>() as c_uint) as *mut Udp };

    // SAFETY: `udphdr` live.
    unsafe {
        (*udphdr).sport = htons(sport);
        (*udphdr).dport = htons(dport);
        (*udphdr).ulen = htons((*m).len as u16);
        (*udphdr).sum = 0; // zero means no checksum is provided
    }

    // Now on to the IP layer.
    net_tx_ip(m, IPPROTO_UDP, dip);
}

/// Sends an ARP packet.
fn net_tx_arp(op: u16, dmac: *const u8, dip: u32) -> c_int {
    let m = mbufalloc(MBUF_DEFAULT_HEADROOM);
    if m.is_null() {
        return -1;
    }

    // Generic part of the ARP header.
    // SAFETY: `m` live; `mbufput` always succeeds for this fixed,
    // in-bounds header size.
    let arphdr = unsafe { mbufput(m, core::mem::size_of::<Arp>() as c_uint) as *mut Arp };
    // SAFETY: `arphdr` live.
    unsafe {
        (*arphdr).hrd = htons(ARP_HRD_ETHER);
        (*arphdr).pro = htons(ETHTYPE_IP);
        (*arphdr).hln = ETHADDR_LEN as u8;
        (*arphdr).pln = core::mem::size_of::<u32>() as u8;
        (*arphdr).op = htons(op);
    }

    // Ethernet + IP part of the ARP header.
    let ndev = netdev_get_default();
    // SAFETY: `arphdr` live; `ndev`/`dmac` checked/caller-provided.
    unsafe {
        if !ndev.is_null() {
            memmove((&raw mut (*arphdr).sha) as *mut c_void, (&raw const (*ndev).mac) as *const c_void, ETHADDR_LEN);
        } else {
            memmove((&raw mut (*arphdr).sha) as *mut c_void, LOCAL_MAC.as_ptr() as *const c_void, ETHADDR_LEN);
        }
        (*arphdr).sip = htonl(LOCAL_IP);
        memmove((&raw mut (*arphdr).tha) as *mut c_void, dmac as *const c_void, ETHADDR_LEN);
        (*arphdr).tip = htonl(dip);
    }

    // Header is ready, send the packet.
    net_tx_eth(m, ETHTYPE_ARP);
    0
}

// ===========================================================================
// Receive path.
// ===========================================================================

/// Receives an ARP packet.
fn net_rx_arp(m: *mut mbuf) {
    // SAFETY: `m` live.
    let arphdr = unsafe { mbufpull(m, core::mem::size_of::<Arp>() as c_uint) as *mut Arp };
    if arphdr.is_null() {
        // SAFETY: `m` still caller-owned.
        unsafe { mbuffree(m) };
        return;
    }

    // SAFETY: `arphdr` live.
    let (hrd, pro, hln, pln) = unsafe { (ntohs((*arphdr).hrd), ntohs((*arphdr).pro), (*arphdr).hln, (*arphdr).pln) };
    // Validate the ARP header.
    if hrd != ARP_HRD_ETHER || pro != ETHTYPE_IP || hln != ETHADDR_LEN as u8 || pln != core::mem::size_of::<u32>() as u8 {
        unsafe { mbuffree(m) };
        return;
    }

    // Only requests are supported so far; check if our IP was solicited.
    // SAFETY: `arphdr` live.
    let (op, tip) = unsafe { (ntohs((*arphdr).op), ntohl((*arphdr).tip)) };
    if op != ARP_OP_REQUEST || tip != LOCAL_IP {
        unsafe { mbuffree(m) };
        return;
    }

    // Sender's ethernet address, sender's IP address (qemu's slirp).
    let mut smac = [0u8; ETHADDR_LEN];
    // SAFETY: `arphdr` live; `smac` local and live.
    let sip = unsafe {
        memmove(smac.as_mut_ptr() as *mut c_void, (&raw const (*arphdr).sha) as *const c_void, ETHADDR_LEN);
        ntohl((*arphdr).sip)
    };
    net_tx_arp(ARP_OP_REPLY, smac.as_ptr(), sip);

    // SAFETY: `m` still caller-owned.
    unsafe { mbuffree(m) };
}

/// Receives a UDP packet.
fn net_rx_udp(m: *mut mbuf, mut len: u16, iphdr: *mut Ip) {
    // SAFETY: `m` live.
    let udphdr = unsafe { mbufpull(m, core::mem::size_of::<Udp>() as c_uint) as *mut Udp };
    if udphdr.is_null() {
        unsafe { mbuffree(m) };
        return;
    }

    // TODO: validate UDP checksum.

    // Validate lengths reported in headers.
    // SAFETY: `udphdr` live.
    if ntohs(unsafe { (*udphdr).ulen }) != len {
        unsafe { mbuffree(m) };
        return;
    }
    len -= core::mem::size_of::<Udp>() as u16;
    // SAFETY: `m` live.
    if len > unsafe { (*m).len as u16 } {
        unsafe { mbuffree(m) };
        return;
    }
    // Minimum packet size could be larger than the payload.
    // SAFETY: `m` live.
    unsafe { mbuftrim(m, (*m).len - len as c_uint) };

    // Parse the necessary fields.
    // SAFETY: `iphdr`/`udphdr` live.
    let (sip, sport, dport) = unsafe { (ntohl((*iphdr).ip_src), ntohs((*udphdr).sport), ntohs((*udphdr).dport)) };
    // SAFETY: `m` handed off to `sockrecvudp` (`sysnet.rs`, this wave),
    // which takes ownership (delivers it to a socket's rxq, or frees it
    // if no socket matches -- matches the C `sockrecvudp` contract).
    unsafe { sockrecvudp(m, sip, dport, sport) };
}

/// Receives an IP packet.
fn net_rx_ip(m: *mut mbuf) {
    // SAFETY: `m` live.
    let iphdr = unsafe { mbufpull(m, core::mem::size_of::<Ip>() as c_uint) as *mut Ip };
    if iphdr.is_null() {
        unsafe { mbuffree(m) };
        return;
    }

    // SAFETY: `iphdr` live.
    unsafe {
        // Check IP version and header length.
        if (*iphdr).ip_vhl != ((4 << 4) | (20 >> 2)) {
            mbuffree(m);
            return;
        }
        // Validate IP checksum.
        if in_cksum(iphdr as *const u8, core::mem::size_of::<Ip>() as i32) != 0 {
            mbuffree(m);
            return;
        }
        // Can't support fragmented IP packets.
        if htons((*iphdr).ip_off) != 0 {
            mbuffree(m);
            return;
        }
        // Is the packet addressed to us?
        if htonl((*iphdr).ip_dst) != LOCAL_IP {
            mbuffree(m);
            return;
        }
        // Can only support UDP.
        if (*iphdr).ip_p != IPPROTO_UDP {
            mbuffree(m);
            return;
        }

        let len = ntohs((*iphdr).ip_len).wrapping_sub(core::mem::size_of::<Ip>() as u16);
        net_rx_udp(m, len, iphdr);
    }
}

/// Called by the e1000/x1_emac driver's interrupt handler to deliver a
/// packet to the networking stack.
// P3-1D mesh sweep: callers (`e1000.rs`, `dev/x1_emac.rs`) now import
// this via crate-path `use` instead of an `extern` redeclaration --
// demoted.
pub(crate) extern "C" fn net_rx(m: *mut mbuf) {
    // SAFETY: `m` live (caller contract).
    let ethhdr = unsafe { mbufpull(m, core::mem::size_of::<Eth>() as c_uint) as *mut Eth };
    if ethhdr.is_null() {
        unsafe { mbuffree(m) };
        return;
    }

    // SAFETY: `ethhdr` live.
    let type_ = ntohs(unsafe { (*ethhdr).type_ });
    if type_ == ETHTYPE_IP {
        net_rx_ip(m);
    } else if type_ == ETHTYPE_ARP {
        net_rx_arp(m);
    } else {
        unsafe { mbuffree(m) };
    }
}
