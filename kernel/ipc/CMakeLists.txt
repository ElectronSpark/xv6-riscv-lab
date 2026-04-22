add_library(ipc OBJECT
    ipc_util.c
    shm.c
    sem.c
    msg.c
)

target_include_directories(ipc PRIVATE ${CMAKE_BINARY_DIR}/kernel/inc)
add_dependencies(ipc generate_asm_offsets)
