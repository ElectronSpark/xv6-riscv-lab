# ==============================================================================
# Doxygen Documentation Configuration
# ==============================================================================
# This file handles Doxygen documentation generation.
# Enable with: cmake -DBUILD_DOCUMENTATION=ON ..
# Build with:  make doc
# ==============================================================================

option(BUILD_DOCUMENTATION "Create and install the HTML based API documentation (requires Doxygen)" OFF)

if(NOT BUILD_DOCUMENTATION)
    message(STATUS "Documentation generation disabled. Enable with -DBUILD_DOCUMENTATION=ON")
    return()
endif()

find_package(Doxygen)

if(NOT DOXYGEN_FOUND)
    message(WARNING "Doxygen not found. Documentation will not be built.")
    return()
endif()

set(DOXYGEN_IN ${CMAKE_SOURCE_DIR}/Doxyfile)
set(DOXYGEN_OUT ${CMAKE_BINARY_DIR}/Doxyfile)

configure_file(${DOXYGEN_IN} ${DOXYGEN_OUT} @ONLY)
message(STATUS "Doxygen build started")

add_custom_target(doc
    COMMAND ${DOXYGEN_EXECUTABLE} ${DOXYGEN_OUT}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Generating API documentation with Doxygen"
    VERBATIM
)

message(STATUS "Documentation target 'doc' created. Run 'make doc' to generate documentation.")
