# ==============================================================================
# lwIP kernel port — CMakeLists.txt
# ==============================================================================
# Builds lwIP core + xv6 port glue as a kernel object library.
# ==============================================================================

set(LWIP_DIR ${CMAKE_SOURCE_DIR}/kernel/lwip)
set(LWIP_PORT_DIR ${CMAKE_SOURCE_DIR}/kernel/lwip_port)

# --------------------------------------------------------------------------
# lwIP core sources (from Filelists.cmake variables, inlined here to avoid
# the add_library targets that Filelists.cmake creates)
# --------------------------------------------------------------------------

set(LWIP_CORE_SRCS
    ${LWIP_DIR}/src/core/init.c
    ${LWIP_DIR}/src/core/def.c
    ${LWIP_DIR}/src/core/dns.c
    ${LWIP_DIR}/src/core/inet_chksum.c
    ${LWIP_DIR}/src/core/ip.c
    ${LWIP_DIR}/src/core/mem.c
    ${LWIP_DIR}/src/core/memp.c
    ${LWIP_DIR}/src/core/netif.c
    ${LWIP_DIR}/src/core/pbuf.c
    ${LWIP_DIR}/src/core/raw.c
    ${LWIP_DIR}/src/core/stats.c
    ${LWIP_DIR}/src/core/sys.c
    ${LWIP_DIR}/src/core/altcp.c
    ${LWIP_DIR}/src/core/altcp_alloc.c
    ${LWIP_DIR}/src/core/altcp_tcp.c
    ${LWIP_DIR}/src/core/tcp.c
    ${LWIP_DIR}/src/core/tcp_in.c
    ${LWIP_DIR}/src/core/tcp_out.c
    ${LWIP_DIR}/src/core/timeouts.c
    ${LWIP_DIR}/src/core/udp.c
)

set(LWIP_CORE4_SRCS
    ${LWIP_DIR}/src/core/ipv4/acd.c
    ${LWIP_DIR}/src/core/ipv4/autoip.c
    ${LWIP_DIR}/src/core/ipv4/dhcp.c
    ${LWIP_DIR}/src/core/ipv4/etharp.c
    ${LWIP_DIR}/src/core/ipv4/icmp.c
    ${LWIP_DIR}/src/core/ipv4/igmp.c
    ${LWIP_DIR}/src/core/ipv4/ip4_frag.c
    ${LWIP_DIR}/src/core/ipv4/ip4.c
    ${LWIP_DIR}/src/core/ipv4/ip4_addr.c
)

set(LWIP_API_SRCS
    ${LWIP_DIR}/src/api/api_lib.c
    ${LWIP_DIR}/src/api/api_msg.c
    ${LWIP_DIR}/src/api/err.c
    ${LWIP_DIR}/src/api/if_api.c
    ${LWIP_DIR}/src/api/netbuf.c
    ${LWIP_DIR}/src/api/netdb.c
    ${LWIP_DIR}/src/api/netifapi.c
    ${LWIP_DIR}/src/api/sockets.c
    ${LWIP_DIR}/src/api/tcpip.c
)

set(LWIP_NETIF_SRCS
    ${LWIP_DIR}/src/netif/ethernet.c
)

# --------------------------------------------------------------------------
# xv6 port / glue sources
# --------------------------------------------------------------------------

set(LWIP_PORT_SRCS
    ${LWIP_PORT_DIR}/sys_arch.c
    ${LWIP_PORT_DIR}/lwip_glue.c
    ${LWIP_PORT_DIR}/sys_socket.c
)

# --------------------------------------------------------------------------
# Combine all sources
# --------------------------------------------------------------------------

set(LWIP_ALL_SRCS
    ${LWIP_CORE_SRCS}
    ${LWIP_CORE4_SRCS}
    ${LWIP_API_SRCS}
    ${LWIP_NETIF_SRCS}
    ${LWIP_PORT_SRCS}
)

# --------------------------------------------------------------------------
# Build as kernel object library
# --------------------------------------------------------------------------

add_library(lwip OBJECT ${LWIP_ALL_SRCS})

# Include paths:
#  1. lwip_port/compat/ — shim headers (stdio.h, stdlib.h) to shadow newlib
#  2. lwip_port/ — for lwipopts.h, arch/cc.h, arch/sys_arch.h
#  3. lwip/src/include/ — lwIP's own headers
#  4. kernel/inc/ — xv6 kernel headers (already in CMAKE_C_FLAGS -I)
target_include_directories(lwip PRIVATE
    ${LWIP_PORT_DIR}/compat
    ${LWIP_PORT_DIR}
    ${LWIP_DIR}/src/include
)

# Suppress warnings in lwIP code that would fail with -Werror
target_compile_options(lwip PRIVATE
    -Wno-address              # pointer comparisons in lwIP macros
    -Wno-unused-variable
    -Wno-unused-function
    -Wno-unused-but-set-variable
    -Wno-maybe-uninitialized
    -Wno-type-limits
)
