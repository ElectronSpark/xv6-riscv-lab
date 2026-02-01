if(NOT DEFINED ENV{LAB})
    message(FATAL_ERROR "Environment variable LAB is not set")
else()
    message(STATUS "Building user programs for lab: $ENV{LAB}")
endif()

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
"-N -e start -Ttext 0x1000")

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
set(USER_LIB_C_SRC
    ${USER_DIR}/ulib.c
    ${USER_DIR}/printf.c
    ${USER_DIR}/umalloc.c
)
if($ENV{LAB} STREQUAL "lock")
    set(USER_LIB_C_SRC 
        ${USER_LIB_C_SRC}
        ${USER_DIR}/statistics.c
    )
endif()

set_source_files_properties(${USER_LIB_C_SRC} PROPERTIES LANGUAGE C)

# Add a custom command to generate usys.S from usys.pl
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/usys.S
    COMMAND perl ${USER_DIR}/usys.pl > ${CMAKE_CURRENT_BINARY_DIR}/usys.S
    DEPENDS ${USER_DIR}/usys.pl
    COMMENT "Generating usys.S from usys.pl"
)

# Define a custom target to ensure usys.S is generated before anything else
add_custom_target(
    generate_usys ALL
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/usys.S
)

# Mark usys.S as a generated file and include it in the user library
set(USER_LIB_S_SRC
    ${CMAKE_CURRENT_BINARY_DIR}/usys.S
)
set_source_files_properties(${USER_LIB_S_SRC} PROPERTIES GENERATED TRUE)
set_source_files_properties(${USER_LIB_S_SRC} PROPERTIES LANGUAGE C)

# Ensure userlib depends on the generation of usys.S
add_library(${LIB_USERLIB} STATIC 
    ${USER_LIB_C_SRC}
    ${USER_LIB_S_SRC}
)
add_dependencies(${LIB_USERLIB} generate_usys)

# Ensure proper linking and include directories for userlib
target_link_options(${LIB_USERLIB} PRIVATE
    "-z max-page-size=4096 -T ${USER_DIR}/user.ld")
target_include_directories(${LIB_USERLIB} PRIVATE ${USER_DIR})

# Add user programs
set(USER_PROGRAMS_C_FILES
    ${USER_DIR}/cat.c
	${USER_DIR}/echo.c
    # @TODO:
	# ${USER_DIR}/forktest.c
	${USER_DIR}/grep.c
	${USER_DIR}/init.c
	${USER_DIR}/kill.c
	${USER_DIR}/ln.c
	${USER_DIR}/mkdir.c
	${USER_DIR}/rm.c
	${USER_DIR}/sh.c
	${USER_DIR}/stressfs.c
	${USER_DIR}/usertests.c
	${USER_DIR}/grind.c
	${USER_DIR}/wc.c
	${USER_DIR}/zombie.c
	${USER_DIR}/sleep.c
    ${USER_DIR}/sync.c
	${USER_DIR}/pingpong.c
	${USER_DIR}/primes.c
	${USER_DIR}/find.c
	${USER_DIR}/xargs.c
    ${USER_DIR}/free.c
    ${USER_DIR}/ps.c
    ${USER_DIR}/dumpchan.c
    ${USER_DIR}/dumppcache.c
    ${USER_DIR}/dumprq.c
    ${USER_DIR}/dumpinode.c
    ${USER_DIR}/testsig.c
    ${USER_DIR}/cowtest.c
    ${USER_DIR}/clonetest.c
    ${USER_DIR}/vforktest.c
    ${USER_DIR}/devtest.c
    ${USER_DIR}/mount.c
    ${USER_DIR}/umount.c
    ${USER_DIR}/mknod.c
)

set(USER_PROGRAMS_C_FILES 
    ${USER_PROGRAMS_C_FILES}
    ${USER_DIR}/bigfile.c
    ${USER_DIR}/symlinktest.c
)


if($ENV{LAB} STREQUAL "lazy")
    set(USER_PROGRAMS_C_FILES 
        ${USER_PROGRAMS_C_FILES}
        ${USER_DIR}/lazytests.c
    )
endif()

if($ENV{LAB} STREQUAL "syscall")
    set(USER_PROGRAMS_C_FILES 
        ${USER_PROGRAMS_C_FILES}
        ${USER_DIR}/attack.c
        ${USER_DIR}/attacktest.c
        ${USER_DIR}/secret.c
    )
endif()

if($ENV{LAB} STREQUAL "lock")
    set(USER_PROGRAMS_C_FILES 
        ${USER_PROGRAMS_C_FILES}
        ${USER_DIR}/stats.c
        ${USER_DIR}/kalloctest.c
	    ${USER_DIR}/bcachetest.c
    )
endif()

if($ENV{LAB} STREQUAL "traps")
    set(USER_PROGRAMS_C_FILES 
        ${USER_PROGRAMS_C_FILES}
        ${USER_DIR}/call.c
        ${USER_DIR}/bttest.c
    )
endif()

# # @TODO:
# if($ENV{LAB} STREQUAL "thread")
#     set(USER_PROGRAMS_C_FILES 
#         ${USER_PROGRAMS_C_FILES}
#         ${USER_DIR}/uthread.c
#         ${USER_DIR}/uthread_switch.S
#     )
#     set_source_files_properties(${USER_DIR}/uthread_switch.S PROPERTIES LANGUAGE ASM)
#     add_executable(uthread_switch ${USER_DIR}/uthread_switch.S)
#     target_compile_options(uthread_switch PRIVATE 
#         -march=rv64g -nostdinc -I${CMAKE_SOURCE_DIR} -I${KERNEL_DIR})
#     set_target_properties(uthread_switch PROPERTIES LINK_FLAGS
#         "-N -e main -Ttext 0")
# endif()

if($ENV{LAB} STREQUAL "pgtbl")
    set(USER_PROGRAMS_C_FILES 
        ${USER_PROGRAMS_C_FILES}
        ${USER_DIR}/pgtbltest.c
    )
endif()

if($ENV{LAB} STREQUAL "mmap")
    set(USER_PROGRAMS_C_FILES 
        ${USER_PROGRAMS_C_FILES}
        ${USER_DIR}/mmaptest.c
    )
endif()

if($ENV{LAB} STREQUAL "net")
    set(USER_PROGRAMS_C_FILES 
        ${USER_PROGRAMS_C_FILES}
        ${USER_DIR}/nettest.c
    )
endif()

# Modify user program output to match the .c file name without a suffix
foreach(PROGRAM ${USER_PROGRAMS_C_FILES})
    get_filename_component(PROGRAM_NAME ${PROGRAM} NAME_WE)
    list(APPEND USER_PROGRAMS _${PROGRAM_NAME})
    add_executable(_${PROGRAM_NAME} ${PROGRAM})
    target_link_options(_${PROGRAM_NAME} PRIVATE -z max-page-size=4096 -T ${USER_DIR}/user.ld)
    # Force linker to include all objects from userlib (including usys.S)
    target_link_libraries(_${PROGRAM_NAME} PRIVATE
        "--whole-archive" ${LIB_USERLIB} "--no-whole-archive"
    )
    set_target_properties(_${PROGRAM_NAME} PROPERTIES OUTPUT_NAME _${PROGRAM_NAME})
endforeach()

# Modify the user_programs target to include all executables
add_custom_target(user_programs ALL
    DEPENDS ${USER_PROGRAMS}
    COMMENT "Building all user programs"
)

# Pass the populated USER_PROGRAMS variable to the parent scope
set(USER_PROGRAMS ${USER_PROGRAMS} PARENT_SCOPE)
