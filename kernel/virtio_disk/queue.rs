//! CPU-owned descriptor allocation; device-provided IDs enter through a check.

pub(super) const NUM: usize = 64;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(super) struct DescriptorIndex(u16);

impl DescriptorIndex {
    pub(super) const fn from_device(id: u32) -> Option<Self> {
        if id < NUM as u32 {
            Some(Self(id as u16))
        } else {
            None
        }
    }

    pub(super) const fn slot(self) -> usize {
        self.0 as usize
    }

    pub(super) const fn raw(self) -> u16 {
        self.0
    }
}

/// The three descriptors owned by one submitted block request.
/// Kept in CPU metadata so cleanup never follows device-visible links.
#[derive(Debug)]
pub(super) struct DescriptorChain([DescriptorIndex; 3]);

impl DescriptorChain {
    pub(super) fn head(&self) -> DescriptorIndex {
        self.0[0]
    }

    pub(super) fn indices(&self) -> [DescriptorIndex; 3] {
        self.0
    }
}

pub(super) struct DescriptorPool {
    free: [bool; NUM],
    slots: [DescriptorIndex; NUM],
    available: usize,
}

impl DescriptorPool {
    pub(super) const fn new() -> Self {
        let mut slots = [DescriptorIndex(0); NUM];
        let mut i = 0;
        while i < NUM {
            slots[i] = DescriptorIndex(i as u16);
            i += 1;
        }
        Self {
            free: [true; NUM],
            slots,
            available: NUM,
        }
    }

    pub(super) fn allocate(&mut self) -> Option<DescriptorChain> {
        if self.available < 3 {
            return None;
        }
        let indices = core::array::from_fn(|_| {
            self.available -= 1;
            let index = self.slots[self.available];
            self.free[index.slot()] = false;
            index
        });
        Some(DescriptorChain(indices))
    }

    pub(super) fn release(&mut self, chain: DescriptorChain) {
        // A chain can only be constructed by allocate; consuming it prevents
        // a caller from releasing it twice without unsafe code.
        for index in chain.0 {
            assert!(!self.free[index.slot()], "virtio descriptor double free");
            self.free[index.slot()] = true;
            self.slots[self.available] = index;
            self.available += 1;
        }
    }

    pub(super) fn has_chain(&self) -> bool {
        self.available >= 3
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn device_ids_are_checked_before_indexing() {
        for id in 0..NUM as u32 {
            assert_eq!(
                DescriptorIndex::from_device(id).unwrap().slot(),
                id as usize
            );
            assert_eq!(DescriptorIndex::from_device(id).unwrap().raw() as u32, id);
        }
        for id in [NUM as u32, u16::MAX as u32, u32::MAX] {
            assert!(DescriptorIndex::from_device(id).is_none());
        }
    }

    #[test]
    fn exhaustion_preserves_the_last_descriptor_and_reuses_chains() {
        let mut pool = DescriptorPool::new();
        let mut seen = [false; NUM];
        let mut chains = Vec::new();
        while let Some(chain) = pool.allocate() {
            assert_eq!(chain.head(), chain.indices()[0]);
            for index in chain.indices() {
                assert!(!seen[index.slot()]);
                seen[index.slot()] = true;
            }
            chains.push(chain);
        }
        assert_eq!(chains.len(), NUM / 3);
        assert_eq!(pool.available, NUM % 3);
        assert!(!pool.has_chain());
        assert!(pool.allocate().is_none());
        assert_eq!(pool.available, NUM % 3);
        for chain in chains.into_iter().rev() {
            pool.release(chain);
        }
        assert_eq!(pool.available, NUM);
        assert!(pool.free.into_iter().all(|free| free));
        assert!(pool.has_chain());
    }

    #[test]
    fn repeated_out_of_order_completion_preserves_unique_ownership() {
        let mut pool = DescriptorPool::new();
        let mut pending = Vec::new();
        for round in 0..1000 {
            while let Some(chain) = pool.allocate() {
                pending.push(chain);
            }
            let chosen = round % pending.len();
            pool.release(pending.swap_remove(chosen));
            let replacement = pool.allocate().unwrap();
            for index in replacement.indices() {
                assert!(pending
                    .iter()
                    .all(|chain| !chain.indices().contains(&index)));
            }
            pending.push(replacement);
        }
        for chain in pending {
            pool.release(chain);
        }
        assert_eq!(pool.available, NUM);
    }
}
