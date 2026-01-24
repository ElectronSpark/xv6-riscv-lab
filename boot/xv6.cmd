# xv6-riscv boot script for U-Boot
#
# This script loads and boots the xv6 RISC-V kernel
# Uses booti command which properly passes DTB address and fixes up memory
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

# Load filesystem image into memory (ramdisk)
echo "Loading filesystem image to ${ramdisk_addr_r}..."
if load ${devtype} ${devnum} ${ramdisk_addr_r} ${prefix}fs.img; then
	# filesize is set by U-Boot after load command
	setenv ramdisk_size ${filesize}
	echo "Filesystem image loaded at ${ramdisk_addr_r} (size: ${ramdisk_size})"
else
	echo "WARNING: Failed to load filesystem image"
	setenv ramdisk_size 0
fi

echo ""
echo "Booting xv6 kernel with booti..."
echo "  Kernel: ${xv6_addr}"
echo "  DTB: ${fdt_addr_r}"
echo ""

# Boot using booti - this properly:
# 1. Fixes up memory regions in DTB
# 2. Passes hartid in a0, DTB address in a1
# 3. Relocates DTB if needed
# xv6 now has a Linux-compatible boot header
booti ${xv6_addr} ${ramdisk_addr_r}:${ramdisk_size} ${fdt_addr_r}

# If we get here, booti failed
echo ""
echo "=========================================="
echo "ERROR: booti command returned - kernel did not start!"
echo "=========================================="
echo ""

# Recompile with:
# mkimage -C none -A riscv -T script -d /boot/xv6.cmd /boot/xv6.scr
