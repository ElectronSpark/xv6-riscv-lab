# dev.c, cdev.c, blkdev.c ported to Rust (Phase 2 Wave 21); bio.c ported
# to Rust (Phase 2 Wave 22); fdt.c ported to Rust (Phase 2 Wave 23) --
# see kernel/dev/{dev,cdev,blkdev,bio,fdt}.rs and kernel/dev/mod.rs.
add_library(dev OBJECT
    dev_test.c
    nullrand.c
    netdev.c
    yt8531.c
    x1_emac.c
    x1_sdhci.c
)
