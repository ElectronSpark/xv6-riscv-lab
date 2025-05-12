set(USER_INITCODE_BIN initcode)
set(USER_INITCODE_OBJ ${USER_INITCODE_BIN}.out)
set(USER_INITCODE_DECOMPILED ${USER_INITCODE_BIN}.asm)
set(LIB_USERLIB userlib)

# init code
set(KERNEL_INITCODE_FILES ${USER_DIR}/initcode.S)
FILE(GLOB KERNEL_INITCODE_SRC ${KERNEL_INITCODE_FILES})
set_source_files_properties(${KERNEL_INITCODE_SRC} PROPERTIES LANGUAGE C)
add_executable(${USER_INITCODE_OBJ} ${KERNEL_INITCODE_SRC})
target_compile_options(${USER_INITCODE_OBJ} PRIVATE 
    -march=rv64g -nostdinc -I${CMAKE_SOURCE_DIR} -I${KERNEL_DIR})
set_target_properties(${USER_INITCODE_OBJ} PROPERTIES LINK_FLAGS
    "-N -e start -Ttext 0")

add_custom_target(${USER_INITCODE_BIN}
    COMMAND ${CMAKE_OBJCOPY} -S -O binary ${USER_INITCODE_OBJ} ${USER_INITCODE_BIN}
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "Creating init code ${USER_INITCODE_BIN}"
    DEPENDS ${USER_INITCODE_OBJ}
)

# decompile init code
add_custom_target(${USER_INITCODE_DECOMPILED}
    COMMAND ${CMAKE_OBJDUMP} -S ${USER_INITCODE_OBJ} > ${USER_INITCODE_DECOMPILED}
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "Decompiling init code binary to ${USER_INITCODE_DECOMPILED}"
    DEPENDS ${USER_INITCODE_OBJ}
)


# add user libraries
set(USER_LIB_C_FILES
    ${USER_DIR}/ulib.c
    ${USER_DIR}/usys.c
    ${USER_DIR}/printf.c
    ${USER_DIR}/umalloc.c
)
if($ENV{LAB} AND $ENV{LAB} STREQUAL "lock")
    set(USER_LIB_C_FILES 
        ${USER_LIB_C_FILES}
        ${USER_DIR}/statistics.c
    )
endif()
file(GLOB USER_LIB_FILES ${USER_LIB_C_FILES})
set_source_files_properties(USER_LIB_FILES PROPERTIES LANGUAGE C)
add_library(${LIB_USERLIB} STATIC 
    ${USER_LIB_FILES})
target_link_options(${LIB_USERLIB} PRIVATE 
    "-T ${USER_DIR}/user.ld")
target_include_directories(${LIB_USERLIB} PRIVATE ${USER_DIR})

# Add user programs
file(GLOB USER_PROGRAMS ${USER_DIR}/*.c)
# Modify user program output to match the .c file name without a suffix
foreach(PROGRAM ${USER_PROGRAMS})
    get_filename_component(PROGRAM_NAME ${PROGRAM} NAME_WE)
    add_executable(${PROGRAM_NAME} ${PROGRAM})
    target_compile_options(${PROGRAM_NAME} PRIVATE -nostdlib -ffreestanding)
    target_link_options(${PROGRAM_NAME} PRIVATE -T ${USER_DIR}/user.ld -nostdlib -ffreestanding)
endforeach()