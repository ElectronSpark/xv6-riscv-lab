//! Checked address arithmetic for RISC-V frame records.

#![forbid(unsafe_code)]

const PAGE_SHIFT: u32 = 12;
const FRAME_SIZE: u64 = 16;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(super) struct StackBounds {
    start: u64,
    end: u64,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(super) struct FrameSlots {
    pub(super) previous_fp: u64,
    pub(super) return_address: u64,
}

impl StackBounds {
    pub(super) fn new(start: u64, end: u64) -> Option<Self> {
        if start == 0 || end.checked_sub(start)? < FRAME_SIZE {
            return None;
        }
        Some(Self { start, end })
    }

    pub(super) fn from_order(start: u64, order: i32) -> Option<Self> {
        let order = u32::try_from(order).ok()?;
        let shift = PAGE_SHIFT.checked_add(order)?;
        let size = 1u64.checked_shl(shift)?;
        Self::new(start, start.checked_add(size)?)
    }

    pub(super) fn addresses(self) -> (u64, u64) {
        (self.start, self.end)
    }

    /// `fp` points just beyond the saved previous-fp/return-address pair.
    /// The complete pair must fit the stack and meet the ABI alignment.
    pub(super) fn record(self, fp: u64) -> Option<FrameSlots> {
        if fp % FRAME_SIZE != 0 || fp > self.end {
            return None;
        }
        let start = fp.checked_sub(FRAME_SIZE)?;
        if start < self.start {
            return None;
        }
        Some(FrameSlots {
            previous_fp: start,
            return_address: fp - 8,
        })
    }
}

#[cfg(test)]
mod tests {
    use super::{FrameSlots, StackBounds};

    #[test]
    fn corrupt_initial_frame_pointer_never_yields_read_addresses() {
        let stack = StackBounds::new(0x1000, 0x2000).unwrap();
        for fp in [0, 1, 8, 15, 0x1000, 0x1008, 0x1011, 0x2001, u64::MAX] {
            assert_eq!(stack.record(fp), None, "fp={fp:#x}");
        }
    }

    #[test]
    fn both_record_words_must_fit_and_top_boundary_is_valid() {
        let stack = StackBounds::new(0x1000, 0x2000).unwrap();
        assert_eq!(
            stack.record(0x1010),
            Some(FrameSlots {
                previous_fp: 0x1000,
                return_address: 0x1008
            })
        );
        assert_eq!(
            stack.record(0x2000),
            Some(FrameSlots {
                previous_fp: 0x1ff0,
                return_address: 0x1ff8
            })
        );
        assert_eq!(
            StackBounds::new(0x1008, 0x2000).unwrap().record(0x1010),
            None
        );
        assert_eq!(
            StackBounds::new(0x1000, 0x1ff8).unwrap().record(0x2000),
            None
        );
    }

    #[test]
    fn invalid_stack_bounds_and_corrupt_orders_are_rejected() {
        for (start, end) in [(0, 0x2000), (0x1000, 0xff0), (0x1000, 0x1000), (1, 16)] {
            assert_eq!(StackBounds::new(start, end), None);
        }
        for order in [-1, i32::MIN, 52, 64, i32::MAX] {
            assert_eq!(StackBounds::from_order(0x1000, order), None);
        }
        assert_eq!(StackBounds::from_order(u64::MAX - 0xfff, 0), None);
        assert_eq!(
            StackBounds::from_order(0x1000, 2).unwrap().addresses(),
            (0x1000, 0x5000)
        );
    }
}
