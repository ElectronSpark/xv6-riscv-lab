# xv6-riscv boot script for U-Boot
#
# This script loads and boots the xv6 RISC-V kernel
# Uses booti command which properly passes DTB address and fixes up memory
#
# Supports both compressed (.gz) and uncompressed images:
#   - xv6.bin.gz / xv6.bin (kernel)
#   - fs.img.gz / fs.img (ramdisk)
# booti automatically decompresses gzip files.
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

# Kernel addresses:
#   xv6_addr: Final kernel address (must match linker script KERNEL_BASE)
#   kernel_load_addr: Temporary address to load compressed kernel
# For compressed kernels, we load to a temp address and booti decompresses to xv6_addr
setenv xv6_addr 0x200000
setenv kernel_load_addr 0x10000000

# Try compressed kernel first, fall back to uncompressed
echo "Loading xv6 kernel..."
if load ${devtype} ${devnum} ${kernel_load_addr} ${prefix}xv6.bin.gz; then
	setenv kernel_is_compressed 1
	echo "Compressed kernel loaded at ${kernel_load_addr} (size: ${filesize})"
elif load ${devtype} ${devnum} ${xv6_addr} ${prefix}xv6.bin; then
	setenv kernel_is_compressed 0
	setenv kernel_load_addr ${xv6_addr}
	echo "Kernel binary loaded at ${xv6_addr}"
else
	echo "ERROR: Failed to load kernel (tried xv6.bin.gz and xv6.bin)"
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
# Try compressed version first, fall back to uncompressed
# NOTE: Unlike the kernel, booti does NOT decompress the initrd/ramdisk.
# We must manually decompress using U-Boot's unzip command.
setenv ramdisk_load_addr 0x30000000
echo "Loading filesystem image to ${ramdisk_addr_r}..."
if load ${devtype} ${devnum} ${ramdisk_load_addr} ${prefix}fs.img.gz; then
	setenv compressed_size ${filesize}
	echo "Compressed filesystem loaded at ${ramdisk_load_addr} (size: ${compressed_size})"
	echo "Decompressing filesystem to ${ramdisk_addr_r}..."
	unzip ${ramdisk_load_addr} ${ramdisk_addr_r}
	setenv ramdisk_size ${filesize}
	echo "Decompressed filesystem size: ${ramdisk_size}"
elif load ${devtype} ${devnum} ${ramdisk_addr_r} ${prefix}fs.img; then
	setenv ramdisk_size ${filesize}
	echo "Filesystem image loaded at ${ramdisk_addr_r} (size: ${ramdisk_size})"
else
	echo "WARNING: Failed to load filesystem image"
	setenv ramdisk_size 0
fi

echo ""
echo "Booting xv6 kernel with booti..."
echo "  Kernel load addr: ${kernel_load_addr}"
echo "  Kernel dest addr: ${xv6_addr}"
echo "  DTB: ${fdt_addr_r}"
echo ""

# Boot using booti - this properly:
# 1. Decompresses gzip-compressed kernel/initrd if needed
# 2. Fixes up memory regions in DTB
# 3. Passes hartid in a0, DTB address in a1
# 4. Relocates DTB if needed
# xv6 now has a Linux-compatible boot header
booti ${kernel_load_addr} ${ramdisk_addr_r}:${ramdisk_size} ${fdt_addr_r}

# If we get here, booti failed
echo ""
echo "=========================================="
echo "ERROR: booti command returned - kernel did not start!"
echo "=========================================="
echo ""

# Recompile with:
# mkimage -C none -A riscv -T script -d /boot/xv6.cmd /boot/xv6.scr
