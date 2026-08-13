#include "program.h"
#include "keyboard.h"
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

/* One entry per resident program, innermost last.
 *
 * A program can now run another and get control back when it ends, with
 * everything it had in memory still there - which is what DOS's EXEC did and
 * what SYS_CHAIN was standing in for while this machine could hold one image
 * at a time. Only one of them is running at any moment: the caller is stopped
 * inside the call, not scheduled alongside it. That is a smaller claim than
 * multitasking and it is the whole of what "run this and come back" needs. */
typedef struct {
    boot_uint64_t base;
    const char* arguments;
    char path[PROGRAM_CHAIN_MAX];
    int exit_code;
} PROGRAM_SLOT;

static PROGRAM_SLOT slots[PROGRAM_SLOTS];
static int depth;                       /* how many are resident */

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

/* One resume point per depth, for the same reason there is one slot: the
   innermost program exits back into the call that started it, not into the
   outermost one. A single point here meant the second exit returned to a
   stack frame that had already been left. */
static RESUME_POINT resume[PROGRAM_SLOTS];

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

/* Whichever program is running now, which is the innermost one. */
const char* program_arguments(void) {
    return depth ? slots[depth - 1].arguments : "";
}

const char* program_path(void) {
    return depth ? slots[depth - 1].path : "";
}

int program_depth(void) { return depth; }

/* The chain: what to run once the running program has gone.
 *
 * Kernel memory, deliberately. The requesting program's own memory is about to
 * be handed to whatever runs next, so a request stored there would be read back
 * out of a buffer the new program has already begun writing over - and would
 * work perfectly until the day the next program happened to be large. */
static char chain_lines[PROGRAM_CHAIN_DEPTH][PROGRAM_CHAIN_MAX];
static int chain_count;

int program_chain(const char* command) {
    boot_uint64_t length = 0;

    if (!command || !command[0]) return 0;
    if (chain_count >= PROGRAM_CHAIN_DEPTH) return 0;

    while (command[length] && length < PROGRAM_CHAIN_MAX - 1) {
        chain_lines[chain_count][length] = command[length];
        length++;
    }
    chain_lines[chain_count][length] = 0;
    chain_count++;
    return 1;
}

int program_chain_take(char* command, boot_uint64_t size) {
    const char* source;
    boot_uint64_t index = 0;

    if (!command || !size || !chain_count) return 0;
    source = chain_lines[--chain_count];
    while (source[index] && index < size - 1) {
        command[index] = source[index];
        index++;
    }
    command[index] = 0;
    return 1;
}

void program_chain_clear(void) { chain_count = 0; }

__attribute__((noreturn)) void program_exit(int code) {
    int leaving = depth - 1;

    if (leaving < 0) leaving = 0;
    slots[leaving].exit_code = code;
    if (depth) depth--;
    /* Interrupts were left enabled by the trap gate, so nothing has to be
       re-enabled here; the abandoned interrupt frame simply goes with the
       stack we are throwing away. */
    program_resume(&resume[leaving]);
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

/* Apply the relocations a position-independent program carries.
 *
 * There is one kind, and it says "add the load address to the value already
 * here". A table of function pointers or an array of string literals is
 * exactly that and nothing else, because there is nobody else to link to - no
 * libraries, no symbols to resolve, no dynamic linker. Everything else the
 * compiler emits is already RIP-relative and needs nothing.
 *
 * A program with no such table has no relocations at all, which is why the
 * first one tried came out empty and looked as though this had not worked. */
static int relocate(const boot_uint8_t* contents, boot_uint32_t length,
                    boot_uint64_t base) {
    const ELF64_HEADER* header = (const ELF64_HEADER*)contents;
    const ELF64_PROGRAM_HEADER* segments =
        (const ELF64_PROGRAM_HEADER*)(contents + header->e_phoff);
    boot_uint64_t table = 0;
    boot_uint64_t bytes = 0;
    boot_uint64_t entry_size = sizeof(ELF64_RELA);

    for (elf_uint16_t index = 0; index < header->e_phnum; index++) {
        const ELF64_PROGRAM_HEADER* segment = &segments[index];
        const ELF64_DYNAMIC* dynamic;
        boot_uint64_t count;

        if (segment->p_type != PT_DYNAMIC) continue;
        if (segment->p_offset > length ||
            segment->p_filesz > length - segment->p_offset) return 0;

        dynamic = (const ELF64_DYNAMIC*)(contents + segment->p_offset);
        count = segment->p_filesz / sizeof(ELF64_DYNAMIC);
        for (boot_uint64_t at = 0; at < count && dynamic[at].d_tag != DT_NULL; at++) {
            if (dynamic[at].d_tag == DT_RELA) table = dynamic[at].d_value;
            else if (dynamic[at].d_tag == DT_RELASZ) bytes = dynamic[at].d_value;
            else if (dynamic[at].d_tag == DT_RELAENT) entry_size = dynamic[at].d_value;
        }
    }

    if (!table || !bytes) return 1;      /* nothing to fix up, and that is fine */
    if (!entry_size || entry_size > 64) return 0;

    for (boot_uint64_t at = 0; at + entry_size <= bytes; at += entry_size) {
        /* Read from the image in memory, which is where the linker put the
           table - it is inside a PT_LOAD segment and has already been copied
           and biased into place. */
        const ELF64_RELA* item =
            (const ELF64_RELA*)(unsigned long long)(base + table + at);
        boot_uint64_t where = base + item->r_offset;

        if (ELF64_R_TYPE(item->r_info) != R_X86_64_RELATIVE) return 0;
        /* The same bounds the segments were checked against. A relocation is a
           write to an address a file chose, and there is no memory protection
           behind this. */
        if (where < base || where + 8 > base + PROGRAM_SLOT_SIZE) return 0;
        *(boot_uint64_t*)(unsigned long long)where =
            base + (boot_uint64_t)item->r_addend;
    }
    return 1;
}

/* Why a file could not be loaded, in words somebody can act on.
 *
 * Every one of these used to be "Not a valid Koi-DOS program", which is true
 * of a corrupt file, a program for another machine, a program built by an SDK
 * whose flags had drifted, and a text file somebody typed the name of. Four
 * different mornings, one sentence. */
static int load_segments(const boot_uint8_t* contents, boot_uint32_t length,
                         boot_uint64_t base, boot_uint64_t* entry_point,
                         const char** reason) {
    const ELF64_HEADER* header = (const ELF64_HEADER*)contents;
    const ELF64_PROGRAM_HEADER* segments;

    *reason = "the file is damaged";
    if (length < sizeof(ELF64_HEADER)) return 0;
    if (header->e_ident[0] != 0x7F || header->e_ident[1] != 'E' ||
        header->e_ident[2] != 'L' || header->e_ident[3] != 'F') {
        *reason = "not a program at all - no ELF header";
        return 0;
    }
    if (header->e_ident[ELF_INDEX_CLASS] != ELF_CLASS_64 ||
        header->e_ident[ELF_INDEX_DATA] != ELF_DATA_LSB) {
        *reason = "not a 64-bit little-endian program";
        return 0;
    }
    if (header->e_machine != ELF_MACHINE_X86_64) {
        *reason = "built for a different processor";
        return 0;
    }
    /* Position-independent, so that a second program can be resident at a
     * different address while the first one waits.
     *
     * A fixed-address program could only ever live in slot zero. Refusing it
     * is right; refusing it without saying which flag is wrong is what made
     * every program built by the SDK fail with one useless sentence after the
     * linker script here changed and the SDK's did not. */
    if (header->e_type == ET_EXEC) {
        *reason = "built to load at a fixed address. Rebuild it with the "
                  "current SDK: koicc now links -pie";
        return 0;
    }
    if (header->e_type != ET_DYN) {
        *reason = "not an executable ELF";
        return 0;
    }
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
        if (segment->p_vaddr + segment->p_memsz >
            PROGRAM_SLOT_SIZE - PROGRAM_STACK_SIZE) {
            *reason = "too large for the memory a program is given";
            return 0;
        }
        if (segment->p_filesz > segment->p_memsz) return 0;
        if (segment->p_offset > length ||
            segment->p_filesz > length - segment->p_offset) {
            *reason = "the file is shorter than its own headers claim";
            return 0;
        }

        memset((void*)(unsigned long long)(base + segment->p_vaddr), 0,
               (boot_uint64_t)segment->p_memsz);
        if (segment->p_filesz)
            memcpy((void*)(unsigned long long)(base + segment->p_vaddr),
                   contents + segment->p_offset, (boot_uint64_t)segment->p_filesz);
    }

    if (header->e_entry >= PROGRAM_SLOT_SIZE) return 0;
    if (!relocate(contents, length, base)) {
        *reason = "it needs a kind of relocation this loader does not do";
        return 0;
    }
    *reason = 0;
    *entry_point = base + header->e_entry;
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
static int abi_is_acceptable(boot_uint64_t base, const char** reason) {
    const KOI_PROGRAM_HEADER* header =
        (const KOI_PROGRAM_HEADER*)(unsigned long long)base;

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
    int slot = depth;
    boot_uint64_t base;

    /* Out of slots is a real answer and not a failure of the file: a program
       that runs a program that runs a program eventually meets the end of the
       window, and saying so beats loading over somebody. */
    if (slot >= PROGRAM_SLOTS) {
        console_write("Too many programs running at once.\n");
        return PROGRAM_REFUSED;
    }
    base = PROGRAM_SLOT_BASE(slot);

    contents = read_program(volume, path, &length);
    if (!contents) return PROGRAM_NOT_LOADABLE;
    {
        const char* reason = "the file is damaged";
        if (!load_segments(contents, length, base, &entry_point, &reason)) {
            kfree(contents);
            /* Said here, where the reason is known, rather than by a caller
               that only sees a number. */
            console_write("Cannot run this program: ");
            console_write(reason);
            console_write(".\n");
            serial_write("PROGRAM: refused - ");
            serial_write(reason);
            serial_write("\n");
            return PROGRAM_REFUSED;
        }
    }
    kfree(contents);

    {
        const char* reason;
        if (!abi_is_acceptable(base, &reason)) {
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

    slots[slot].base = base;
    slots[slot].arguments = arguments ? arguments : "";
    /* Copied rather than pointed at: the caller's buffer is a local in the
       command parser and is reused for the next line the moment this one
       finishes. */
    {
        boot_uint64_t index = 0;
        while (path[index] && index < PROGRAM_CHAIN_MAX - 1) {
            slots[slot].path[index] = path[index];
            index++;
        }
        slots[slot].path[index] = 0;
    }
    slots[slot].exit_code = 0;
    /* A Ctrl+C nobody acted on belongs to whatever it was aimed at, which is
       not this. Pressed at the prompt it stops here; pressed at a program that
       had already finished, likewise. Without this it would be waiting for the
       next program to make its first system call, and stop that one instead. */
    keyboard_break_clear();

    if (program_save(&resume[slot])) {
        /* Arrived here through program_exit(). Whatever this program was
           given is gone with it; the one underneath keeps its own. */
        if (exit_code_out) *exit_code_out = slots[slot].exit_code;
        return PROGRAM_OK;
    }
    depth = slot + 1;
    enter_program(entry_point, PROGRAM_SLOT_TOP(slot));
}
