/*
 * musl.ld — Linker script for musl-linked x86_64 xv6 user programs
 *
 * Page-aligned segments to avoid mappages remap panics.
 * Uses x86_64 SYSCALL convention (rax=nr, rdi/rsi/rdx/r10/r8/r9=args).
 */

OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

PHDRS
{
  text PT_LOAD FILEHDR PHDRS FLAGS(5);   /* R E — includes ELF + phdr */
  data PT_LOAD FLAGS(6);                  /* RW  */
  tls  PT_TLS FLAGS(4);                   /* R   */
}

SECTIONS
{
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

  .note.gnu.build-id : {
    *(.note.gnu.build-id)
  } :text

  .note.gnu.property : {
    *(.note.gnu.property)
  } :text

  . = ALIGN(0x1000);
  .data : {
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
    *(.bss .bss.*)
    *(.gnu.linkonce.b.*)
    *(COMMON)
  } :data

  . = ALIGN(8);
  PROVIDE(end = .);
  PROVIDE(_end = .);
}
