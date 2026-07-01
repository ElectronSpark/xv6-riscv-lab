PHDRS
{
  text PT_LOAD FILEHDR PHDRS FLAGS(5);
  dynamic PT_DYNAMIC FLAGS(4);
}

SECTIONS
{
  . = SIZEOF_HEADERS;

  .hash : { *(.hash) } :text
  .gnu.hash : { *(.gnu.hash) } :text
  .dynsym : { *(.dynsym) } :text
  .dynstr : { *(.dynstr) } :text
  .gnu.version : { *(.gnu.version) } :text
  .gnu.version_d : { *(.gnu.version_d) } :text
  .dynamic : { *(.dynamic) } :text :dynamic
  .rodata : { *(.rodata*) } :text
  .text : ALIGN(16) { *(.text*) } :text

  /DISCARD/ : {
    *(.note.GNU-stack)
    *(.eh_frame*)
    *(.comment)
  }
}
