# ==============================================================================
# QEMU and GDB Configuration
# ==============================================================================
# This file handles QEMU setup, GDB initialization, and related debug targets.
# ==============================================================================

if(ARCH STREQUAL "x86_64")
    find_program(QEMU_X86_EXECUTABLE NAMES qemu-system-x86_64)
    if(NOT QEMU_X86_EXECUTABLE)
        message(WARNING "qemu-system-x86_64 not found. x86_64 qemu targets disabled.")
        return()
    endif()

    find_program(X86_GCC NAMES x86_64-linux-gnu-gcc)
    find_program(X86_LD NAMES x86_64-linux-gnu-ld)
    find_program(X86_OBJCOPY NAMES x86_64-linux-gnu-objcopy)
    if(NOT X86_GCC OR NOT X86_LD OR NOT X86_OBJCOPY)
        message(WARNING "x86_64 host toolchain not found (gcc/ld/objcopy). x86_64 qemu targets disabled.")
        return()
    endif()

    set(X86_BANNER_DIR ${CMAKE_BINARY_DIR}/x86_banner)
    file(MAKE_DIRECTORY ${X86_BANNER_DIR})

    set(X86_ENTRY_S ${CMAKE_SOURCE_DIR}/kernel/arch/x86_64/entry.S)
    set(X86_LD_SCRIPT ${CMAKE_SOURCE_DIR}/kernel/arch/x86_64/banner.ld)
    set(X86_BANNER_ELF ${X86_BANNER_DIR}/xv6_x86_banner.elf)
    set(X86_BANNER_PAYLOAD ${X86_BANNER_DIR}/xv6_x86_banner.payload.bin)
    set(X86_BANNER_IMAGE ${X86_BANNER_DIR}/xv6_banner.bin)
    set(X86_FULL_IMAGE ${CMAKE_BINARY_DIR}/kernel/kernel)

    add_custom_command(
        OUTPUT ${X86_BANNER_IMAGE}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${X86_BANNER_DIR}
        COMMAND ${X86_GCC} -m32 -ffreestanding -fno-pie -no-pie -fno-stack-protector -c ${X86_ENTRY_S} -o ${X86_BANNER_DIR}/entry.o
        COMMAND ${X86_LD} -m elf_i386 -nostdlib -T ${X86_LD_SCRIPT} -o ${X86_BANNER_ELF} ${X86_BANNER_DIR}/entry.o
        COMMAND ${X86_OBJCOPY} -O binary ${X86_BANNER_ELF} ${X86_BANNER_PAYLOAD}
        COMMAND python3 ${CMAKE_SOURCE_DIR}/scripts/make_linux_x86_image.py ${X86_BANNER_PAYLOAD} ${X86_BANNER_IMAGE}
        COMMAND python3 ${CMAKE_SOURCE_DIR}/scripts/verify_linux_x86_boot_header.py ${X86_BANNER_IMAGE}
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        DEPENDS ${X86_ENTRY_S} ${X86_LD_SCRIPT}
                ${CMAKE_SOURCE_DIR}/scripts/make_linux_x86_image.py
                ${CMAKE_SOURCE_DIR}/scripts/verify_linux_x86_boot_header.py
        COMMENT "Building x86_64 long-mode banner image"
    )

    add_custom_target(x86-banner-image
        DEPENDS ${X86_BANNER_IMAGE}
    )

    add_custom_target(qemu-smoke
        COMMAND ${QEMU_X86_EXECUTABLE} -machine pc -cpu qemu64 -m 512M -nographic -monitor none -serial none -debugcon stdio -no-reboot -no-shutdown -kernel ${X86_BANNER_ELF}
        DEPENDS x86-banner-image
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        COMMENT "Running x86_64 QEMU banner smoke image"
    )

    set(X86_INITRD_IMG ${CMAKE_BINARY_DIR}/fs.img)

    # x86 PCI devices: virtio-blk-pci disk + e1000 NIC
    set(X86_DISK_OPTS -drive file=${X86_INITRD_IMG},if=none,format=raw,id=x0 -device virtio-blk-pci,drive=x0)
    set(X86_NET_OPTS -netdev user,id=net0,hostfwd=tcp::2323-:23,hostfwd=tcp::8080-:80 -object filter-dump,id=net0,netdev=net0,file=packets.pcap -device e1000,netdev=net0)

    add_custom_target(qemu
        COMMAND ${QEMU_X86_EXECUTABLE} -machine pc -cpu qemu64 -m 1G -nographic -monitor none -chardev stdio,id=char0,signal=off -serial chardev:char0 -debugcon file:debugcon.log -no-reboot -no-shutdown -kernel ${X86_FULL_IMAGE} -initrd ${X86_INITRD_IMG} ${X86_DISK_OPTS} ${X86_NET_OPTS}
        DEPENDS kernel_all fs_img
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        COMMENT "Running x86_64 QEMU full kernel"
    )

    add_custom_target(qemu-serial
        COMMAND ${QEMU_X86_EXECUTABLE} -machine pc -cpu qemu64 -m 1G -nographic -monitor none -chardev stdio,id=char0,signal=off -serial chardev:char0 -no-reboot -no-shutdown -kernel ${X86_FULL_IMAGE} -initrd ${X86_INITRD_IMG} ${X86_DISK_OPTS} ${X86_NET_OPTS}
        DEPENDS kernel_all fs_img
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        COMMENT "Running x86_64 QEMU full kernel with COM1 on stdio"
    )

    add_custom_target(qemu-debugcon
        COMMAND ${QEMU_X86_EXECUTABLE} -machine pc -cpu qemu64 -m 1G -nographic -monitor none -serial none -debugcon stdio -no-reboot -no-shutdown -kernel ${X86_FULL_IMAGE} -initrd ${X86_INITRD_IMG} ${X86_DISK_OPTS} ${X86_NET_OPTS}
        DEPENDS kernel_all fs_img
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        COMMENT "Running x86_64 QEMU full kernel with debugcon output"
    )

    add_custom_command(
        OUTPUT ${CMAKE_BINARY_DIR}/.gdbinit
        COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/cmake/gdbinit_x86.in ${CMAKE_BINARY_DIR}/.gdbinit
        DEPENDS ${CMAKE_SOURCE_DIR}/cmake/gdbinit_x86.in
        COMMENT "Generating x86_64 .gdbinit"
    )

    add_custom_target(gdbinit ALL
        DEPENDS ${CMAKE_BINARY_DIR}/.gdbinit
    )

    add_custom_target(qemu-gdb
        COMMAND ${QEMU_X86_EXECUTABLE} -machine pc -cpu qemu64 -m 1G -nographic -monitor none -chardev stdio,id=char0,signal=off -serial chardev:char0 -debugcon file:debugcon.log -no-reboot -no-shutdown -kernel ${X86_FULL_IMAGE} -initrd ${X86_INITRD_IMG} ${X86_DISK_OPTS} ${X86_NET_OPTS} -S -gdb tcp::1234
        DEPENDS kernel_all fs_img gdbinit
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        COMMENT "Running x86_64 QEMU full kernel with GDB stub on :1234"
    )

    return()
endif()

find_program(QEMU_EXECUTABLE NAMES qemu-system-riscv64 qemu-system-riscv32 qemu-system-riscv)
if(NOT QEMU_EXECUTABLE)
    message(WARNING "QEMU executable not found. Please install QEMU or set the QEMU environment variable.")
    return()
endif()

set(QEMU ${QEMU_EXECUTABLE})

# Generate a unique GDB port based on user id to avoid conflicts
execute_process(
    COMMAND id -u
    OUTPUT_VARIABLE USER_ID
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
math(EXPR GDBPORT "${USER_ID} % 5000 + 25000")

# Generate QEMU GDB stub options
# Check if QEMU supports the '-gdb' option
execute_process(
    COMMAND ${QEMU} -help
    OUTPUT_VARIABLE QEMU_HELP
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
# GDB stub connection: prefer -gdb flag, fall back to deprecated -s/-p
if(QEMU_HELP MATCHES "-gdb")
    set(QEMUGDB -gdb tcp::${GDBPORT})
else()
    set(QEMUGDB -s -p ${GDBPORT})
endif()

# Default CPU count (overridden by CPUS env var)
if(NOT DEFINED ENV{CPUS} OR "$ENV{CPUS}" STREQUAL "")
    set(CPUS 6)
else()
    set(CPUS $ENV{CPUS})
endif()
if("$ENV{LAB}" STREQUAL "fs")
    set(CPUS 1)
endif()

# Network port forwarding (unique per user)
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
#
# NOTE: QEMU's -kernel option for RISC-V does NOT support gzip-compressed
# kernels. Only U-Boot's 'booti' command can decompress gzip kernels.
# Therefore, QEMU always uses uncompressed images.
# Compressed images (xv6.bin.gz, fs.img.gz) are for U-Boot deployment only.

set(QEMU_KERNEL_IMG ${CMAKE_BINARY_DIR}/kernel/xv6.bin)
set(QEMU_INITRD_IMG ${CMAKE_BINARY_DIR}/fs.img)

set(QEMUOPTS_PARAM
    ${QEMU_BIOS_OPT}
    -kernel ${QEMU_KERNEL_IMG}
    -initrd ${QEMU_INITRD_IMG}
    -m 4G
    -smp ${CPUS}
    -nographic
    -global virtio-mmio.force-legacy=false
    -drive file=fs.img,if=none,format=raw,id=x0
    -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0
    -netdev user,id=net0,hostfwd=udp::${FWDPORT1}-:2000,hostfwd=udp::${FWDPORT2}-:2001,hostfwd=tcp::2323-:23,hostfwd=tcp::2159-:2159,hostfwd=tcp::8080-:80,hostfwd=udp::6969-:69,hostfwd=tcp::5001-:5001
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
add_custom_target(print-gdbport
    COMMAND ${CMAKE_COMMAND} -E echo "[GDBPORT NUMBER]: ${GDBPORT}"
)

add_custom_target(grade
    COMMAND ${CMAKE_COMMAND} -E env
            LAB=$ENV{LAB}
            python3 "grade-lab-$ENV{LAB}"
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Running grade-lab-$ENV{LAB}"
)
