# xv6-riscv TFTP boot script for U-Boot
#
# This script loads and boots the xv6 RISC-V kernel over TFTP
# Useful for rapid development - no need to copy files to SD card
#
# TFTP server setup:
#   1. Install tftpd-hpa: sudo apt install tftpd-hpa
#   2. Default TFTP root: /srv/tftp (or /var/lib/tftpboot)
#   3. Copy xv6.bin and fs.img to TFTP root
#   4. Set serverip below to your TFTP server's IP address
#

echo "=========================================="
echo "    xv6-riscv TFTP Boot Loader"
echo "=========================================="
echo ""

# Network configuration
# Use DHCP to initialize network, then override serverip

# Run DHCP to properly initialize the network interface
echo "Initializing network via DHCP..."
dhcp

# Override TFTP server IP after DHCP (change to your server)
setenv serverip 192.168.0.30

echo "Board IP: ${ipaddr}"
echo "TFTP server: ${serverip}"
echo ""

# Load orangepiEnv.txt from SD card to get fdtfile
if test -e ${devtype} ${devnum} ${prefix}orangepiEnv.txt; then
	load ${devtype} ${devnum} 0x9000000 ${prefix}orangepiEnv.txt
	env import -t 0x9000000 ${filesize}
fi

# Kernel load address - must match linker script KERNEL_BASE
setenv xv6_addr 0x200000

echo "Loading xv6 kernel from TFTP to ${xv6_addr}..."

# Load the kernel binary via TFTP
if tftpboot ${xv6_addr} xv6.bin; then
	echo "Kernel binary loaded at ${xv6_addr}"
else
	echo "ERROR: Failed to load kernel from TFTP"
	echo "Make sure xv6.bin is in your TFTP server root directory"
	exit
fi

# Load the device tree from SD card (required for proper hardware discovery)
echo "Loading device tree (fdtfile=${fdtfile})..."
if load ${devtype} ${devnum} ${fdt_addr_r} ${prefix}dtb/${fdtfile}; then
	echo "Device tree loaded at ${fdt_addr_r}"
	fdt addr ${fdt_addr_r}
	fdt resize 65536
else
	echo "WARNING: Failed to load device tree from ${prefix}dtb/${fdtfile}"
fi

# Load filesystem image from TFTP
echo "Loading filesystem image from TFTP to ${ramdisk_addr_r}..."
if tftpboot ${ramdisk_addr_r} fs.img; then
	setenv ramdisk_size ${filesize}
	echo "Filesystem image loaded at ${ramdisk_addr_r} (size: ${ramdisk_size})"
else
	echo "WARNING: Failed to load filesystem image from TFTP"
	echo "Trying to load from SD card..."
	if load ${devtype} ${devnum} ${ramdisk_addr_r} ${prefix}fs.img; then
		setenv ramdisk_size ${filesize}
		echo "Filesystem image loaded from SD card (size: ${ramdisk_size})"
	else
		echo "WARNING: No filesystem image available"
		setenv ramdisk_size 0
	fi
fi

echo ""
echo "Booting xv6 kernel with booti..."
echo "  Kernel: ${xv6_addr}"
echo "  DTB: ${fdt_addr_r}"
echo ""

# Boot using booti
booti ${xv6_addr} ${ramdisk_addr_r}:${ramdisk_size} ${fdt_addr_r}

# If we get here, booti failed
echo ""
echo "=========================================="
echo "ERROR: booti command returned - kernel did not start!"
echo "=========================================="
echo ""

# Recompile with:
# mkimage -C none -A riscv -T script -d /boot/xv6-tftp.cmd /boot/xv6-tftp.scr
