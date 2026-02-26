set(MM_SOURCES
    kalloc.c
    page.c
    slab.c
    sysmm.c
    pcache.c
    early_allocator.c
    vm.c
    rmap.c
)

add_library(mm OBJECT ${MM_SOURCES})
