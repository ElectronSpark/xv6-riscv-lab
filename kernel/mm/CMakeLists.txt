add_library(mm OBJECT
    kalloc.c
    page.c
    slab.c
    vm.c
    sysmm.c
    pcache.c
    early_allocator.c
)
