//! Allocation-free, checked views of an immutable flattened device tree.
//!
//! Format: https://devicetree-specification.readthedocs.io/en/stable/flattened-format.html
//! Raw firmware pointers and boot-time allocation belong to the parent module.

#![forbid(unsafe_code)]

use core::ffi::CStr;

pub const HEADER_LEN: usize = 40;
const MAX_DEPTH: usize = 32;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Error {
    Truncated,
    Magic,
    Version,
    Bounds,
    Alignment,
    Token,
    Structure,
    Depth,
    String,
    Cells,
    Range,
    Allocation,
}

fn word(bytes: &[u8], offset: usize) -> Result<u32, Error> {
    let end = offset.checked_add(4).ok_or(Error::Bounds)?;
    Ok(u32::from_be_bytes(bytes.get(offset..end).ok_or(Error::Truncated)?.try_into().map_err(|_| Error::Truncated)?))
}

fn double_word(bytes: &[u8], offset: usize) -> Result<u64, Error> {
    let end = offset.checked_add(8).ok_or(Error::Bounds)?;
    Ok(u64::from_be_bytes(bytes.get(offset..end).ok_or(Error::Truncated)?.try_into().map_err(|_| Error::Truncated)?))
}

/// Only reads the fixed header. The raw adapter must establish readable storage
/// for this many bytes before constructing the full borrowed slice.
pub fn total_size(header: &[u8]) -> Result<usize, Error> {
    if header.len() < HEADER_LEN { return Err(Error::Truncated); }
    if word(header, 0)? != 0xd00d_feed { return Err(Error::Magic); }
    let size = word(header, 4)? as usize;
    if size < HEADER_LEN || size > isize::MAX as usize { return Err(Error::Bounds); }
    Ok(size)
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum TokenKind { Begin, EndNode, Property, Nop, End }

impl TryFrom<u32> for TokenKind {
    type Error = Error;
    fn try_from(value: u32) -> Result<Self, Error> {
        match value {
            1 => Ok(Self::Begin), 2 => Ok(Self::EndNode), 3 => Ok(Self::Property),
            4 => Ok(Self::Nop), 9 => Ok(Self::End), _ => Err(Error::Token),
        }
    }
}

#[derive(Clone, Copy)]
enum Token<'a> { Begin(&'a CStr), EndNode, Property(Property<'a>), Nop, End }

#[derive(Clone, Copy)]
struct Cursor<'a> { bytes: &'a [u8], strings: &'a [u8], position: usize }

impl<'a> Cursor<'a> {
    fn new(bytes: &'a [u8], strings: &'a [u8]) -> Self { Self { bytes, strings, position: 0 } }
    fn take(&mut self, len: usize) -> Result<&'a [u8], Error> {
        let end = self.position.checked_add(len).ok_or(Error::Bounds)?;
        let bytes = self.bytes.get(self.position..end).ok_or(Error::Truncated)?;
        self.position = end;
        Ok(bytes)
    }
    fn word(&mut self) -> Result<u32, Error> { word(self.take(4)?, 0) }
    fn align(&mut self) -> Result<(), Error> {
        self.take((4 - (self.position & 3)) & 3)?;
        Ok(())
    }
    fn next(&mut self) -> Result<Token<'a>, Error> {
        match TokenKind::try_from(self.word()?)? {
            TokenKind::Begin => {
                let name = CStr::from_bytes_until_nul(self.bytes.get(self.position..).ok_or(Error::Truncated)?)
                    .map_err(|_| Error::String)?;
                self.take(name.to_bytes_with_nul().len())?;
                self.align()?;
                Ok(Token::Begin(name))
            }
            TokenKind::Property => {
                let len = self.word()? as usize;
                let offset = self.word()? as usize;
                let name = CStr::from_bytes_until_nul(self.strings.get(offset..).ok_or(Error::Bounds)?)
                    .map_err(|_| Error::String)?;
                if name.to_bytes().is_empty() { return Err(Error::String); }
                let bytes = self.take(len)?;
                self.align()?;
                Ok(Token::Property(Property { name, bytes }))
            }
            TokenKind::EndNode => Ok(Token::EndNode),
            TokenKind::Nop => Ok(Token::Nop),
            TokenKind::End => Ok(Token::End),
        }
    }

    /// Called only for a begin token in an already validated tree.
    fn node(&mut self, name: &'a CStr) -> Option<Node<'a>> {
        let start = self.position;
        let mut depth = 1;
        while self.position < self.bytes.len() {
            let end = self.position;
            match self.next().ok()? {
                Token::Begin(_) => depth += 1,
                Token::EndNode => {
                    depth -= 1;
                    if depth == 0 { return Some(Node { name, body: &self.bytes[start..end], strings: self.strings }); }
                }
                _ => {}
            }
        }
        None
    }
}

#[derive(Clone, Copy)]
pub struct DeviceTree<'a> {
    bytes: &'a [u8],
    structure: &'a [u8],
    strings: &'a [u8],
    reservations: &'a [u8],
    count: usize,
}

impl<'a> DeviceTree<'a> {
    pub fn parse(bytes: &'a [u8]) -> Result<Self, Error> {
        let total = total_size(bytes)?;
        let bytes = bytes.get(..total).ok_or(Error::Truncated)?;
        let version = word(bytes, 20)?;
        let compatible = word(bytes, 24)?;
        if version < 16 || compatible > 17 || compatible > version { return Err(Error::Version); }
        let header_len = if version == 16 { 36 } else { HEADER_LEN };
        let struct_start = word(bytes, 8)? as usize;
        let strings_start = word(bytes, 12)? as usize;
        let reserve_start = word(bytes, 16)? as usize;
        let strings_size = word(bytes, 32)? as usize;
        let struct_size = if version == 16 {
            strings_start.checked_sub(struct_start).ok_or(Error::Bounds)?
        } else { word(bytes, 36)? as usize };
        let struct_end = struct_start.checked_add(struct_size).ok_or(Error::Bounds)?;
        let strings_end = strings_start.checked_add(strings_size).ok_or(Error::Bounds)?;
        if struct_start % 4 != 0 || reserve_start % 8 != 0 { return Err(Error::Alignment); }
        if struct_start < header_len || strings_start < header_len || reserve_start < header_len
            || struct_end > total || strings_end > total
            || (struct_start < strings_end && strings_start < struct_end)
        { return Err(Error::Bounds); }
        // The reservation block has no length field. Stop before the next
        // declared block, never by reading beyond the blob looking for a zero.
        let reserve_end = [struct_start, strings_start, total].into_iter()
            .filter(|end| *end >= reserve_start).min().ok_or(Error::Bounds)?;
        if (struct_start..struct_end).contains(&reserve_start)
            || (strings_start..strings_end).contains(&reserve_start)
        { return Err(Error::Bounds); }
        let reserve_bytes = bytes.get(reserve_start..reserve_end).ok_or(Error::Bounds)?;
        let mut reserve_len = 0;
        loop {
            let base = double_word(reserve_bytes, reserve_len)?;
            let size = double_word(reserve_bytes, reserve_len + 8)?;
            if base == 0 && size == 0 { break; }
            base.checked_add(size).ok_or(Error::Range)?;
            reserve_len += 16;
        }
        let strings = &bytes[strings_start..strings_end];
        let structure = &bytes[struct_start..struct_end];
        let mut cursor = Cursor::new(structure, strings);
        let mut depth = 0usize;
        let mut children_started = [false; MAX_DEPTH];
        let mut seen_root = false;
        let mut count = 0usize;
        loop {
            match cursor.next()? {
                Token::Begin(name) => {
                    if depth == 0 {
                        if seen_root || !name.to_bytes().is_empty() { return Err(Error::Structure); }
                        seen_root = true;
                    } else {
                        if name.to_bytes().is_empty() || name.to_bytes().contains(&b'/') { return Err(Error::String); }
                        children_started[depth - 1] = true;
                    }
                    if depth == MAX_DEPTH { return Err(Error::Depth); }
                    children_started[depth] = false;
                    depth += 1;
                    count += 1;
                }
                Token::EndNode => { depth = depth.checked_sub(1).ok_or(Error::Structure)?; }
                Token::Property(_) => {
                    if depth == 0 || children_started[depth - 1] { return Err(Error::Structure); }
                    count += 1;
                }
                Token::Nop => {}
                Token::End => {
                    if !seen_root || depth != 0 || (version != 16 && cursor.position != structure.len()) {
                        return Err(Error::Structure);
                    }
                    return Ok(Self { bytes, structure: &structure[..cursor.position], strings,
                        reservations: &reserve_bytes[..reserve_len], count });
                }
            }
        }
    }

    pub fn bytes(self) -> &'a [u8] { self.bytes }
    pub fn node_property_count(self) -> usize { self.count }
    pub fn root(self) -> Node<'a> {
        let mut cursor = Cursor::new(self.structure, self.strings);
        loop {
            if let Token::Begin(name) = cursor.next().expect("validated FDT root") {
                return cursor.node(name).expect("validated FDT root extent");
            }
        }
    }
    pub fn reservations(self) -> impl Iterator<Item = Region> + 'a {
        self.reservations.chunks_exact(16).map(|bytes| Region {
            base: double_word(bytes, 0).expect("validated reservation"),
            size: double_word(bytes, 8).expect("validated reservation"),
        })
    }
    /// Returns the controller and its parent, so `reg` uses its own bus cells.
    pub fn find_phandle(self, handle: u32) -> Result<Option<(Node<'a>, Node<'a>)>, Error> {
        fn find<'a>(node: Node<'a>, parent: Node<'a>, handle: u32) -> Result<Option<(Node<'a>, Node<'a>)>, Error> {
            for name in [c"phandle", c"linux,phandle"] {
                if let Some(prop) = node.property(name) {
                    if prop.u32()? == handle { return Ok(Some((node, parent))); }
                }
            }
            for child in node.children() {
                if let Some(found) = find(child, node, handle)? { return Ok(Some(found)); }
            }
            Ok(None)
        }
        if handle == 0 || handle == u32::MAX { return Ok(None); }
        find(self.root(), self.root(), handle)
    }
}

#[derive(Clone, Copy)]
pub struct Node<'a> { name: &'a CStr, body: &'a [u8], strings: &'a [u8] }

impl<'a> Node<'a> {
    pub fn name(self) -> &'a CStr { self.name }
    pub fn base_name(self) -> &'a [u8] { self.name.to_bytes().split(|b| *b == b'@').next().unwrap_or_default() }
    pub fn properties(self) -> impl Iterator<Item = Property<'a>> {
        let mut cursor = Cursor::new(self.body, self.strings);
        let mut finished = false;
        core::iter::from_fn(move || loop {
            if finished { return None; }
            match cursor.next().ok()? {
                Token::Property(prop) => return Some(prop),
                Token::Nop => {},
                _ => { finished = true; return None; },
            }
        })
    }
    pub fn property(self, name: &CStr) -> Option<Property<'a>> { self.properties().find(|prop| prop.name == name) }
    pub fn u32_or(self, name: &CStr, default: u32) -> Result<u32, Error> {
        self.property(name).map_or(Ok(default), Property::u32)
    }
    pub fn children(self) -> impl Iterator<Item = Node<'a>> {
        let mut cursor = Cursor::new(self.body, self.strings);
        core::iter::from_fn(move || loop {
            match cursor.next().ok()? {
                Token::Begin(name) => return cursor.node(name),
                Token::Property(_) | Token::Nop => {},
                _ => return None,
            }
        })
    }
    /// Preserve the old tree's device order without allocating an index. Boot
    /// discovery is small; repeated immutable scans avoid arena-owned links.
    pub fn ordered_children(self) -> impl Iterator<Item = Node<'a>> {
        let mut previous = None;
        core::iter::from_fn(move || {
            let node = self.children().filter(|node| previous.is_none_or(|key| node.key() > key))
                .min_by_key(|node| node.key())?;
            previous = Some(node.key());
            Some(node)
        })
    }
    pub fn child(self, name: &CStr) -> Option<Node<'a>> { self.children().find(|node| node.name == name) }
    pub fn children_named(self, name: &'a [u8]) -> impl Iterator<Item = Node<'a>> {
        self.ordered_children().filter(move |node| node.base_name() == name)
    }
    pub fn compatible(self, names: &[&CStr]) -> Result<bool, Error> {
        let Some(prop) = self.property(c"compatible") else { return Ok(false); };
        Ok(prop.strings()?.any(|entry| names.contains(&entry)))
    }
    fn key(self) -> (u64, &'a [u8], bool, u64, &'a [u8]) {
        let base = self.base_name();
        let suffix = self.name.to_bytes().get(base.len() + 1..);
        let address = suffix.map_or(0, |bytes| {
            if bytes.is_empty() { return name_hash(bytes); }
            bytes.iter().try_fold(0u64, |value, byte| {
                let digit = match byte { b'0'..=b'9' => byte - b'0', b'a'..=b'f' => byte - b'a' + 10,
                    b'A'..=b'F' => byte - b'A' + 10, _ => return None };
                Some(value.wrapping_mul(16).wrapping_add(digit as u64))
            }).unwrap_or_else(|| name_hash(bytes))
        });
        // Distinct names may have the same numeric address/hash; retain both.
        (name_hash(base), base, suffix.is_some(), address, self.name.to_bytes())
    }
}

fn name_hash(bytes: &[u8]) -> u64 {
    const GOLDEN: u64 = 0x9e37_ffff_fffc_0001;
    let mut hash = GOLDEN.wrapping_mul(bytes.len() as u64);
    let mut chunks = bytes.chunks_exact(8);
    for chunk in &mut chunks {
        hash ^= u64::from_le_bytes(chunk.try_into().expect("eight-byte chunk")).wrapping_mul(GOLDEN);
    }
    let tail = chunks.remainder().iter().fold(0u64, |word, byte| (word << 8) | *byte as u64);
    hash ^= tail.wrapping_mul(GOLDEN);
    if hash == 0 { GOLDEN } else { hash }
}

#[derive(Clone, Copy)]
pub struct Property<'a> { name: &'a CStr, bytes: &'a [u8] }

impl<'a> Property<'a> {
    pub fn name(self) -> &'a CStr { self.name }
    pub fn bytes(self) -> &'a [u8] { self.bytes }
    pub fn cell(self, index: usize) -> Result<u32, Error> { word(self.bytes, index.checked_mul(4).ok_or(Error::Bounds)?) }
    pub fn u32(self) -> Result<u32, Error> {
        if self.bytes.len() != 4 { return Err(Error::Cells); }
        self.cell(0)
    }
    pub fn integer(self) -> Result<u64, Error> {
        match self.bytes.len() { 4 => Ok(self.cell(0)? as u64), 8 => double_word(self.bytes, 0), _ => Err(Error::Cells) }
    }
    pub fn strings(self) -> Result<Strings<'a>, Error> {
        let mut remaining = self.bytes;
        while !remaining.is_empty() {
            let string = CStr::from_bytes_until_nul(remaining).map_err(|_| Error::String)?;
            remaining = &remaining[string.to_bytes_with_nul().len()..];
        }
        Ok(Strings(self.bytes))
    }
    pub fn regions(self, cells: CellConfig) -> Result<Regions<'a>, Error> {
        let stride = (cells.address + cells.size) * 4;
        if self.bytes.len() % stride != 0 { return Err(Error::Cells); }
        let regions = Regions { chunks: self.bytes.chunks_exact(stride), cells };
        for region in regions.clone() { region.base.checked_add(region.size).ok_or(Error::Range)?; }
        Ok(regions)
    }
}

#[derive(Clone)]
pub struct Strings<'a>(&'a [u8]);
impl<'a> Iterator for Strings<'a> {
    type Item = &'a CStr;
    fn next(&mut self) -> Option<Self::Item> {
        if self.0.is_empty() { return None; }
        let string = CStr::from_bytes_until_nul(self.0).ok()?;
        self.0 = &self.0[string.to_bytes_with_nul().len()..];
        Some(string)
    }
}

#[derive(Clone, Copy, Debug)]
pub struct CellConfig { address: usize, size: usize }
impl CellConfig {
    pub const ROOT_DEFAULT: Self = Self { address: 2, size: 1 };
    pub fn for_node(node: Node<'_>, default: Self) -> Result<Self, Error> {
        let address = node.u32_or(c"#address-cells", default.address as u32)? as usize;
        let size = node.u32_or(c"#size-cells", default.size as u32)? as usize;
        // The kernel's physical/MMIO addresses are u64. PCI child bus addresses
        // with three cells are not used as physical `reg` values here.
        if !(1..=2).contains(&address) || size > 2 { return Err(Error::Cells); }
        Ok(Self { address, size })
    }
}

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct Region { pub base: u64, pub size: u64 }

/// The current UART driver accesses registers 0 through 6 with this stride.
/// Check the full window before publishing shifts or aligned MMIO operands.
pub fn uart_window(region: Region, shift: u32, width: u32) -> Result<(), Error> {
    if ![1, 4].contains(&width) || (width == 4 && (region.base % 4 != 0 || shift < 2)) {
        return Err(Error::Range);
    }
    let span = 1u64.checked_shl(shift).and_then(|stride| stride.checked_mul(6))
        .and_then(|last| last.checked_add(width as u64)).ok_or(Error::Range)?;
    region.base.checked_add(span).ok_or(Error::Range)?;
    if span > region.size { return Err(Error::Range); }
    Ok(())
}

#[derive(Clone)]
pub struct Regions<'a> { chunks: core::slice::ChunksExact<'a, u8>, cells: CellConfig }
impl Iterator for Regions<'_> {
    type Item = Region;
    fn next(&mut self) -> Option<Region> {
        let bytes = self.chunks.next()?;
        let address_bytes = self.cells.address * 4;
        let decode = |bytes: &[u8]| bytes.iter().fold(0u64, |value, byte| (value << 8) | *byte as u64);
        Some(Region { base: decode(&bytes[..address_bytes]), size: decode(&bytes[address_bytes..]) })
    }
}

pub fn first_memory(tree: DeviceTree<'_>) -> Result<Option<Region>, Error> {
    let root = tree.root();
    let cells = CellConfig::for_node(root, CellConfig::ROOT_DEFAULT)?;
    for node in root.children_named(b"memory") {
        if let Some(reg) = node.property(c"reg") {
            if let Some(region) = reg.regions(cells)?.find(|region| region.size != 0) { return Ok(Some(region)); }
        }
    }
    Ok(None)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::vec::Vec;

    struct Builder { structure: Vec<u8>, strings: Vec<u8>, reserved: Vec<u8> }
    impl Builder {
        fn new() -> Self { Self { structure: Vec::new(), strings: Vec::new(), reserved: Vec::new() } }
        fn token(&mut self, token: u32) { self.structure.extend_from_slice(&token.to_be_bytes()); }
        fn pad(&mut self) { while self.structure.len() % 4 != 0 { self.structure.push(0); } }
        fn begin(&mut self, name: &CStr) {
            self.token(1); self.structure.extend_from_slice(name.to_bytes_with_nul()); self.pad();
        }
        fn end(&mut self) { self.token(2); }
        fn prop(&mut self, name: &CStr, value: &[u8]) {
            self.token(3); self.token(value.len() as u32); self.token(self.strings.len() as u32);
            self.strings.extend_from_slice(name.to_bytes_with_nul());
            self.structure.extend_from_slice(value); self.pad();
        }
        fn cells(&mut self, name: &CStr, value: &[u32]) {
            let value: Vec<_> = value.iter().flat_map(|cell| cell.to_be_bytes()).collect();
            self.prop(name, &value);
        }
        fn finish(mut self) -> Vec<u8> {
            self.token(9); self.reserved.extend_from_slice(&[0; 16]);
            let structure_offset = HEADER_LEN + self.reserved.len();
            let strings_offset = structure_offset + self.structure.len();
            let header = [0xd00d_feed, (strings_offset + self.strings.len()) as u32,
                structure_offset as u32, strings_offset as u32, HEADER_LEN as u32,
                17, 16, 0, self.strings.len() as u32, self.structure.len() as u32];
            header.into_iter().flat_map(u32::to_be_bytes).chain(self.reserved)
                .chain(self.structure).chain(self.strings).collect()
        }
    }
    fn set_word(bytes: &mut [u8], offset: usize, value: u32) { bytes[offset..offset + 4].copy_from_slice(&value.to_be_bytes()); }
    fn memory_blob(address_cells: u32, size_cells: u32, reg: &[u32]) -> Vec<u8> {
        let mut b = Builder::new();
        b.begin(c""); b.cells(c"#address-cells", &[address_cells]); b.cells(c"#size-cells", &[size_cells]);
        b.begin(c"memory@80000000"); b.cells(c"reg", reg); b.end(); b.end(); b.finish()
    }

    #[test]
    fn root_only_is_valid_without_allocated_sentinel_nodes() {
        let mut b = Builder::new(); b.begin(c""); b.end();
        let bytes = b.finish(); let tree = DeviceTree::parse(&bytes).unwrap();
        assert_eq!(tree.root().name(), c"");
        assert_eq!(tree.root().children().count(), 0);
        assert_eq!(tree.node_property_count(), 1);
        assert_eq!(first_memory(tree), Ok(None));
    }

    #[test]
    fn early_memory_scan_finds_root_child_and_decodes_both_cell_widths() {
        for (address, size, reg) in [(2, 2, &[0, 0x8000_0000, 0, 0x4000_0000][..]),
            (1, 1, &[0x8000_0000, 0x4000_0000][..])] {
            let bytes = memory_blob(address, size, reg);
            assert_eq!(first_memory(DeviceTree::parse(&bytes).unwrap()),
                Ok(Some(Region { base: 0x8000_0000, size: 0x4000_0000 })));
        }
    }

    #[test]
    fn every_truncated_prefix_is_rejected() {
        let bytes = memory_blob(2, 2, &[0, 0x8000_0000, 0, 0x1000]);
        for length in 0..bytes.len() { assert!(DeviceTree::parse(&bytes[..length]).is_err(), "prefix {length}"); }
    }

    #[test]
    fn malformed_offsets_sizes_alignment_and_versions_are_rejected() {
        let original = memory_blob(2, 2, &[0, 0x8000_0000, 0, 0x1000]);
        for (offset, value) in [(0, 0), (4, 0), (8, u32::MAX), (12, u32::MAX),
            (16, 41), (20, 15), (24, 18), (32, u32::MAX), (36, u32::MAX), (8, 57)] {
            let mut bytes = original.clone(); set_word(&mut bytes, offset, value);
            assert!(DeviceTree::parse(&bytes).is_err(), "header offset {offset}");
        }
        let mut overlap = original; set_word(&mut overlap, 12, 56);
        assert!(matches!(DeviceTree::parse(&overlap), Err(Error::Bounds)));
    }

    #[test]
    fn version_16_and_forward_compatible_headers_work() {
        let original = memory_blob(2, 2, &[0, 0x8000_0000, 0, 0x1000]);
        for version in [16, 17, 18] {
            let mut bytes = original.clone(); set_word(&mut bytes, 20, version);
            if version == 16 { set_word(&mut bytes, 36, u32::MAX); }
            assert!(first_memory(DeviceTree::parse(&bytes).unwrap()).unwrap().is_some());
        }
    }

    #[test]
    fn reservation_scan_is_bounded_and_keeps_full_64_bit_values() {
        let mut b = Builder::new(); b.begin(c""); b.end();
        b.reserved.extend_from_slice(&0x1234_5678_0000u64.to_be_bytes());
        b.reserved.extend_from_slice(&0x2000u64.to_be_bytes());
        let mut bytes = b.finish();
        assert_eq!(DeviceTree::parse(&bytes).unwrap().reservations().collect::<Vec<_>>(),
            [Region { base: 0x1234_5678_0000, size: 0x2000 }]);
        bytes[56..72].fill(1);
        assert!(DeviceTree::parse(&bytes).is_err());
        bytes[40..48].fill(255);
        assert!(matches!(DeviceTree::parse(&bytes), Err(Error::Range)));
    }

    #[test]
    fn rejects_missing_terminators_and_out_of_block_property_payloads() {
        let mut b = Builder::new(); b.begin(c""); b.prop(c"value", b"ok\0"); b.end();
        let original = b.finish();
        let mut bytes = original.clone(); *bytes.last_mut().unwrap() = b'x';
        assert!(matches!(DeviceTree::parse(&bytes), Err(Error::String)));
        let mut bytes = original.clone(); set_word(&mut bytes, 68, u32::MAX);
        assert!(DeviceTree::parse(&bytes).is_err());
        let mut bytes = original; set_word(&mut bytes, 72, u32::MAX);
        assert!(DeviceTree::parse(&bytes).is_err());
        let mut b = Builder::new(); b.token(1); b.structure.extend_from_slice(b"nonul");
        let mut bytes = b.finish();
        let start = word(&bytes, 8).unwrap() as usize;
        let end = word(&bytes, 12).unwrap() as usize;
        bytes[start + 4..end].fill(b'x');
        assert!(matches!(DeviceTree::parse(&bytes), Err(Error::String)));
    }

    #[test]
    fn grammar_rejects_late_properties_unbalanced_nodes_and_excess_depth() {
        let mut b = Builder::new(); b.begin(c""); b.begin(c"child"); b.end(); b.prop(c"late", &[]); b.end();
        assert!(matches!(DeviceTree::parse(&b.finish()), Err(Error::Structure)));
        let mut b = Builder::new(); b.begin(c""); b.end(); b.end();
        assert!(matches!(DeviceTree::parse(&b.finish()), Err(Error::Structure)));
        let mut b = Builder::new(); b.begin(c"");
        assert!(matches!(DeviceTree::parse(&b.finish()), Err(Error::Structure)));
        let mut b = Builder::new(); b.begin(c"");
        for _ in 0..MAX_DEPTH { b.begin(c"child"); }
        assert!(matches!(DeviceTree::parse(&b.finish()), Err(Error::Depth)));
        let mut b = Builder::new(); b.begin(c""); b.end(); b.begin(c""); b.end();
        assert!(matches!(DeviceTree::parse(&b.finish()), Err(Error::Structure)));
    }

    #[test]
    fn cell_counts_are_checked_before_division_or_indexing() {
        for (address, size) in [(0, 0), (3, 1), (2, 3), (u32::MAX, u32::MAX)] {
            let bytes = memory_blob(address, size, &[0]);
            assert_eq!(first_memory(DeviceTree::parse(&bytes).unwrap()), Err(Error::Cells));
        }
        let bytes = memory_blob(2, 2, &[0, 0x8000_0000, 0]);
        assert_eq!(first_memory(DeviceTree::parse(&bytes).unwrap()), Err(Error::Cells));
        let bytes = memory_blob(2, 2, &[u32::MAX, u32::MAX, 0, 1]);
        assert_eq!(first_memory(DeviceTree::parse(&bytes).unwrap()), Err(Error::Range));
    }

    #[test]
    fn zero_size_cells_are_valid_for_cpu_identifiers() {
        let bytes = memory_blob(1, 0, &[3]);
        let tree = DeviceTree::parse(&bytes).unwrap();
        let root = tree.root();
        let cells = CellConfig::for_node(root, CellConfig::ROOT_DEFAULT).unwrap();
        let reg = root.children().next().unwrap().property(c"reg").unwrap();
        assert_eq!(reg.regions(cells).unwrap().next(), Some(Region { base: 3, size: 0 }));
        // Address-only identifiers must never become an empty allocator arena.
        assert_eq!(first_memory(tree), Ok(None));
    }

    #[test]
    fn uart_register_window_checks_shift_extent_and_alignment() {
        let region = Region { base: 0x1000_0000, size: 0x100 };
        assert_eq!(uart_window(region, 0, 1), Ok(()));
        assert_eq!(uart_window(region, 2, 4), Ok(()));
        for (shift, width) in [(64, 1), (63, 1), (2, 2), (0, 4), (8, 1)] {
            assert_eq!(uart_window(region, shift, width), Err(Error::Range));
        }
        assert_eq!(uart_window(Region { base: region.base + 1, ..region }, 2, 4), Err(Error::Range));
        assert_eq!(uart_window(Region { base: u64::MAX - 3, size: 0x100 }, 0, 1), Err(Error::Range));
    }

    #[test]
    fn numeric_unit_address_order_preserves_device_indices() {
        let mut b = Builder::new(); b.begin(c"");
        for name in [c"virtio_mmio@10008000", c"virtio_mmio@10002000", c"virtio_mmio@10001000"] {
            b.begin(name); b.end();
        }
        b.end(); let bytes = b.finish();
        let nodes: Vec<_> = DeviceTree::parse(&bytes).unwrap().root().ordered_children().map(Node::name).collect();
        assert_eq!(nodes, [c"virtio_mmio@10001000", c"virtio_mmio@10002000", c"virtio_mmio@10008000"]);
    }

    #[test]
    fn equivalent_numeric_addresses_do_not_hide_distinct_names() {
        let mut b = Builder::new(); b.begin(c"");
        for name in [c"device@1", c"device@01"] { b.begin(name); b.end(); }
        b.end(); let bytes = b.finish();
        assert_eq!(DeviceTree::parse(&bytes).unwrap().root().ordered_children().map(Node::name).collect::<Vec<_>>(),
            [c"device@01", c"device@1"]);
    }

    #[test]
    fn properties_stay_with_their_own_node() {
        let mut b = Builder::new(); b.begin(c""); b.prop(c"root", &[]);
        b.begin(c"child"); b.prop(c"child", &[]); b.end(); b.end(); let bytes = b.finish();
        let root = DeviceTree::parse(&bytes).unwrap().root();
        let mut properties = root.properties(); assert_eq!(properties.next().unwrap().name(), c"root");
        assert!(properties.next().is_none()); assert!(properties.next().is_none());
        assert!(root.property(c"child").is_none());
        assert!(root.child(c"child").unwrap().property(c"child").is_some());
    }

    #[test]
    fn compatible_strings_are_bounded_and_phandles_resolve_real_nodes() {
        let mut b = Builder::new(); b.begin(c""); b.begin(c"soc");
        b.begin(c"clock@1000"); b.cells(c"phandle", &[7]);
        b.prop(c"compatible", b"vendor,clock\0generic,clock\0"); b.end(); b.end(); b.end();
        let bytes = b.finish(); let tree = DeviceTree::parse(&bytes).unwrap();
        let (clock, parent) = tree.find_phandle(7).unwrap().unwrap();
        assert_eq!(parent.name(), c"soc"); assert_eq!(clock.name(), c"clock@1000");
        assert!(clock.compatible(&[c"generic,clock"]).unwrap());
        assert!(tree.find_phandle(0).unwrap().is_none());
        let invalid = Property { name: c"compatible", bytes: b"missing-terminator" };
        assert!(matches!(invalid.strings(), Err(Error::String)));
        assert_eq!(invalid.cell(usize::MAX), Err(Error::Bounds));
    }

    #[test]
    fn multiple_64_bit_registers_advance_by_cells_once() {
        let bytes = memory_blob(2, 2, &[0, 0x8000_0000, 0, 0x1000, 1, 0x2000, 0, 0x800]);
        let root = DeviceTree::parse(&bytes).unwrap().root();
        let cells = CellConfig::for_node(root, CellConfig::ROOT_DEFAULT).unwrap();
        let values: Vec<_> = root.children_named(b"memory").next().unwrap().property(c"reg").unwrap().regions(cells).unwrap().collect();
        assert_eq!(values, [Region { base: 0x8000_0000, size: 0x1000 }, Region { base: 0x1_0000_2000, size: 0x800 }]);
    }

    #[test]
    fn mutated_blobs_never_escape_checked_views() {
        let original = memory_blob(2, 2, &[0, 0x8000_0000, 0, 0x1000]);
        for index in 0..original.len() {
            for value in [0, 1, 0x7f, 0xff] {
                let mut bytes = original.clone(); bytes[index] = value;
                if let Ok(tree) = DeviceTree::parse(&bytes) {
                    let _ = first_memory(tree);
                    let _ = tree.find_phandle(1);
                    let _ = tree.root().ordered_children().count();
                }
            }
        }
    }
}
