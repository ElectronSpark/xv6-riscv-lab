# ==============================================================================
# Kernel daemons — CMakeLists.txt
# ==============================================================================
# Builds all kernel-resident network daemons as a single object library.
# ==============================================================================

set(DAEMON_SRCS
    ${CMAKE_CURRENT_SOURCE_DIR}/telnetd.c
    ${CMAKE_CURRENT_SOURCE_DIR}/tftpd.c
    # httpd disabled — user-space Flask handles HTTP
    # ${CMAKE_CURRENT_SOURCE_DIR}/httpd_daemon.c
    ${CMAKE_CURRENT_SOURCE_DIR}/iperfd.c
    ${CMAKE_CURRENT_SOURCE_DIR}/sntpd.c
    ${CMAKE_CURRENT_SOURCE_DIR}/netbiosd.c
    ${CMAKE_CURRENT_SOURCE_DIR}/mdnsd.c
)

add_library(daemons OBJECT ${DAEMON_SRCS})
target_include_directories(daemons PRIVATE
    ${CMAKE_SOURCE_DIR}/kernel/inc
    ${CMAKE_SOURCE_DIR}/kernel/lwip/src/include
    ${CMAKE_SOURCE_DIR}/kernel/lwip_port
    ${CMAKE_SOURCE_DIR}/kernel/lwip_port/compat
)
