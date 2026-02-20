/*
 * musl.ld — Linker script for musl-linked xv6 user programs
 *
 * Differences from user.ld:
 *   - Has ENTRY(_start) for musl CRT
 *   - Includes .init/.fini sections
 *   - Includes .tdata/.tbss for TLS
 *   - Includes .init_array/.fini_array for constructors/destructors
 *   - Includes .dynamic/.got/.got.plt for dynamic linking support
 *   - Provides __bss_start and _end symbols musl expects
 */

OUTPUT_ARCH( "riscv" )
ENTRY(_start)

/*
 * Standard ELF program headers.
 * FILEHDR tells the linker to include the ELF file header in the segment.
 * PHDRS tells the linker to include the program headers in the segment.
 * This ensures the ELF header + program headers are mapped into memory
 * as part of the first PT_LOAD, so the kernel can set AT_PHDR correctly.
 */
PHDRS
{
  text PT_LOAD FILEHDR PHDRS FLAGS(5);   /* R E — includes ELF + phdr */
  data PT_LOAD FLAGS(6);                  /* RW  */
  tls  PT_TLS FLAGS(4);                   /* R   */
}

SECTIONS
{
  /*
   * Base address 0x1000 = UVMBOTTOM (lowest valid user address in xv6).
   * With FILEHDR PHDRS, the first LOAD segment extends back to include
   * the ELF header and program headers at the segment start.
   * The segment maps: vaddr 0x1000 = file offset 0 (ELF header).
   * Actual code starts at 0x1000 + SIZEOF_HEADERS.
   */
  . = 0x1000 + SIZEOF_HEADERS;

  .text : {
    *(.text.unlikely .text.*_unlikely .text.unlikely.*)
    *(.text.exit .text.exit.*)
    *(.text.startup .text.startup.*)
    *(.text.hot .text.hot.*)
    *(.text .text.*)
    *(.stub)
    *(.gnu.linkonce.t.*)
  } :text

  .init : {
    KEEP (*(.init))
  } :text

  .fini : {
    KEEP (*(.fini))
  } :text

  .rodata : {
    . = ALIGN(16);
    *(.srodata .srodata.*)
    . = ALIGN(16);
    *(.rodata .rodata.*)
    *(.gnu.linkonce.r.*)
  } :text

  .eh_frame_hdr : {
    *(.eh_frame_hdr)
    *(.eh_frame_entry .eh_frame_entry.*)
  } :text

  .eh_frame : {
    KEEP (*(.eh_frame))
    *(.eh_frame.*)
  } :text

  .preinit_array : {
    PROVIDE_HIDDEN (__preinit_array_start = .);
    KEEP (*(.preinit_array))
    PROVIDE_HIDDEN (__preinit_array_end = .);
  } :text

  .init_array : {
    PROVIDE_HIDDEN (__init_array_start = .);
    KEEP (*(SORT_BY_INIT_PRIORITY(.init_array.*) SORT_BY_INIT_PRIORITY(.ctors.*)))
    KEEP (*(.init_array EXCLUDE_FILE (*crtbegin.o *crtbegin?.o *crtend.o *crtend?.o ) .ctors))
    PROVIDE_HIDDEN (__init_array_end = .);
  } :text

  .fini_array : {
    PROVIDE_HIDDEN (__fini_array_start = .);
    KEEP (*(SORT_BY_INIT_PRIORITY(.fini_array.*) SORT_BY_INIT_PRIORITY(.dtors.*)))
    KEEP (*(.fini_array EXCLUDE_FILE (*crtbegin.o *crtbegin?.o *crtend.o *crtend?.o ) .dtors))
    PROVIDE_HIDDEN (__fini_array_end = .);
  } :text

  . = ALIGN(0x1000);
  .data : {
    . = ALIGN(16);
    *(.sdata .sdata.*)
    . = ALIGN(16);
    *(.data .data.*)
    *(.gnu.linkonce.d.*)
  } :data

  .got : {
    *(.got)
    *(.got.plt)
  } :data

  .tdata : {
    PROVIDE_HIDDEN (__tdata_start = .);
    *(.tdata .tdata.*)
    *(.gnu.linkonce.td.*)
  } :data :tls

  .tbss : {
    *(.tbss .tbss.*)
    *(.gnu.linkonce.tb.*)
  } :data :tls

  . = ALIGN(0x1000);
  PROVIDE_HIDDEN (__bss_start = .);
  .bss : {
    . = ALIGN(16);
    *(.sbss .sbss.*)
    . = ALIGN(16);
    *(.bss .bss.*)
    *(.gnu.linkonce.b.*)
    *(COMMON)
  } :data

  . = ALIGN(8);
  PROVIDE(end = .);
  PROVIDE(_end = .);
}
