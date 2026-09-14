//! Minimal Ethernet/IPv4/UDP/ARP stack with owned packet buffers.
//!
//! DMA ownership is isolated in `buffer`; wire parsing and serialization use
//! checked slices in `wire`. The protocol path moves packets between layers.

use crate::dev::netdev::Netdev;
use crate::sysnet::SysNet;

mod buffer;
mod wire;

pub use buffer::{Mbuf, MbufQueue};
pub(crate) use buffer::{Packet, MBUF_DEFAULT_HEADROOM, MBUF_SIZE};

pub(crate) struct Net;

impl Net {
    /// Maximum UDP payload that fits both the active interface MTU and
    /// this stack's packet storage. Fragmentation is not implemented.
    pub(crate) fn udp_payload_capacity() -> Option<usize> {
        let raw = Netdev::get_default();
        // SAFETY: registered devices have static lifetime; mtu is set before
        // publication and neither supported driver changes it afterward.
        let device = unsafe { raw.as_ref() }?;
        let mtu = usize::try_from(device.mtu).ok()?;
        let payload = mtu.checked_sub(wire::IPV4_LEN + wire::UDP_LEN)?;
        Some(payload.min(MBUF_SIZE - MBUF_DEFAULT_HEADROOM))
    }

    fn local_mac() -> [u8; 6] {
        let device = Netdev::get_default();
        // SAFETY: registered netdevs have static lifetime; their MAC is set
        // before publication and is not modified by either network driver.
        unsafe { device.as_ref() }.map_or(wire::LOCAL_MAC, |device| device.mac)
    }

    fn transmit_ethernet(mut packet: Packet, kind: wire::EtherType) {
        if packet.prepend(&wire::ethernet(Self::local_mac(), kind)).is_none() {
            return;
        }
        let raw_device = Netdev::get_default();
        // SAFETY: published netdevs live for the lifetime of the kernel.
        let Some(device) = (unsafe { raw_device.as_ref() }) else { return };
        let Some(ops) = device.ops else { return };
        let raw_packet = packet.into_raw();
        // SAFETY: the registered driver receives ownership on success. On
        // failure the driver contract leaves the packet owned by this call.
        if unsafe { ops.transmit(raw_device, raw_packet) } != 0 {
            // SAFETY: failed transmission did not consume the allocation.
            unsafe { Mbuf::free(raw_packet) };
        }
    }

    /// Send one UDP datagram, consuming its packet allocation on every path.
    pub(crate) fn net_tx_udp(mut packet: Packet, destination: u32, source_port: u16, destination_port: u16) {
        let Some(udp) = wire::udp(source_port, destination_port, packet.len()) else { return };
        if packet.prepend(&udp).is_none() {
            return;
        }
        let Some(ip) = wire::ipv4(destination, packet.len()) else { return };
        if packet.prepend(&ip).is_none() {
            return;
        }
        Self::transmit_ethernet(packet, wire::EtherType::Ipv4);
    }

    fn receive(mut packet: Packet) {
        match wire::receive(packet.as_ref()) {
            Some(wire::ReceivedPacket::ArpRequest { sender_mac, sender_ip }) => {
                let Some(mut reply) = Packet::allocate(MBUF_DEFAULT_HEADROOM) else { return };
                let Some(bytes) = reply.append(wire::ARP_LEN) else { return };
                bytes.copy_from_slice(&wire::arp_reply(Self::local_mac(), sender_mac, sender_ip));
                Self::transmit_ethernet(reply, wire::EtherType::Arp);
            }
            Some(wire::ReceivedPacket::Udp { source_ip, source_port, destination_port, payload }) => {
                if packet.retain(payload).is_some() {
                    SysNet::sockrecvudp(packet, source_ip, destination_port, source_port);
                }
            }
            None => {}
        }
    }

    /// Receive a driver's completed frame.
    ///
    /// # Safety
    /// `raw` must satisfy `Packet::from_raw`'s ownership and DMA contract.
    pub(crate) unsafe fn net_rx(raw: *mut Mbuf) {
        // SAFETY: the driver transfers exclusive ownership after DMA completion.
        if let Some(packet) = unsafe { Packet::from_raw(raw) } {
            Self::receive(packet);
        }
    }
}
