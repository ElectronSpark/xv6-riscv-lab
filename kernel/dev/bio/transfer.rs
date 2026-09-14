//! Checked sector arithmetic and shared completion accounting for block I/O.
#![forbid(unsafe_code)]

use core::sync::atomic::{AtomicI32, AtomicU32, Ordering};

pub const SECTOR_SIZE: usize = 512;
pub const MAX_BYTES: usize = 32768;
pub const MAX_SEGMENTS: usize = 128;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum InvalidTransfer {
    Empty,
    Segment,
    Size,
    Overflow,
}

#[derive(Clone, Copy)]
pub struct SegmentLayout {
    pub has_page: bool,
    pub offset: usize,
    pub len: usize,
}

/// All block drivers consume 512-byte sector addresses. `block_shift`
/// constrains the transfer alignment, not the units of the starting sector.
pub struct TransferLayout {
    sector: u64,
    bytes: usize,
}

impl TransferLayout {
    pub fn new(
        sector: u64,
        block_shift: u16,
        bytes: usize,
        segments: impl IntoIterator<Item = SegmentLayout>,
    ) -> Result<Self, InvalidTransfer> {
        // checked_shl only checks the shift count; it permits high bits to
        // overflow to zero. Bound the supported logical block size first.
        if block_shift > 3 {
            return Err(InvalidTransfer::Size);
        }
        let block_size = SECTOR_SIZE << block_shift;
        if bytes == 0 || bytes > MAX_BYTES || bytes % block_size != 0 {
            return Err(InvalidTransfer::Size);
        }
        let mut total = 0usize;
        let mut count = 0;
        for segment in segments {
            count += 1;
            if count > MAX_SEGMENTS
                || !segment.has_page
                || segment.len == 0
                || segment.len % block_size != 0
                || segment
                    .offset
                    .checked_add(segment.len)
                    .is_none_or(|end| end > 4096)
            {
                return Err(InvalidTransfer::Segment);
            }
            total = total
                .checked_add(segment.len)
                .ok_or(InvalidTransfer::Overflow)?;
        }
        if count == 0 {
            return Err(InvalidTransfer::Empty);
        }
        if total != bytes {
            return Err(InvalidTransfer::Size);
        }
        sector
            .checked_add((bytes / SECTOR_SIZE) as u64)
            .ok_or(InvalidTransfer::Overflow)?;
        Ok(Self { sector, bytes })
    }

    pub fn sector_at(&self, completed: usize) -> Option<u64> {
        (completed < self.bytes && completed % SECTOR_SIZE == 0)
            .then(|| self.sector + (completed / SECTOR_SIZE) as u64)
    }
}

/// One submission sentinel plus one count per issued segment. Keeping the
/// sentinel until the iterator is dropped prevents an early interrupt from
/// publishing completion while submission still accesses the request.
#[repr(C, align(8))]
pub struct CompletionState {
    pending: AtomicU32,
    completed: AtomicU32,
    error: AtomicI32,
}

#[derive(Debug, PartialEq, Eq)]
pub struct TransferResult {
    pub bytes: u32,
    pub error: i32,
}

impl CompletionState {
    pub const fn new() -> Self {
        Self {
            pending: AtomicU32::new(1),
            completed: AtomicU32::new(0),
            error: AtomicI32::new(0),
        }
    }

    pub fn reserve(&self) {
        let before = self.pending.fetch_add(1, Ordering::Relaxed);
        assert!(before > 0 && before <= MAX_SEGMENTS as u32);
    }

    pub fn finish(&self, bytes: usize, error: i32) -> Option<TransferResult> {
        if error != 0 {
            let _ = self
                .error
                .compare_exchange(0, error, Ordering::Relaxed, Ordering::Relaxed);
        }
        self.completed.fetch_add(bytes as u32, Ordering::Relaxed);
        // The final Acquire observes each preceding segment's byte/error
        // publication; callers publish the legacy fields before waking waiters.
        let before = self.pending.fetch_sub(1, Ordering::AcqRel);
        assert!(before > 0);
        (before == 1).then(|| TransferResult {
            bytes: self.completed.load(Ordering::Relaxed),
            error: self.error.load(Ordering::Relaxed),
        })
    }
}

const _: () = assert!(core::mem::size_of::<CompletionState>() == 16);

#[cfg(test)]
mod tests {
    use super::*;
    fn segment(len: usize) -> SegmentLayout {
        SegmentLayout {
            has_page: true,
            offset: 0,
            len,
        }
    }

    #[test]
    fn unequal_segments_advance_in_sector_units() {
        let layout =
            TransferLayout::new(17, 0, 2048, [segment(512), segment(1024), segment(512)]).unwrap();
        assert_eq!(layout.sector_at(0), Some(17));
        assert_eq!(layout.sector_at(512), Some(18));
        assert_eq!(layout.sector_at(1536), Some(20));
        assert_eq!(layout.sector_at(2048), None);
        assert_eq!(layout.sector_at(1), None);
    }

    #[test]
    fn logical_block_size_does_not_change_sector_units() {
        let layout = TransferLayout::new(8, 1, 2048, [segment(1024), segment(1024)]).unwrap();
        assert_eq!(layout.sector_at(1024), Some(10));
        assert!(TransferLayout::new(0, 1, 1024, [segment(512), segment(512)]).is_err());
    }

    #[test]
    fn invalid_lengths_pages_and_ranges_are_rejected() {
        for bad in [
            segment(0),
            segment(513),
            SegmentLayout {
                has_page: false,
                ..segment(512)
            },
            SegmentLayout {
                offset: 4000,
                ..segment(512)
            },
            SegmentLayout {
                offset: usize::MAX,
                ..segment(512)
            },
        ] {
            assert!(TransferLayout::new(0, 0, 512, [bad]).is_err());
        }
        assert!(TransferLayout::new(0, 0, 1024, [segment(512)]).is_err());
        for shift in [4, 55, 63, 64, u16::MAX] {
            assert!(TransferLayout::new(0, shift, 512, [segment(512)]).is_err());
        }
        assert!(TransferLayout::new(u64::MAX, 0, 512, [segment(512)]).is_err());
        assert!(TransferLayout::new(0, 0, 512, []).is_err());
    }

    #[test]
    fn submission_sentinel_prevents_early_completion() {
        let state = CompletionState::new();
        state.reserve();
        assert_eq!(state.finish(512, 0), None);
        state.reserve();
        assert_eq!(state.finish(1024, 0), None);
        assert_eq!(
            state.finish(0, 0),
            Some(TransferResult {
                bytes: 1536,
                error: 0
            })
        );
    }

    #[test]
    fn errors_wait_for_all_parts_and_keep_first_error() {
        let state = CompletionState::new();
        state.reserve();
        state.reserve();
        assert_eq!(state.finish(0, 0), None);
        assert_eq!(state.finish(0, -5), None);
        assert_eq!(
            state.finish(0, -22),
            Some(TransferResult {
                bytes: 0,
                error: -5
            })
        );
    }

    #[test]
    fn concurrent_completion_publishes_all_bytes_once() {
        let state = std::sync::Arc::new(CompletionState::new());
        for _ in 0..8 {
            state.reserve();
        }
        assert_eq!(state.finish(0, 0), None);
        let barrier = std::sync::Arc::new(std::sync::Barrier::new(8));
        let threads: Vec<_> = (0..8)
            .map(|_| {
                let state = state.clone();
                let barrier = barrier.clone();
                std::thread::spawn(move || {
                    barrier.wait();
                    state.finish(512, 0)
                })
            })
            .collect();
        let completed: Vec<_> = threads
            .into_iter()
            .filter_map(|thread| thread.join().unwrap())
            .collect();
        assert_eq!(
            completed,
            [TransferResult {
                bytes: 4096,
                error: 0
            }]
        );
    }
}
