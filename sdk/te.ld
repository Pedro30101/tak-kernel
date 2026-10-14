ENTRY(_start)

PHDRS
{
    header PT_LOAD FLAGS(4);
    text PT_LOAD FLAGS(5);
    data PT_LOAD FLAGS(6);
}

SECTIONS
{
    . = 0x40000000 - 52;
    .te_hdr : { KEEP(*(.te_hdr)) } :header
    . = 0x40000000;
    .text : { *(.text._start) *(.text*) } :text
    .rodata : { *(.rodata*) } :text
    . = ALIGN(4096);
    __te_data_start = .;
    .data : { *(.data*) } :data
    __te_fend = .;
    .bss : { *(.bss*) *(COMMON) } :data
    . = ALIGN(16);
    __te_end = .;
    /DISCARD/ : { *(.note*) *(.comment) *(.eh_frame*) }
}
