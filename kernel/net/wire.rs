//! Ethernet, IPv4, UDP and ARP wire formats, independent of allocation and DMA.

use core::ops::Range;

pub const ETHERNET_LEN: usize = 14;
pub const IPV4_LEN: usize = 20;
pub const UDP_LEN: usize = 8;
pub const ARP_LEN: usize = 28;
pub const LOCAL_IP: u32 = u32::from_be_bytes([10, 0, 2, 15]);
pub const LOCAL_MAC: [u8; 6] = [0x52, 0x54, 0x00, 0x12, 0x34, 0x56];

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u16)]
pub enum EtherType {
    Ipv4 = 0x0800,
    Arp = 0x0806,
}

impl TryFrom<u16> for EtherType {
    type Error = ();

    fn try_from(value: u16) -> Result<Self, Self::Error> {
        match value {
            0x0800 => Ok(Self::Ipv4),
            0x0806 => Ok(Self::Arp),
            _ => Err(()),
        }
    }
}

/// Validated dispatch information. Payload ranges exclude Ethernet padding.
#[derive(Debug, PartialEq, Eq)]
pub enum ReceivedPacket {
    ArpRequest { sender_mac: [u8; 6], sender_ip: u32 },
    Udp { source_ip: u32, source_port: u16, destination_port: u16, payload: Range<usize> },
}

fn read_u16(bytes: &[u8]) -> Option<u16> {
    Some(u16::from_be_bytes(bytes.get(..2)?.try_into().ok()?))
}

fn read_u32(bytes: &[u8]) -> Option<u32> {
    Some(u32::from_be_bytes(bytes.get(..4)?.try_into().ok()?))
}

/// Internet checksum in host numeric order; serialize with `to_be_bytes()`.
pub fn checksum(bytes: &[u8]) -> u16 {
    let mut sum = 0u32;
    for pair in bytes.chunks(2) {
        let word = u16::from_be_bytes([pair[0], pair.get(1).copied().unwrap_or(0)]);
        sum += u32::from(word);
        // Folding each word also prevents overflow on arbitrarily long slices.
        sum = (sum & 0xffff) + (sum >> 16);
    }
    !(sum as u16)
}

/// Parse a complete frame without aligned loads or unchecked header arithmetic.
pub fn receive(frame: &[u8]) -> Option<ReceivedPacket> {
    let ethernet = frame.get(..ETHERNET_LEN)?;
    let data = &frame[ETHERNET_LEN..];
    match EtherType::try_from(read_u16(&ethernet[12..])?).ok()? {
        EtherType::Arp => {
            let arp = data.get(..ARP_LEN)?;
            if read_u16(arp)? != 1 || read_u16(&arp[2..])? != EtherType::Ipv4 as u16
                || arp[4] != 6 || arp[5] != 4 || read_u16(&arp[6..])? != 1
                || read_u32(&arp[24..])? != LOCAL_IP
            {
                return None;
            }
            Some(ReceivedPacket::ArpRequest {
                sender_mac: arp[8..14].try_into().ok()?,
                sender_ip: read_u32(&arp[14..])?,
            })
        }
        EtherType::Ipv4 => {
            let ip = data.get(..IPV4_LEN)?;
            // Options and fragments require reassembly support. DF is valid
            // on an unfragmented datagram and must not cause it to be dropped.
            if ip[0] != 0x45 || checksum(ip) != 0 || read_u16(&ip[6..])? & 0xbfff != 0
                || ip[9] != 17 || read_u32(&ip[16..])? != LOCAL_IP
            {
                return None;
            }
            let ip_len = usize::from(read_u16(&ip[2..])?);
            let datagram = data.get(IPV4_LEN..ip_len)?;
            let udp = datagram.get(..UDP_LEN)?;
            if usize::from(read_u16(&udp[4..])?) != datagram.len() {
                return None;
            }
            // A zero UDP checksum is permitted by IPv4. Nonzero checksum
            // validation remains unimplemented in this minimal stack.
            Some(ReceivedPacket::Udp {
                source_ip: read_u32(&ip[12..])?,
                source_port: read_u16(udp)?,
                destination_port: read_u16(&udp[2..])?,
                payload: ETHERNET_LEN + IPV4_LEN + UDP_LEN..ETHERNET_LEN + ip_len,
            })
        }
    }
}

pub fn ethernet(source: [u8; 6], kind: EtherType) -> [u8; ETHERNET_LEN] {
    let mut bytes = [0; ETHERNET_LEN];
    // The minimal stack uses broadcast until it has an ARP cache.
    bytes[..6].fill(0xff);
    bytes[6..12].copy_from_slice(&source);
    bytes[12..].copy_from_slice(&(kind as u16).to_be_bytes());
    bytes
}

pub fn ipv4(destination: u32, payload_len: usize) -> Option<[u8; IPV4_LEN]> {
    let total_len = u16::try_from(payload_len.checked_add(IPV4_LEN)?).ok()?;
    let mut bytes = [0; IPV4_LEN];
    bytes[0] = 0x45;
    bytes[2..4].copy_from_slice(&total_len.to_be_bytes());
    bytes[8] = 100;
    bytes[9] = 17;
    bytes[12..16].copy_from_slice(&LOCAL_IP.to_be_bytes());
    bytes[16..20].copy_from_slice(&destination.to_be_bytes());
    let sum = checksum(&bytes);
    bytes[10..12].copy_from_slice(&sum.to_be_bytes());
    Some(bytes)
}

pub fn udp(source: u16, destination: u16, payload_len: usize) -> Option<[u8; UDP_LEN]> {
    let total_len = u16::try_from(payload_len.checked_add(UDP_LEN)?).ok()?;
    let mut bytes = [0; UDP_LEN];
    bytes[..2].copy_from_slice(&source.to_be_bytes());
    bytes[2..4].copy_from_slice(&destination.to_be_bytes());
    bytes[4..6].copy_from_slice(&total_len.to_be_bytes());
    Some(bytes)
}

pub fn arp_reply(source_mac: [u8; 6], destination_mac: [u8; 6], destination_ip: u32) -> [u8; ARP_LEN] {
    let mut bytes = [0; ARP_LEN];
    bytes[..2].copy_from_slice(&1u16.to_be_bytes());
    bytes[2..4].copy_from_slice(&(EtherType::Ipv4 as u16).to_be_bytes());
    bytes[4] = 6;
    bytes[5] = 4;
    bytes[6..8].copy_from_slice(&2u16.to_be_bytes());
    bytes[8..14].copy_from_slice(&source_mac);
    bytes[14..18].copy_from_slice(&LOCAL_IP.to_be_bytes());
    bytes[18..24].copy_from_slice(&destination_mac);
    bytes[24..28].copy_from_slice(&destination_ip.to_be_bytes());
    bytes
}

#[cfg(test)]
mod tests {
    use super::*;

    fn datagram(payload: &[u8]) -> Vec<u8> {
        let mut frame = ethernet(LOCAL_MAC, EtherType::Ipv4).to_vec();
        frame.extend(ipv4(LOCAL_IP, UDP_LEN + payload.len()).unwrap());
        frame.extend(udp(2000, 2001, payload.len()).unwrap());
        frame.extend(payload);
        frame
    }

    fn fix_ip_checksum(frame: &mut [u8]) {
        let ip = &mut frame[ETHERNET_LEN..ETHERNET_LEN + IPV4_LEN];
        ip[10..12].fill(0);
        let sum = checksum(ip);
        ip[10..12].copy_from_slice(&sum.to_be_bytes());
    }

    #[test]
    fn checksum_handles_odd_lengths_and_known_header() {
        assert_eq!(checksum(&[]), 0xffff);
        assert_eq!(checksum(&[0x12, 0x34, 0x56]), 0x97cb);
        let header = [0x45, 0, 0, 0x73, 0, 0, 0x40, 0, 0x40, 0x11, 0, 0, 0xc0, 0xa8, 0, 1, 0xc0, 0xa8, 0, 0xc7];
        assert_eq!(checksum(&header), 0xb861);
        assert_eq!(checksum(&vec![0xff; 200_000]), 0);
    }

    #[test]
    fn receives_udp_and_excludes_ethernet_padding() {
        let mut frame = datagram(b"hello");
        frame.resize(64, 0xaa);
        let Some(ReceivedPacket::Udp { source_ip, source_port, destination_port, payload }) = receive(&frame) else {
            panic!("valid UDP packet was rejected");
        };
        assert_eq!(source_ip, LOCAL_IP);
        assert_eq!((source_port, destination_port), (2000, 2001));
        assert_eq!(&frame[payload], b"hello");
    }

    #[test]
    fn rejects_every_truncated_header_and_payload() {
        let frame = datagram(b"hello");
        for len in 0..frame.len() {
            assert_eq!(receive(&frame[..len]), None, "length {len}");
        }
    }

    #[test]
    fn rejects_underflowing_and_inconsistent_lengths() {
        for length in 0u16..(IPV4_LEN + UDP_LEN) as u16 {
            let mut frame = datagram(b"hello");
            frame[16..18].copy_from_slice(&length.to_be_bytes());
            fix_ip_checksum(&mut frame);
            assert_eq!(receive(&frame), None, "IP length {length}");
        }
        for length in [0u16, 7, 8, 12, 14, u16::MAX] {
            let mut frame = datagram(b"hello");
            frame[38..40].copy_from_slice(&length.to_be_bytes());
            assert_eq!(receive(&frame), None, "UDP length {length}");
        }
        assert!(ipv4(LOCAL_IP, usize::MAX).is_none());
        assert!(udp(1, 2, usize::MAX).is_none());
    }

    #[test]
    fn allows_dont_fragment_but_rejects_fragments() {
        for (flags, accepted) in [(0x4000u16, true), (0x2000, false), (1, false), (0x8000, false)] {
            let mut frame = datagram(&[]);
            frame[20..22].copy_from_slice(&flags.to_be_bytes());
            fix_ip_checksum(&mut frame);
            assert_eq!(receive(&frame).is_some(), accepted);
        }
    }

    #[test]
    fn rejects_bad_checksum_protocol_and_destination() {
        let mut frame = datagram(&[]);
        frame[24] ^= 1;
        assert_eq!(receive(&frame), None);
        for (offset, value) in [(14, 0x46), (23, 6), (33, 99)] {
            let mut frame = datagram(&[]);
            frame[offset] = value;
            fix_ip_checksum(&mut frame);
            assert_eq!(receive(&frame), None);
        }
    }

    #[test]
    fn arp_requests_are_validated_before_dispatch() {
        let sender = [1, 2, 3, 4, 5, 6];
        let mut arp = arp_reply(sender, LOCAL_MAC, LOCAL_IP);
        arp[6..8].copy_from_slice(&1u16.to_be_bytes());
        let mut frame = ethernet(sender, EtherType::Arp).to_vec();
        frame.extend(arp);
        assert_eq!(receive(&frame), Some(ReceivedPacket::ArpRequest { sender_mac: sender, sender_ip: LOCAL_IP }));
        for len in 0..frame.len() {
            assert_eq!(receive(&frame[..len]), None);
        }
        for offset in [14, 16, 18, 19, 21, 41] {
            let mut invalid = frame.clone();
            invalid[offset] ^= 0x80;
            assert_eq!(receive(&invalid), None);
        }
    }
}
