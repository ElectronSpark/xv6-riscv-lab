# ==============================================================================
# QEMU and GDB Configuration
# ==============================================================================
# This file handles QEMU setup, GDB initialization, and related debug targets.
# ==============================================================================

find_program(QEMU_EXECUTABLE NAMES qemu-system-riscv64 qemu-system-riscv32 qemu-system-riscv)
if(NOT QEMU_EXECUTABLE)
    message(WARNING "QEMU executable not found. Please install QEMU or set the QEMU environment variable.")
    return()
endif()

set(QEMU ${QEMU_EXECUTABLE})

# Generate a unique GDB port based on user id
execute_process(
    COMMAND bash -c "expr \$(id -u) % 5000 + 25000"
    OUTPUT_VARIABLE GDBPORT
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

# Generate QEMU GDB stub options
# Check if QEMU supports the '-gdb' option
execute_process(
    COMMAND ${QEMU} -help
    OUTPUT_VARIABLE QEMU_HELP
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(QEMU_HELP MATCHES "-gdb")
    set(QEMUGDB -gdb tcp::${GDBPORT})
else()
    set(QEMUGDB -s -p ${GDBPORT})
endif()

# Set default CPUS
if(NOT DEFINED ENV{CPUS} OR "$ENV{CPUS}" STREQUAL "")
    set(CPUS 6)
else()
    set(CPUS $ENV{CPUS})
endif()

# If LAB is fs, set CPUS to 1
if(DEFINED ENV{LAB} AND "$ENV{LAB}" STREQUAL "fs")
    set(CPUS 1)
endif()

# Generate unique FWDPORT1 and FWDPORT2 using CMake math and user id
execute_process(
    COMMAND id -u
    OUTPUT_VARIABLE USER_ID
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
math(EXPR FWDPORT1 "${USER_ID} % 5000 + 25999")
math(EXPR FWDPORT2 "${USER_ID} % 5000 + 30999")

# Configure OpenSBI BIOS option for QEMU
if(OPENSBI_MODE STREQUAL "none")
    set(QEMU_BIOS_OPT -bios none)
elseif(OPENSBI_MODE STREQUAL "default")
    set(QEMU_BIOS_OPT -bios default)
elseif(OPENSBI_MODE STREQUAL "build" OR OPENSBI_MODE STREQUAL "external")
    set(QEMU_BIOS_OPT -bios ${OPENSBI_PATH})
endif()

# Compose QEMU options
# Use xv6.bin flat binary with Linux boot header (unified with Orange Pi)
# Load fs.img as initrd/ramdisk - appears in FDT for ramdisk driver
set(QEMUOPTS_PARAM
    ${QEMU_BIOS_OPT}
    -kernel ${CMAKE_BINARY_DIR}/kernel/xv6.bin
    -initrd ${CMAKE_BINARY_DIR}/fs.img
    -m 1024M
    -smp ${CPUS}
    -nographic
    -global virtio-mmio.force-legacy=false
    -drive file=fs.img,if=none,format=raw,id=x0
    -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0
    -drive file=fs0.img,if=none,format=raw,id=x1
    -device virtio-blk-device,drive=x1,bus=virtio-mmio-bus.1
    -netdev user,id=net0,hostfwd=udp::${FWDPORT1}-:2000,hostfwd=udp::${FWDPORT2}-:2001
    -object filter-dump,id=net0,netdev=net0,file=packets.pcap
    -device e1000,netdev=net0,bus=pcie.0
)

set(QEMUOPTS
    -machine virt
    ${QEMUOPTS_PARAM}
)

set(QEMUOPTS_DTB
    -machine virt,dumpdtb=${CMAKE_BINARY_DIR}/virt.dtb
    ${QEMUOPTS_PARAM}
)

# Base dependencies for QEMU targets
# Use kernel_with_symbols which has embedded symbols
# kernel_all generates xv6.bin (the flat binary with Linux boot header)
set(QEMU_BASE_DEPS
    kernel_all
    kernel_with_symbols
    fs_img
)

# ==============================================================================
# QEMU Targets
# ==============================================================================

add_custom_target(qemu-dts
    COMMAND ${QEMU} ${QEMUOPTS_DTB}
    COMMAND dtc -o virt.dts -O dts -I dtb ${CMAKE_BINARY_DIR}/virt.dtb
    DEPENDS ${QEMU_BASE_DEPS}
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Generate the device tree file"
)

add_custom_target(qemu
    COMMAND ${QEMU} ${QEMUOPTS}
    DEPENDS ${QEMU_BASE_DEPS}
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Running QEMU with kernel and fs.img"
)

# Add OpenSBI dependency if building from source
if(OPENSBI_MODE STREQUAL "build")
    add_dependencies(qemu-dts ${OPENSBI_DEPENDENCY})
    add_dependencies(qemu ${OPENSBI_DEPENDENCY})
endif()

# ==============================================================================
# GDB Configuration
# ==============================================================================

# Generate .gdbinit from template
add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/.gdbinit
    COMMAND sed "s/:1234/:${GDBPORT}/" < ${CMAKE_SOURCE_DIR}/.gdbinit.tmpl-riscv > ${CMAKE_BINARY_DIR}/.gdbinit
    COMMAND echo "b __panic_end" >> ${CMAKE_BINARY_DIR}/.gdbinit
    COMMAND echo "thread 1" >> ${CMAKE_BINARY_DIR}/.gdbinit
    DEPENDS ${CMAKE_SOURCE_DIR}/.gdbinit.tmpl-riscv
    COMMENT "Generating .gdbinit with unique GDB port"
)

add_custom_target(gdbinit ALL
    DEPENDS ${CMAKE_BINARY_DIR}/.gdbinit
)

# Add custom target to run QEMU with GDB stub
add_custom_target(qemu-gdb
    COMMAND ${CMAKE_COMMAND} -E echo "*** Now run 'gdb' in another window."
    COMMAND ${QEMU} ${QEMUOPTS} -S ${QEMUGDB}
    DEPENDS ${QEMU_BASE_DEPS}
            ${CMAKE_BINARY_DIR}/.gdbinit
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Running QEMU with GDB stub"
)

# Add OpenSBI dependency to qemu-gdb if building from source
if(OPENSBI_MODE STREQUAL "build")
    add_dependencies(qemu-gdb ${OPENSBI_DEPENDENCY})
endif()

# ==============================================================================
# Utility Targets
# ==============================================================================

# If LAB is net, generate SERVERPORT
if(DEFINED ENV{LAB} AND "$ENV{LAB}" STREQUAL "net")
    execute_process(
        COMMAND bash -c "expr \$(id -u) % 5000 + 25099"
        OUTPUT_VARIABLE SERVERPORT
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
endif()

add_custom_target(print-gdbport
    COMMAND ${CMAKE_COMMAND} -E echo "[GDBPORT NUMBER]: ${GDBPORT}"
)

add_custom_target(grade
    COMMAND ${CMAKE_COMMAND} -E env 
            LAB=$ENV{LAB}
            python3 "grade-lab-util"
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMAND_EXPAND_LISTS
    COMMENT "run grad-lab-util"
)
