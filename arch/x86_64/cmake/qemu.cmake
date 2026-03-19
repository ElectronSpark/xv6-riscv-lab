# ==============================================================================
# QEMU and GDB Configuration — x86_64
# ==============================================================================
# This file handles QEMU setup, GDB initialization, and related debug targets
# for the x86_64 architecture.
# ==============================================================================

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

set(X86_ENTRY_S ${CMAKE_SOURCE_DIR}/arch/x86_64/entry.S)
set(X86_LD_SCRIPT ${CMAKE_SOURCE_DIR}/arch/x86_64/banner.ld)
set(X86_BANNER_ELF ${X86_BANNER_DIR}/xv6_x86_banner.elf)
set(X86_BANNER_PAYLOAD ${X86_BANNER_DIR}/xv6_x86_banner.payload.bin)
set(X86_BANNER_IMAGE ${X86_BANNER_DIR}/xv6_banner.bin)
set(X86_FULL_IMAGE ${CMAKE_BINARY_DIR}/kernel/kernel_with_symbols_elf)

add_custom_command(
    OUTPUT ${X86_BANNER_IMAGE}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${X86_BANNER_DIR}
    COMMAND ${X86_GCC} -m32 -ffreestanding -fno-pie -no-pie -fno-stack-protector -c ${X86_ENTRY_S} -o ${X86_BANNER_DIR}/entry.o
    COMMAND ${X86_LD} -m elf_i386 -nostdlib -T ${X86_LD_SCRIPT} -o ${X86_BANNER_ELF} ${X86_BANNER_DIR}/entry.o
    COMMAND ${X86_OBJCOPY} -O binary ${X86_BANNER_ELF} ${X86_BANNER_PAYLOAD}
    COMMAND python3 ${CMAKE_SOURCE_DIR}/arch/x86_64/scripts/make_linux_x86_image.py ${X86_BANNER_PAYLOAD} ${X86_BANNER_IMAGE}
    COMMAND python3 ${CMAKE_SOURCE_DIR}/arch/x86_64/scripts/verify_linux_x86_boot_header.py ${X86_BANNER_IMAGE}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    DEPENDS ${X86_ENTRY_S} ${X86_LD_SCRIPT}
            ${CMAKE_SOURCE_DIR}/arch/x86_64/scripts/make_linux_x86_image.py
            ${CMAKE_SOURCE_DIR}/arch/x86_64/scripts/verify_linux_x86_boot_header.py
    COMMENT "Building x86_64 long-mode banner image"
)

add_custom_target(x86-banner-image
    DEPENDS ${X86_BANNER_IMAGE}
)

add_custom_target(qemu-smoke
    COMMAND ${QEMU_X86_EXECUTABLE} -machine pc -cpu qemu64,+pcid -m 512M -nographic -monitor none -serial none -debugcon stdio -kernel ${X86_BANNER_ELF}
    DEPENDS x86-banner-image
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Running x86_64 QEMU banner smoke image"
)

# Number of CPUs for x86 SMP
if(NOT DEFINED CPUS)
    set(CPUS 6)
endif()

if(NOT DEFINED MEMORY)
    set(MEMORY 4G)
endif()

# x86 PCI devices: virtio-blk-pci disks + e1000 NIC
# iothreads move virtio-blk I/O processing off QEMU's main loop into
# dedicated host threads, enabling true parallel queue processing.
set(X86_DISK_OPTS
    -object iothread,id=iot0 -object iothread,id=iot1
    -drive file=${CMAKE_BINARY_DIR}/fs.img,if=none,format=raw,id=x0,aio=native,cache.direct=on -device virtio-blk-pci,drive=x0,num-queues=${CPUS},iothread=iot0
    -drive file=${CMAKE_BINARY_DIR}/xv6fs_test.img,if=none,format=raw,id=x1,aio=native,cache.direct=on -device virtio-blk-pci,drive=x1,num-queues=${CPUS},iothread=iot1
)
set(X86_NET_OPTS -netdev user,id=net0,hostfwd=tcp::2323-:23,hostfwd=tcp::2159-:2159,hostfwd=tcp::8080-:80,hostfwd=tcp::8443-:443,hostfwd=tcp::2222-:22 -object filter-dump,id=net0,netdev=net0,file=packets.pcap -device e1000,netdev=net0)
set(X86_VGA_OPTS -vga std)
set(X86_CMDLINE -append "root=/dev/disk0")

add_custom_target(qemu
    COMMAND ${QEMU_X86_EXECUTABLE} -machine pc -cpu qemu64,+pcid -smp ${CPUS} -m ${MEMORY} -nographic -chardev stdio,id=char0,mux=on,signal=off -mon chardev=char0,mode=readline -serial chardev:char0 -serial file:diag.log -debugcon file:debugcon.log -kernel ${X86_FULL_IMAGE} ${X86_DISK_OPTS} ${X86_NET_OPTS} ${X86_CMDLINE}
    COMMAND stty sane
    DEPENDS kernel_all fs_img
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Running x86_64 QEMU full kernel"
)

# GUI mode: QEMU window with VGA display + VNC on :5900
add_custom_target(qemu-gui
    COMMAND ${QEMU_X86_EXECUTABLE} -machine pc -cpu qemu64,+pcid -smp ${CPUS} -m ${MEMORY} ${X86_VGA_OPTS} -display gtk -chardev stdio,id=char0,mux=on,signal=off -mon chardev=char0,mode=readline -serial chardev:char0 -serial file:diag.log -debugcon file:debugcon.log -kernel ${X86_FULL_IMAGE} ${X86_DISK_OPTS} ${X86_NET_OPTS} ${X86_CMDLINE} -usb -device usb-tablet
    COMMAND stty sane
    DEPENDS kernel_all fs_img
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Running x86_64 QEMU with GUI display (VGA + mouse)"
)

# VNC mode: headless with VNC server on port 5900
add_custom_target(qemu-vnc
    COMMAND ${QEMU_X86_EXECUTABLE} -machine pc -cpu qemu64,+pcid -smp ${CPUS} -m ${MEMORY} ${X86_VGA_OPTS} -display vnc=:0 -chardev stdio,id=char0,mux=on,signal=off -mon chardev=char0,mode=readline -serial chardev:char0 -serial file:diag.log -debugcon file:debugcon.log -kernel ${X86_FULL_IMAGE} ${X86_DISK_OPTS} ${X86_NET_OPTS} ${X86_CMDLINE} -usb -device usb-tablet
    COMMAND stty sane
    DEPENDS kernel_all fs_img
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Running x86_64 QEMU with VNC display on :5900"
)

add_custom_target(qemu-serial
    COMMAND ${QEMU_X86_EXECUTABLE} -machine pc -cpu qemu64,+pcid -smp ${CPUS} -m ${MEMORY} -nographic -chardev stdio,id=char0,mux=on,signal=off -mon chardev=char0,mode=readline -serial chardev:char0 -serial file:diag.log -kernel ${X86_FULL_IMAGE} ${X86_DISK_OPTS} ${X86_NET_OPTS} ${X86_CMDLINE}
    COMMAND stty sane
    DEPENDS kernel_all fs_img
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Running x86_64 QEMU full kernel with COM1 on stdio"
)

add_custom_target(qemu-debugcon
    COMMAND ${QEMU_X86_EXECUTABLE} -machine pc -cpu qemu64,+pcid -smp ${CPUS} -m ${MEMORY} -nographic -chardev stdio,id=char0,mux=on,signal=off -mon chardev=char0,mode=readline -serial none -debugcon chardev:char0 -kernel ${X86_FULL_IMAGE} ${X86_DISK_OPTS} ${X86_NET_OPTS} ${X86_CMDLINE}
    COMMAND stty sane
    DEPENDS kernel_all fs_img
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Running x86_64 QEMU full kernel with debugcon output"
)

add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/.gdbinit
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/arch/x86_64/cmake/gdbinit_x86.in ${CMAKE_BINARY_DIR}/.gdbinit
    DEPENDS ${CMAKE_SOURCE_DIR}/arch/x86_64/cmake/gdbinit_x86.in
    COMMENT "Generating x86_64 .gdbinit"
)

add_custom_target(gdbinit ALL
    DEPENDS ${CMAKE_BINARY_DIR}/.gdbinit
)

add_custom_target(qemu-gdb
    COMMAND ${QEMU_X86_EXECUTABLE} -machine pc -cpu qemu64,+pcid -smp ${CPUS} -m ${MEMORY} -nographic -chardev stdio,id=char0,mux=on,signal=off -mon chardev=char0,mode=readline -serial chardev:char0 -serial file:diag.log -debugcon file:debugcon.log -kernel ${X86_FULL_IMAGE} ${X86_DISK_OPTS} ${X86_NET_OPTS} ${X86_CMDLINE} -S -gdb tcp::1234
    COMMAND stty sane
    DEPENDS kernel_all fs_img gdbinit
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Running x86_64 QEMU full kernel with GDB stub on :1234"
)
