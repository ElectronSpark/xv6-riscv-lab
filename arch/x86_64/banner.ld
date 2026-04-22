OUTPUT_FORMAT(elf32-i386)
OUTPUT_ARCH(i386)
ENTRY(_entry)

SECTIONS
{
  . = 0x00100000;

  .multiboot : {
    *(.multiboot)
  }

  .text.entry : {
    *(.text.entry)
  }

  .text : {
    *(.text .text.*)
  }

  .rodata : {
    *(.rodata .rodata.*)
  }

  .data : {
    *(.data .data.*)
  }

  .bss : {
    *(.bss .bss.*)
    *(COMMON)
  }
}
