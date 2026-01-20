# xv6.cmd - U-Boot script to load and boot xv6-riscv kernel
#
# This script is sourced from boot.cmd when xv6 boot is selected.
# It loads the xv6 kernel binary and device tree, then jumps to the kernel.
#
# Requirements:
#   - xv6.bin: Flat binary kernel at /boot/xv6.bin
#   - Device tree: /boot/dtb/${fdtfile}
#   - Kernel must be linked for KERNEL_BASE=0x200000 (Orange Pi)
#
# Boot sequence:
#   1. Load orangepiEnv.txt for fdtfile variable
#   2. Load xv6.bin to 0x200000
#   3. Load device tree to fdt_addr_r
#   4. Jump to kernel entry point
#
# xv6-riscv boot script for U-Boot
#
# This script loads and boots the xv6 RISC-V kernel
#

echo "=========================================="
echo "       xv6-riscv Boot Loader"
echo "=========================================="
echo ""

# Load orangepiEnv.txt to get fdtfile and other env variables
if test -e ${devtype} ${devnum} ${prefix}orangepiEnv.txt; then
	load ${devtype} ${devnum} 0x9000000 ${prefix}orangepiEnv.txt
	env import -t 0x9000000 ${filesize}
fi

# Kernel load address - must match linker script KERNEL_BASE
setenv xv6_addr 0x200000

echo "Loading xv6 kernel binary to ${xv6_addr}..."

# Load the flat binary directly to kernel address
if load ${devtype} ${devnum} ${xv6_addr} ${prefix}xv6.bin; then
	echo "Kernel binary loaded at ${xv6_addr}"
else
	echo "ERROR: Failed to load kernel"
	exit
fi

# Load the device tree (required for proper hardware discovery)
echo "Loading device tree (fdtfile=${fdtfile})..."
if load ${devtype} ${devnum} ${fdt_addr_r} ${prefix}dtb/${fdtfile}; then
	echo "Device tree loaded at ${fdt_addr_r}"
	fdt addr ${fdt_addr_r}
	fdt resize 65536
else
	echo "WARNING: Failed to load device tree from ${prefix}dtb/${fdtfile}"
fi

echo ""
echo "Booting xv6 kernel..."
echo "Jumping to ${xv6_addr} - kernel should print '[xv6] entry'"
echo ""

# Jump to kernel entry point
# go command: a0=addr (will be overwritten), doesn't set up a0/a1 for kernel
# We need to manually set up: a0=hartid (0), a1=dtb address
# Unfortunately go doesn't let us set registers, so kernel must handle this

go ${xv6_addr}

# If we get here, go failed
echo ""
echo "=========================================="
echo "ERROR: go command returned - kernel did not start!"
echo "=========================================="
echo ""

# Recompile with:
# mkimage -C none -A riscv -T script -d /boot/xv6.cmd /boot/xv6.scr
