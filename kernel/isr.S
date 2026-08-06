/* Interrupt stubs.
 *
 * The CPU pushes an error code for some exceptions and not for others. To give
 * the C handler one uniform frame, the no-error stubs push a zero in its place.
 * Every stub then pushes its vector number and falls into the common path,
 * which saves the general-purpose registers and hands a pointer to the whole
 * thing to interrupt_dispatch().
 *
 * Push order here must mirror INTERRUPT_FRAME in idt.h exactly: the last
 * register pushed sits at the lowest address, which is where the struct
 * starts.
 */

.section .text
.extern interrupt_dispatch

.macro ISR_NOERR vector
.global isr_stub_\vector
isr_stub_\vector:
    pushq $0
    pushq $\vector
    jmp isr_common
.endm

.macro ISR_ERR vector
.global isr_stub_\vector
isr_stub_\vector:
    pushq $\vector
    jmp isr_common
.endm

isr_common:
    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rbp
    pushq %rsi
    pushq %rdi
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    /* The System V ABI requires DF clear on entry to a C function, and an
       interrupt handler is a call injected at an arbitrary instruction
       boundary - it inherits whatever the interrupted code was using. GCC
       compiles struct copies and array fills into rep movs/stos, which run
       backwards with DF set, so a handler entered from code mid-std would
       corrupt memory silently and only when the timing lined up.
       iretq restores RFLAGS, so clearing it here does not disturb the
       interrupted code's own flag. */
    cld

    movq %rsp, %rdi
    call interrupt_dispatch

    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rdi
    popq %rsi
    popq %rbp
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax

    addq $16, %rsp          /* drop the vector and the error code */
    iretq

/* Vectors 0-31: CPU exceptions. 8, 10-14, 17, 21, 29 and 30 arrive with an
   error code already on the stack. */
ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_ERR   21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_ERR   29
ISR_ERR   30
ISR_NOERR 31

/* Vectors 32-47: the two 8259 PICs, after pic_init() remaps them here. */
ISR_NOERR 32
ISR_NOERR 33
ISR_NOERR 34
ISR_NOERR 35
ISR_NOERR 36
ISR_NOERR 37
ISR_NOERR 38
ISR_NOERR 39
ISR_NOERR 40
ISR_NOERR 41
ISR_NOERR 42
ISR_NOERR 43
ISR_NOERR 44
ISR_NOERR 45
ISR_NOERR 46
ISR_NOERR 47

/* The system call gate, vector 0x40.
 *
 * Separate from isr_common on purpose. A system call is not an interrupt that
 * happened to the program - it is a call the program made, so it keeps the
 * ordinary argument registers and returns its result in RAX rather than having
 * a whole frame saved and restored around it.
 *
 * RAX holds the function number and RDI/RSI/RDX/RCX the arguments, which is
 * one register out of step with the C convention: shifting them all along by
 * one puts the function number where a first parameter belongs and the fourth
 * argument into R8. The shifts must run in that order - each one reads a
 * register the next is about to overwrite.
 *
 * The pushq is not only for RBP. The CPU aligns RSP to 16 before pushing a
 * 40-byte interrupt frame, leaving it 8 past alignment; one push brings it
 * back, so the CALL below meets the ABI's alignment requirement. */
.global syscall_stub
syscall_stub:
    cld
    pushq %rbp
    movq %rsp, %rbp
    movq %rcx, %r8              /* argument four, before RCX is reused */
    movq %rdx, %rcx             /* argument three */
    movq %rsi, %rdx             /* argument two */
    movq %rdi, %rsi             /* argument one */
    movq %rax, %rdi             /* function number becomes parameter one */
    call syscall_dispatch
    popq %rbp
    iretq

/* Table the C side walks to fill in the IDT. */
.section .rodata
.global isr_stub_table
.align 8
isr_stub_table:
    .quad isr_stub_0,  isr_stub_1,  isr_stub_2,  isr_stub_3
    .quad isr_stub_4,  isr_stub_5,  isr_stub_6,  isr_stub_7
    .quad isr_stub_8,  isr_stub_9,  isr_stub_10, isr_stub_11
    .quad isr_stub_12, isr_stub_13, isr_stub_14, isr_stub_15
    .quad isr_stub_16, isr_stub_17, isr_stub_18, isr_stub_19
    .quad isr_stub_20, isr_stub_21, isr_stub_22, isr_stub_23
    .quad isr_stub_24, isr_stub_25, isr_stub_26, isr_stub_27
    .quad isr_stub_28, isr_stub_29, isr_stub_30, isr_stub_31
    .quad isr_stub_32, isr_stub_33, isr_stub_34, isr_stub_35
    .quad isr_stub_36, isr_stub_37, isr_stub_38, isr_stub_39
    .quad isr_stub_40, isr_stub_41, isr_stub_42, isr_stub_43
    .quad isr_stub_44, isr_stub_45, isr_stub_46, isr_stub_47

/* Nothing here needs an executable stack. */
.section .note.GNU-stack,"",@progbits
