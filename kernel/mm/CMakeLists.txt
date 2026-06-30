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

if(XV6_KASAN)
    list(APPEND MM_SOURCES kasan.c)
endif()

add_library(mm OBJECT ${MM_SOURCES})

if(XV6_KASAN)
    set_source_files_properties(kasan.c PROPERTIES
        COMPILE_OPTIONS "-fno-sanitize=kernel-address")
endif()
