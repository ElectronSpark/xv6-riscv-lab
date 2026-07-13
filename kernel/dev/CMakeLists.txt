# dev.c, cdev.c, blkdev.c ported to Rust (Phase 2 Wave 21); bio.c ported
# to Rust (Phase 2 Wave 22); fdt.c ported to Rust (Phase 2 Wave 23);
# nullrand.c + netdev.c ported to Rust (Phase 2 Wave 24) -- see
# kernel/dev/{dev,cdev,blkdev,bio,fdt,nullrand,netdev}.rs and
# kernel/dev/mod.rs. dev_test.c (dead code -- its one call site was
# already commented out in start_kernel.c) deleted outright in Wave 24,
# same precedent as the tmpfs/xv6fs smoketests in Waves 18/19.
add_library(dev OBJECT
    yt8531.c
    x1_emac.c
    x1_sdhci.c
)
