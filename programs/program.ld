ENTRY(_start)

/* Programs are position-independent and linked at zero: the loader adds
   whatever address the slot it lands in starts at.
 *
 * They used to be linked -no-pie at a fixed 16 MiB, which was right while the
 * machine could hold one program at a time - there was nothing to relocate
 * around. Now a program can run another and get control back when it ends,
 * both are resident, and both cannot be at 16 MiB. See kernel/program.h.
 *
 * Nothing here is dynamically linked. There are no libraries and no symbols to
 * resolve; the only relocations are R_X86_64_RELATIVE, which say "add the load
 * address", and they exist because a table of pointers in initialised data
 * cannot be written any other way. */
SECTIONS
{
    . = 0;

    /* First, at the load address exactly, so the kernel can read which version
       of the interface this program was built against before it runs a single
       instruction of it. KEEP() because nothing references it - the only
       reader is the loader, and the linker would otherwise discard it. */
    .koi_abi : { KEEP(*(.koi_abi)) }

    .text ALIGN(4K) : { *(.text*) }
    .rodata ALIGN(4K) : { *(.rodata*) }
    .rela.dyn : { *(.rela.dyn) *(.rela*) }
    .dynamic : { *(.dynamic) }
    .data ALIGN(4K) : { *(.data*) }
    .bss ALIGN(4K) : { *(COMMON) *(.bss*) }

    /DISCARD/ : { *(.eh_frame*) *(.comment*) *(.note*)
                  *(.dynsym) *(.dynstr) *(.hash) *(.gnu.hash) }
}
