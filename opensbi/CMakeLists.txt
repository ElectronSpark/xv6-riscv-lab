# ==============================================================================
# OpenSBI Build Configuration
# ==============================================================================
# This file handles downloading and building OpenSBI from source, including
# the libsbi static library for SBI ecall interface.
#
# Usage:
#   add_subdirectory(opensbi)
#
# Exported targets:
#   opensbi::libsbi      - The libsbi.a static library
#   opensbi::libplatsbi  - The platform-specific libplatsbi.a (optional)
#
# Exported variables:
#   OPENSBI_FW_JUMP      - Path to fw_jump.elf firmware
#   OPENSBI_INCLUDE_DIR  - Path to OpenSBI include directory
#   OPENSBI_BUILT        - TRUE if OpenSBI was built
# ==============================================================================

include(ExternalProject)

# Configuration options
set(OPENSBI_VERSION "v1.2" CACHE STRING "OpenSBI version to build")
set(OPENSBI_PLATFORM "generic" CACHE STRING "OpenSBI platform to build for")
set(OPENSBI_FW_JUMP_ADDR "0x80200000" CACHE STRING "OpenSBI firmware jump address")

# Determine cross-compile prefix
if(DEFINED ENV{TOOLPREFIX})
    set(OPENSBI_CROSS_COMPILE "$ENV{TOOLPREFIX}")
elseif(DEFINED TOOLPREFIX)
    set(OPENSBI_CROSS_COMPILE "${TOOLPREFIX}")
else()
    # Extract from CMAKE_C_COMPILER
    get_filename_component(COMPILER_NAME ${CMAKE_C_COMPILER} NAME)
    string(REGEX REPLACE "gcc$" "" OPENSBI_CROSS_COMPILE_NAME "${COMPILER_NAME}")
    get_filename_component(COMPILER_DIR ${CMAKE_C_COMPILER} DIRECTORY)
    if(COMPILER_DIR)
        set(OPENSBI_CROSS_COMPILE "${COMPILER_DIR}/${OPENSBI_CROSS_COMPILE_NAME}")
    else()
        set(OPENSBI_CROSS_COMPILE "${OPENSBI_CROSS_COMPILE_NAME}")
    endif()
endif()

message(STATUS "OpenSBI cross-compile prefix: ${OPENSBI_CROSS_COMPILE}")

# Build directories
set(OPENSBI_SOURCE_DIR ${CMAKE_CURRENT_BINARY_DIR}/src)
set(OPENSBI_BUILD_DIR ${CMAKE_CURRENT_BINARY_DIR}/build)

# Output paths (Note: path structure varies by OpenSBI version)
set(OPENSBI_FW_JUMP_PATH ${OPENSBI_BUILD_DIR}/platform/${OPENSBI_PLATFORM}/firmware/fw_jump.elf)
set(OPENSBI_FW_DYNAMIC_PATH ${OPENSBI_BUILD_DIR}/platform/${OPENSBI_PLATFORM}/firmware/fw_dynamic.elf)
set(OPENSBI_LIBSBI_PATH ${OPENSBI_BUILD_DIR}/lib/libsbi.a)
set(OPENSBI_LIBPLATSBI_PATH ${OPENSBI_BUILD_DIR}/platform/${OPENSBI_PLATFORM}/lib/libplatsbi.a)
set(OPENSBI_INCLUDE_PATH ${OPENSBI_SOURCE_DIR}/include)

# Determine number of parallel jobs
if(DEFINED CMAKE_BUILD_PARALLEL_LEVEL AND CMAKE_BUILD_PARALLEL_LEVEL GREATER 0)
    set(OPENSBI_PARALLEL_JOBS ${CMAKE_BUILD_PARALLEL_LEVEL})
else()
    include(ProcessorCount)
    ProcessorCount(NPROC)
    if(NPROC EQUAL 0)
        set(NPROC 4)
    endif()
    set(OPENSBI_PARALLEL_JOBS ${NPROC})
endif()

message(STATUS "Building OpenSBI ${OPENSBI_VERSION} for platform '${OPENSBI_PLATFORM}'")
message(STATUS "OpenSBI build directory: ${OPENSBI_BUILD_DIR}")
message(STATUS "OpenSBI parallel jobs: ${OPENSBI_PARALLEL_JOBS}")

# Download and build OpenSBI
ExternalProject_Add(opensbi_external
    GIT_REPOSITORY https://github.com/riscv-software-src/opensbi.git
    GIT_TAG ${OPENSBI_VERSION}
    GIT_SHALLOW TRUE
    GIT_PROGRESS TRUE
    SOURCE_DIR ${OPENSBI_SOURCE_DIR}
    BINARY_DIR ${OPENSBI_BUILD_DIR}
    CONFIGURE_COMMAND ""
    BUILD_COMMAND make -C ${OPENSBI_SOURCE_DIR}
        O=${OPENSBI_BUILD_DIR}
        CROSS_COMPILE=${OPENSBI_CROSS_COMPILE}
        PLATFORM=${OPENSBI_PLATFORM}
        FW_JUMP_ADDR=${OPENSBI_FW_JUMP_ADDR}
        -j${OPENSBI_PARALLEL_JOBS}
    BUILD_IN_SOURCE FALSE
    INSTALL_COMMAND ""
    BUILD_BYPRODUCTS
        ${OPENSBI_FW_JUMP_PATH}
        ${OPENSBI_FW_DYNAMIC_PATH}
        ${OPENSBI_LIBSBI_PATH}
        ${OPENSBI_LIBPLATSBI_PATH}
    LOG_DOWNLOAD ON
    LOG_BUILD ON
    LOG_OUTPUT_ON_FAILURE ON
)

# Create the include directory at configure time so CMake doesn't complain
# about non-existent paths. The actual headers will be populated when
# opensbi_external downloads the source.
file(MAKE_DIRECTORY ${OPENSBI_INCLUDE_PATH})

# Create imported target for libsbi
add_library(opensbi_libsbi STATIC IMPORTED GLOBAL)
set_target_properties(opensbi_libsbi PROPERTIES
    IMPORTED_LOCATION ${OPENSBI_LIBSBI_PATH}
    INTERFACE_INCLUDE_DIRECTORIES ${OPENSBI_INCLUDE_PATH}
)
add_dependencies(opensbi_libsbi opensbi_external)

# Create imported target for libplatsbi
add_library(opensbi_libplatsbi STATIC IMPORTED GLOBAL)
set_target_properties(opensbi_libplatsbi PROPERTIES
    IMPORTED_LOCATION ${OPENSBI_LIBPLATSBI_PATH}
)
add_dependencies(opensbi_libplatsbi opensbi_external)

# Create alias targets with namespace
add_library(opensbi::libsbi ALIAS opensbi_libsbi)
add_library(opensbi::libplatsbi ALIAS opensbi_libplatsbi)

# Export variables to parent scope
set(OPENSBI_FW_JUMP ${OPENSBI_FW_JUMP_PATH} PARENT_SCOPE)
set(OPENSBI_FW_DYNAMIC ${OPENSBI_FW_DYNAMIC_PATH} PARENT_SCOPE)
set(OPENSBI_INCLUDE_DIR ${OPENSBI_INCLUDE_PATH} PARENT_SCOPE)
set(OPENSBI_LIBSBI ${OPENSBI_LIBSBI_PATH} PARENT_SCOPE)
set(OPENSBI_LIBPLATSBI ${OPENSBI_LIBPLATSBI_PATH} PARENT_SCOPE)
set(OPENSBI_BUILT TRUE PARENT_SCOPE)
set(OPENSBI_DEPENDENCY opensbi_external PARENT_SCOPE)

# Print summary
message(STATUS "OpenSBI outputs:")
message(STATUS "  fw_jump.elf:   ${OPENSBI_FW_JUMP_PATH}")
message(STATUS "  libsbi.a:      ${OPENSBI_LIBSBI_PATH}")
message(STATUS "  libplatsbi.a:  ${OPENSBI_LIBPLATSBI_PATH}")
message(STATUS "  include dir:   ${OPENSBI_INCLUDE_PATH}")
