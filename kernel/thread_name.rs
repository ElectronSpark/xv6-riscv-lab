//! Thread names read by diagnostics while the owning thread may rename itself.
//!
//! Each byte is atomic, so panic and scheduler paths need neither a lock nor a
//! borrowed C string into mutable thread storage. A snapshot may contain bytes
//! from adjacent renames; it is a diagnostic label, not a coherent identity.
//! The final byte is always NUL, including during concurrent updates.

#![forbid(unsafe_code)]

use core::ffi::CStr;
use core::fmt::{self, Write};
use core::sync::atomic::{AtomicU8, Ordering};

const NAME_CAPACITY: usize = 16;

/// Same size and alignment as the former C `char name[16]` field.
/// There is deliberately no raw buffer accessor or `Copy` implementation.
#[repr(transparent)]
pub(crate) struct AtomicName {
    bytes: [AtomicU8; NAME_CAPACITY],
}

impl AtomicName {
    pub(crate) const fn empty() -> Self {
        Self { bytes: [const { AtomicU8::new(0) }; NAME_CAPACITY] }
    }

    pub(crate) fn set(&self, name: &CStr) {
        let bytes = crate::string::cstr_array::<NAME_CAPACITY>(name);
        for (destination, byte) in self.bytes.iter().zip(bytes) {
            // No other data is published by the name. Relaxed accesses are
            // sufficient; all stores to the final byte write zero.
            destination.store(byte as u8, Ordering::Relaxed);
        }
    }

    pub(crate) fn snapshot(&self) -> NameSnapshot {
        NameSnapshot {
            bytes: core::array::from_fn(|index| self.bytes[index].load(Ordering::Relaxed)),
        }
    }
}

/// Owned diagnostic text. Any C-string borrow is tied to this local value,
/// never to the thread or to its concurrent writers.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) struct NameSnapshot {
    bytes: [u8; NAME_CAPACITY],
}

impl NameSnapshot {
    pub(crate) fn from_c_str(name: &CStr) -> Self {
        Self { bytes: crate::string::cstr_array::<NAME_CAPACITY>(name).map(|byte| byte as u8) }
    }

    pub(crate) fn as_c_str(&self) -> &CStr {
        CStr::from_bytes_until_nul(&self.bytes).expect("thread names always end in NUL")
    }
}

impl fmt::Display for NameSnapshot {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        let name = self.as_c_str();
        match name.to_str() {
            Ok(text) => formatter.write_str(text),
            // Preserve the existing diagnostic formatter's non-UTF-8 output.
            Err(_) => {
                for &byte in name.to_bytes() {
                    formatter.write_char(byte as char)?;
                }
                Ok(())
            }
        }
    }
}

const _: () = {
    assert!(core::mem::size_of::<AtomicName>() == NAME_CAPACITY);
    assert!(core::mem::align_of::<AtomicName>() == 1);
};

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::Barrier;

    #[test]
    fn layout_and_shared_access_match_the_thread_field_contract() {
        fn shared<T: Send + Sync>() {}
        fn owned<T: Copy>() {}
        shared::<AtomicName>();
        owned::<NameSnapshot>();
        assert_eq!(core::mem::size_of::<AtomicName>(), 16);
        assert_eq!(core::mem::align_of::<AtomicName>(), 1);
    }

    #[test]
    fn names_truncate_terminate_and_snapshots_outlive_renames() {
        let name = AtomicName::empty();
        assert_eq!(name.snapshot().as_c_str(), c"");
        name.set(c"12345678901234567890");
        let old = name.snapshot();
        assert_eq!(old.as_c_str(), c"123456789012345");
        name.set(c"x");
        assert_eq!(name.snapshot().as_c_str(), c"x");
        assert_eq!(name.snapshot().bytes[2..], [0; 14]);
        name.set(c"");
        assert_eq!(name.snapshot().as_c_str(), c"");
        assert_eq!(old.as_c_str(), c"123456789012345");
    }

    #[test]
    fn formatting_handles_utf8_and_arbitrary_name_bytes() {
        assert_eq!(format!("{}", NameSnapshot::from_c_str(c"thread")), "thread");
        assert_eq!(format!("{}", NameSnapshot::from_c_str(c"日本語")), "日本語");
        let bytes = CStr::from_bytes_with_nul(b"\xff\0").unwrap();
        assert_eq!(format!("{}", NameSnapshot::from_c_str(bytes)), "ÿ");
    }

    #[test]
    fn concurrent_renames_keep_every_snapshot_bounded_and_owned() {
        let name = AtomicName::empty();
        let barrier = Barrier::new(3);
        std::thread::scope(|scope| {
            for (long, short) in [(c"AAAAAAAAAAAAAAA", c"A"), (c"BBBBBBBBBBBBBBB", c"")] {
                let name = &name;
                let barrier = &barrier;
                scope.spawn(move || {
                    barrier.wait();
                    for _ in 0..10_000 {
                        name.set(long);
                        name.set(short);
                    }
                });
            }
            barrier.wait();
            for _ in 0..10_000 {
                let snapshot = name.snapshot();
                let saved = snapshot;
                assert_eq!(snapshot.bytes[15], 0);
                assert!(snapshot.as_c_str().to_bytes().len() <= 15);
                assert!(snapshot.bytes.iter().all(|byte| matches!(byte, 0 | b'A' | b'B')));
                std::thread::yield_now();
                assert_eq!(snapshot, saved);
            }
        });
    }
}
