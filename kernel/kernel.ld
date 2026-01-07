OUTPUT_ARCH( "riscv" )
ENTRY( _entry )

SECTIONS
{
  /*
   * ensure that entry.S / _entry is at 0x80200000,
   * where qemu's -kernel jumps.
   */
  . = 0x80200000;

  /* Keep the entry stub at the very start so qemu -kernel jumps to real code. */
  .text.entry : {
    _entry = .;
    KEEP(*(.text.entry))
  }

  .text : {
    . = ALIGN(16);
    *(.text .text.*);
    . = ALIGN(0x1000);
    _sigtrampoline = .;
    *(sig_trampsec)
    . = ALIGN(0x1000);
    ASSERT(. - _sigtrampoline == 0x1000, "error: signal trampoline larger than one page");
    PROVIDE(etext = .);
  }

  .trampoline : {
    . = ALIGN(0x1000);
    PROVIDE(_trampoline_data = .);
    *(trampdatasec)
    . = ALIGN(0x1000);
    PROVIDE(_trampoline = .);
    *(trampsec)
    . = ALIGN(0x1000);
    ASSERT(. - _trampoline == 0x1000, "error: trampoline larger than one page");
  }

  .rodata : {
    . = ALIGN(4096);
    _rodata = .;
    *(.srodata .srodata.*) /* do not need to distinguish this from .rodata */
    . = ALIGN(16);
    *(.rodata .rodata.*)
    . = ALIGN(4096);
    _rodata_end = .;
  }

  .data : {
    . = ALIGN(4096);
    _data = .;
    *(.sdata .sdata.*) /* do not need to distinguish this from .data */
    . = ALIGN(16);
    *(.data .data.*)
    . = ALIGN(4096);
    _data_end = .;
  }

  .bss : {
    . = ALIGN(16);
    _bss = .;
    *(.sbss .sbss.*) /* do not need to distinguish this from .bss */
    . = ALIGN(16);
    *(.bss .bss.*)
    . = ALIGN(4096);
    _bss_end = .;
  }

  PROVIDE(end = .);
}
