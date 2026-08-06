ENTRY(_start)

/* Programs are linked -no-pie at a fixed address, the same trade the kernel
   makes and for the same reason: one program runs at a time in ring 0, so
   there is nothing to relocate around. 16 MiB is the base of the window
   memory_init() reserves for programs; the top 256 KiB of it is the stack.
   See kernel/program.h. */
SECTIONS
{
    . = 0x1000000;

    /* First, at the load address exactly, so the kernel can read which version
       of the interface this program was built against before it runs a single
       instruction of it. KEEP() because nothing references it - the only
       reader is the loader, and the linker would otherwise discard it. */
    .koi_abi : { KEEP(*(.koi_abi)) }

    .text ALIGN(4K) : { *(.text*) }
    .rodata ALIGN(4K) : { *(.rodata*) }
    .data ALIGN(4K) : { *(.data*) }
    .bss ALIGN(4K) : { *(COMMON) *(.bss*) }

    /DISCARD/ : { *(.eh_frame*) *(.comment*) *(.note*) }
}
