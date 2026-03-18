set(MM_SOURCES
    kalloc.c
    page.c
    folio.c
    buffer.c
    slab.c
    sysmm.c
    pcache.c
    early_allocator.c
    vm.c
    rmap.c
    shrinker.c
    mm_watermark.c
    oom_kill.c
)

add_library(mm OBJECT ${MM_SOURCES})
