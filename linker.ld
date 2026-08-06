ENTRY(kernel_main)

/* The kernel is linked -no-pie at a fixed physical address and the bootloader
   places it there with AllocateAddress. 1 MiB is conventional memory on every
   UEFI machine, below anything the firmware places itself. */
SECTIONS
{
    . = 0x100000;

    kernel_image_start = .;

    .text ALIGN(4K) : { *(.text*) }
    .rodata ALIGN(4K) : { *(.rodata*) }
    .data ALIGN(4K) : { *(.data*) }

    /* .bss occupies no space in the file. The bootloader zeroes the whole
       PT_LOAD span, which is what makes p_memsz > p_filesz safe here. */
    .bss ALIGN(4K) : { *(COMMON) *(.bss*) }

    kernel_image_end = .;

    /DISCARD/ : { *(.eh_frame*) *(.comment*) *(.note*) }
}
