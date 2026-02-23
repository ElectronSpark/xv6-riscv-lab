/*
 * user.ld — Linker script for x86_64 xv6 userlib programs
 *
 * Forces each section to start on a page boundary so that the kernel's
 * mmap-based ELF loader can map LOAD segments without overlap.
 */

OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)

SECTIONS
{
  . = 0x1000;

  .text : {
    *(.text .text.*)
    . = ALIGN(0x1000);
  }

  .rodata : {
    . = ALIGN(16);
    *(.rodata .rodata.*)
    . = ALIGN(0x1000);
  }

  .eh_frame : {
    *(.eh_frame)
    *(.eh_frame.*)
    . = ALIGN(0x1000);
  }

  . = ALIGN(0x1000);
  .data : {
    . = ALIGN(16);
    *(.data .data.*)
    . = ALIGN(0x1000);
  }

  . = ALIGN(0x1000);
  .bss : {
    . = ALIGN(16);
    *(.bss .bss.*)
    *(COMMON)
    . = ALIGN(0x1000);
  }

  PROVIDE(end = .);
}
