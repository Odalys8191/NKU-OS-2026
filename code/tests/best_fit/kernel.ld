OUTPUT_ARCH(riscv)
ENTRY(_start)
SECTIONS {
    . = 0x80200000;
    .text : { *(.text.entry) *(.text .text.*) }
    .rodata : { *(.rodata .rodata.*) }
    .data : {
        *(.data .data.*)
        PROVIDE(__global_pointer$ = . + 0x800);
        *(.sdata .sdata.*)
    }
    . = ALIGN(16);
    __bss_start = .;
    .bss : { *(.sbss .sbss.*) *(.bss .bss.*) *(COMMON) }
    . = ALIGN(8);
    __bss_end = .;
}
