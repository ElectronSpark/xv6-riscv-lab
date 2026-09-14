//! Checked SDHCI address/count calculations shared with the hardware driver.
#![forbid(unsafe_code)]

pub const BLOCK_SIZE: u32 = 512;
pub const SDMA_BOUNDARY: u32 = 512 * 1024;
pub const SDMA_BOUNDARY_ARGUMENT: u16 = (SDMA_BOUNDARY.trailing_zeros() - 12) as u16;
pub const CACHE_BLOCK_SIZE: usize = 64;

const _: () = {
    assert!(SDMA_BOUNDARY.is_power_of_two());
    assert!(SDMA_BOUNDARY_ARGUMENT <= 7);
    assert!(SDMA_BOUNDARY == 1 << (12 + SDMA_BOUNDARY_ARGUMENT));
};

/// SDMA has a 32-bit address register. A whole cache line must belong to this
/// transfer because invalidating a partial line can discard unrelated bytes.
/// `None` selects PIO; it does not reject an otherwise valid transfer.
pub fn sdma_address(address: u64, bytes: u32) -> Option<u32> {
    let end = address.checked_add(u64::from(bytes))?;
    if bytes == 0 || address % CACHE_BLOCK_SIZE as u64 != 0
        || bytes as usize % CACHE_BLOCK_SIZE != 0 || end > u64::from(u32::MAX) + 1
        // Some controllers request a final boundary restart before DATA_END.
        // Use PIO instead of programming a one-past-buffer (possibly 4GiB)
        // address. This also keeps each resumed DMA address inside its extent.
        || end % SDMA_BOUNDARY as u64 == 0 {
        return None;
    }
    u32::try_from(address).ok()
}

/// Check register-width conversions before configuring or issuing a command.
/// Returns the command's card address and transfer length in bytes. SDHC/eMMC
/// use sector addresses; older SD cards use a 32-bit byte address instead.
pub fn block_transfer(lba: u32, blocks: u32, byte_addressed: bool) -> Option<(u32, u32)> {
    if blocks == 0 || blocks > u16::MAX as u32 { return None; }
    let last = lba.checked_add(blocks - 1)?;
    let bytes = blocks.checked_mul(BLOCK_SIZE)?;
    let address = if byte_addressed {
        last.checked_mul(BLOCK_SIZE)?.checked_add(BLOCK_SIZE - 1)?;
        lba.checked_mul(BLOCK_SIZE)?
    } else { lba };
    Some((address, bytes))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn aligned_ranges_use_dma_without_changing_the_address() {
        for bytes in [BLOCK_SIZE, 4096] {
            assert_eq!(sdma_address(0x8000_0000, bytes), Some(0x8000_0000));
            assert_eq!(sdma_address(0x8000_0040, bytes), Some(0x8000_0040));
        }
    }

    #[test]
    fn partial_cache_lines_and_empty_transfers_use_pio() {
        for address in [0x8000_0001, 0x8000_003f] {
            assert_eq!(sdma_address(address, BLOCK_SIZE), None);
        }
        for bytes in [0, 1, BLOCK_SIZE - 1, BLOCK_SIZE + 1] {
            assert_eq!(sdma_address(0x8000_0000, bytes), None);
        }
    }

    #[test]
    fn full_dma_extent_must_fit_32_bit_addressing() {
        let limit = 1u64 << 32;
        assert_eq!(sdma_address(limit - 2 * BLOCK_SIZE as u64, BLOCK_SIZE), Some(0xffff_fc00));
        assert_eq!(sdma_address(limit - BLOCK_SIZE as u64, BLOCK_SIZE * 2), None);
        assert_eq!(sdma_address(limit, BLOCK_SIZE), None);
        assert_eq!(sdma_address(u64::MAX - CACHE_BLOCK_SIZE as u64 + 1, BLOCK_SIZE), None);
        // Exactly 4GiB also requests a boundary restart with an unrepresentable
        // one-past address; PIO retains access to the valid final block.
        assert_eq!(sdma_address(limit - BLOCK_SIZE as u64, BLOCK_SIZE), None);
    }

    #[test]
    fn exact_boundary_completion_uses_pio_but_neighboring_ranges_use_dma() {
        let boundary = SDMA_BOUNDARY as u64;
        assert_eq!(sdma_address(boundary - BLOCK_SIZE as u64, BLOCK_SIZE), None);
        assert_eq!(sdma_address(boundary - 4096, 4096), None);
        assert_eq!(sdma_address(boundary - 2 * BLOCK_SIZE as u64, BLOCK_SIZE), Some((boundary - 1024) as u32));
        assert_eq!(sdma_address(boundary, BLOCK_SIZE), Some(boundary as u32));
    }

    #[test]
    fn sector_and_byte_addressed_cards_keep_distinct_command_units() {
        assert_eq!(block_transfer(17, 2, false), Some((17, 1024)));
        assert_eq!(block_transfer(17, 2, true), Some((17 * BLOCK_SIZE, 1024)));
    }

    #[test]
    fn count_register_bounds_are_checked_before_multiplication() {
        assert_eq!(block_transfer(0, 0, false), None);
        assert_eq!(block_transfer(0, u16::MAX as u32, false), Some((0, u16::MAX as u32 * BLOCK_SIZE)));
        assert_eq!(block_transfer(0, u16::MAX as u32 + 1, false), None);
        assert_eq!(block_transfer(0, u32::MAX, false), None);
    }

    #[test]
    fn sector_range_overflow_is_rejected_without_losing_the_last_sector() {
        assert_eq!(block_transfer(u32::MAX, 1, false), Some((u32::MAX, BLOCK_SIZE)));
        assert_eq!(block_transfer(u32::MAX, 2, false), None);
        assert_eq!(block_transfer(u32::MAX - 1, 2, false), Some((u32::MAX - 1, 1024)));
        assert_eq!(block_transfer(u32::MAX - 1, 3, false), None);
    }

    #[test]
    fn byte_addressed_card_checks_the_entire_transfer() {
        let last_sector = u32::MAX / BLOCK_SIZE;
        assert_eq!(block_transfer(last_sector, 1, true), Some((0xffff_fe00, BLOCK_SIZE)));
        assert_eq!(block_transfer(last_sector, 2, true), None);
        assert_eq!(block_transfer(last_sector + 1, 1, true), None);
        assert_eq!(block_transfer(last_sector - 1, 2, true), Some((0xffff_fc00, 1024)));
        assert_eq!(block_transfer(u32::MAX, 1, true), None);
    }
}
