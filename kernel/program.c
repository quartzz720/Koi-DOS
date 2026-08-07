#include "program.h"
#include "fat32.h"
#include "memory.h"
#include "string.h"
#include "heap.h"
#include "../include/elf.h"
#include "../include/syscall.h"
#include "console.h"
#include "serial.h"

/* Loading and running a program.
 *
 * The same ELF64 walk the bootloader does for the kernel, against a file read
 * through FAT32 instead of UEFI. include/elf.h is shared between the two so
 * the structures cannot drift.
 */

static const char* current_arguments = "";
static int program_running;

/* Where to resume when the program exits.
 *
 * SYS_EXIT is reached from inside an interrupt handler, several frames deep,
 * and has to get back out to program_run() without unwinding through any of
 * it. Saving the callee-saved registers and the stack pointer on entry and
 * restoring them on exit is the whole mechanism - a setjmp/longjmp pair with
 * no library to borrow one from. */
typedef struct {
    boot_uint64_t rsp, rbp, rbx, r12, r13, r14, r15, rip;
} RESUME_POINT;

static RESUME_POINT resume;
static int exit_code;

/* Returns 0 when saving, and 1 when arrived at through program_resume().
   Not static: the definitions below are global assembly labels, and the
   assembler has no way to satisfy a file-local declaration. The offsets in
   that assembly track RESUME_POINT above, which is why the two are kept
   next to each other. */
int program_save(RESUME_POINT* point);
__attribute__((noreturn)) void program_resume(RESUME_POINT* point);

__asm__(
".text\n"
".global program_save\n"
"program_save:\n"
"    movq (%rsp), %rax\n"        /* return address becomes the resume point */
"    movq %rax, 56(%rdi)\n"
"    leaq 8(%rsp), %rax\n"       /* stack as it will be after we return */
"    movq %rax, 0(%rdi)\n"
"    movq %rbp, 8(%rdi)\n"
"    movq %rbx, 16(%rdi)\n"
"    movq %r12, 24(%rdi)\n"
"    movq %r13, 32(%rdi)\n"
"    movq %r14, 40(%rdi)\n"
"    movq %r15, 48(%rdi)\n"
"    xorl %eax, %eax\n"
"    ret\n"
".global program_resume\n"
"program_resume:\n"
"    movq 0(%rdi), %rsp\n"
"    movq 8(%rdi), %rbp\n"
"    movq 16(%rdi), %rbx\n"
"    movq 24(%rdi), %r12\n"
"    movq 32(%rdi), %r13\n"
"    movq 40(%rdi), %r14\n"
"    movq 48(%rdi), %r15\n"
"    movl $1, %eax\n"
"    jmp *56(%rdi)\n"
);

const char* program_arguments(void) {
    return current_arguments;
}

__attribute__((noreturn)) void program_exit(int code) {
    exit_code = code;
    program_running = 0;
    /* Interrupts were left enabled by the trap gate, so nothing has to be
       re-enabled here; the abandoned interrupt frame simply goes with the
       stack we are throwing away. */
    program_resume(&resume);
}

/* Read the whole file into a buffer. Programs are small; streaming the ELF
   headers separately would buy nothing and complicate the bounds checks. */
static boot_uint8_t* read_program(VOLUME* volume, const char* path,
                                  boot_uint32_t* length) {
    FAT_ENTRY entry;
    boot_uint8_t* contents;
    boot_uint32_t offset = 0;

    if (!fat32_stat(volume, path, &entry)) return (boot_uint8_t*)0;
    if (entry.attributes & FAT_ATTRIBUTE_DIRECTORY) return (boot_uint8_t*)0;
    if (!entry.size || entry.size > PROGRAM_LIMIT - PROGRAM_BASE)
        return (boot_uint8_t*)0;

    contents = (boot_uint8_t*)kmalloc(entry.size);
    if (!contents) return (boot_uint8_t*)0;

    while (offset < entry.size) {
        boot_uint32_t got = fat32_read(volume, &entry, offset,
                                       contents + offset, entry.size - offset);
        if (!got) { kfree(contents); return (boot_uint8_t*)0; }
        offset += got;
    }
    *length = entry.size;
    return contents;
}

static int load_segments(const boot_uint8_t* contents, boot_uint32_t length,
                         boot_uint64_t* entry_point) {
    const ELF64_HEADER* header = (const ELF64_HEADER*)contents;
    const ELF64_PROGRAM_HEADER* segments;

    if (length < sizeof(ELF64_HEADER)) return 0;
    if (header->e_ident[0] != 0x7F || header->e_ident[1] != 'E' ||
        header->e_ident[2] != 'L' || header->e_ident[3] != 'F') return 0;
    if (header->e_ident[ELF_INDEX_CLASS] != ELF_CLASS_64 ||
        header->e_ident[ELF_INDEX_DATA] != ELF_DATA_LSB) return 0;
    if (header->e_type != ELF_TYPE_EXEC ||
        header->e_machine != ELF_MACHINE_X86_64) return 0;
    if (!header->e_phnum || header->e_phentsize != sizeof(ELF64_PROGRAM_HEADER))
        return 0;
    if (header->e_phoff > length ||
        (boot_uint64_t)header->e_phnum * sizeof(ELF64_PROGRAM_HEADER) >
            length - header->e_phoff) return 0;

    segments = (const ELF64_PROGRAM_HEADER*)(contents + header->e_phoff);

    for (elf_uint16_t index = 0; index < header->e_phnum; index++) {
        const ELF64_PROGRAM_HEADER* segment = &segments[index];
        if (segment->p_type != PT_LOAD || !segment->p_memsz) continue;
        /* Refusing anything outside the program window is what keeps a
           malformed or hostile file from writing over the kernel: there is no
           memory protection to fall back on. */
        if (segment->p_paddr < PROGRAM_BASE ||
            segment->p_paddr + segment->p_memsz > PROGRAM_LIMIT - PROGRAM_STACK_SIZE)
            return 0;
        if (segment->p_filesz > segment->p_memsz) return 0;
        if (segment->p_offset > length ||
            segment->p_filesz > length - segment->p_offset) return 0;

        memset((void*)(unsigned long long)segment->p_paddr, 0,
               (boot_uint64_t)segment->p_memsz);
        if (segment->p_filesz)
            memcpy((void*)(unsigned long long)segment->p_paddr,
                   contents + segment->p_offset, (boot_uint64_t)segment->p_filesz);
    }

    if (header->e_entry < PROGRAM_BASE || header->e_entry >= PROGRAM_LIMIT)
        return 0;
    *entry_point = header->e_entry;
    return 1;
}

/* Is this program built against an interface this kernel can honour?
 *
 * Checked after loading and before entering, because the header is part of the
 * loaded image - and checked in both directions. A program built for a newer
 * interface would call functions that do not exist here. A program built for
 * an older one is refused too, while the numbering is still alpha: a call
 * whose number has since changed meaning does not fail, it quietly does
 * something else, and that is far worse than refusing to start.
 *
 * Fills `reason` with something a person can act on. */
static int abi_is_acceptable(const char** reason) {
    const KOI_PROGRAM_HEADER* header =
        (const KOI_PROGRAM_HEADER*)(unsigned long long)PROGRAM_BASE;

    if (header->magic != KOI_PROGRAM_MAGIC) {
        *reason = "not a Koi-DOS program, or built before programs carried a "
                  "version";
        return 0;
    }
    if (header->abi_version > KOI_ABI_VERSION) {
        *reason = "built for a newer Koi-DOS than this one";
        return 0;
    }
    if (header->abi_version < KOI_ABI_MINIMUM) {
        *reason = KOI_ABI_IS_ALPHA
            ? "built for an older Koi-DOS, and the interface has changed since"
            : "built for an interface this kernel no longer supports";
        return 0;
    }
    *reason = 0;
    return 1;
}

/* Enter the program on its own stack. A runaway program will wreck that one
   rather than the kernel's - and with the double-fault stack in place, even
   that gets reported instead of rebooting the machine. */
__attribute__((noreturn)) static void enter_program(boot_uint64_t entry_point,
                                                    boot_uint64_t stack_top) {
    __asm__ volatile (
        "movq %0, %%rsp\n"
        "andq $-16, %%rsp\n"
        "xorl %%ebp, %%ebp\n"
        "callq *%1\n"
        /* A program that returns instead of calling SYS_EXIT still has to end
           up somewhere sensible. */
        "movl %%eax, %%edi\n"
        "call program_exit\n"
        : : "r"(stack_top), "r"(entry_point) : "memory");
    __builtin_unreachable();
}

int program_run(VOLUME* volume, const char* path, const char* arguments,
                int* exit_code_out) {
    boot_uint8_t* contents;
    boot_uint32_t length = 0;
    boot_uint64_t entry_point = 0;

    if (program_running) return PROGRAM_NOT_LOADABLE;   /* one at a time, as in DOS */

    contents = read_program(volume, path, &length);
    if (!contents) return PROGRAM_NOT_LOADABLE;
    if (!load_segments(contents, length, &entry_point)) {
        kfree(contents);
        return PROGRAM_NOT_LOADABLE;
    }
    kfree(contents);

    {
        const char* reason;
        if (!abi_is_acceptable(&reason)) {
            /* Said here rather than by the caller, because only this function
               knows which of the reasons it was, and "will not run" without
               "why" is the least useful message a system can give. */
            console_write("Cannot run this program: ");
            console_write(reason);
            console_write(".\n");
            serial_write("PROGRAM: refused - ");
            serial_write(reason);
            serial_write("\n");
            return PROGRAM_REFUSED;
        }
    }

    current_arguments = arguments ? arguments : "";
    exit_code = 0;

    if (program_save(&resume)) {
        /* Arrived here through program_exit(). */
        current_arguments = "";
        if (exit_code_out) *exit_code_out = exit_code;
        return PROGRAM_OK;
    }
    program_running = 1;
    enter_program(entry_point, PROGRAM_LIMIT);
}
