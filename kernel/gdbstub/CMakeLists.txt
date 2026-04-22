add_library(gdbstub OBJECT gdbstub.c)
target_include_directories(gdbstub PRIVATE
    ${CMAKE_SOURCE_DIR}/kernel/inc
    ${CMAKE_SOURCE_DIR}/kernel/lwip/src/include
    ${CMAKE_SOURCE_DIR}/kernel/lwip_port
    ${CMAKE_SOURCE_DIR}/kernel/lwip_port/compat
)
