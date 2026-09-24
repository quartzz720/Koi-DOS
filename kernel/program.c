#include "program.h"
#include "keyboard.h"
#include "fat32.h"
#include "memory.h"
#include "paging.h"
#include "cpu.h"
#include "task.h"
#include "string.h"
#include "heap.h"
#include "../include/elf.h"
#include "../include/syscall.h"
#include "syscall.h"
#include "console.h"
#include "serial.h"

/* Loading and running a program.
 *
 * The same ELF64 walk the bootloader does for the kernel, against a file read
 * through FAT32 instead of UEFI. include/elf.h is shared between the two so
 * the structures cannot drift.
 */

/* One entry per resident program.
 *
 * A program can run another and get control back when it ends, with
 * everything it had in memory still there - which is what DOS's EXEC did and
 * what SYS_CHAIN was standing in for while this machine could hold one image
 * at a time. The caller stops inside the call, and now that is a decision
 * rather than the only thing available: each slot is a task (task.c), the
 * caller is a task that has asked to wait, and a program started without one
 * waiting for it runs alongside everything else.
 *
 * A slot is claimed by index rather than by depth, because "the innermost
 * program" stops being a meaningful phrase the moment two of them are
 * running. The index is also the owner tag the system calls use, which is why
 * it has to identify a program rather than a position in a stack. */
typedef struct {
    boot_uint64_t base;
    const char* arguments;
    char path[PROGRAM_CHAIN_MAX];
    int exit_code;
    int used;                  /* claimed: loaded, running, or awaiting reaping */
    int generation;            /* how many programs this slot has held */
    int finished;              /* the task has ended; the memory has not */
    int user;                  /* running at ring 3 */
    int threads;               /* extra tasks this program has started */
    boot_uint64_t entry;       /* where its task begins */
    PAGING_SPACE* space;       /* its own tables, when it has them */
} PROGRAM_SLOT;

static PROGRAM_SLOT slots[PROGRAM_SLOTS];

/* The slot the processor is in now, which is a question only the scheduler
   can answer. -1 means the shell. */
static int current_slot(void) { return task_current_slot(); }

/* Whichever program is running now. */
const char* program_arguments(void) {
    int slot = current_slot();
    return slot >= 0 ? slots[slot].arguments : "";
}

/* Where the running program was loaded. With the address a fault reports,
   this is what turns "it died somewhere" into an offset in one program. */
boot_uint64_t program_base(void) {
    int slot = current_slot();
    return slot >= 0 ? slots[slot].base : 0;
}

const char* program_path(void) {
    int slot = current_slot();
    return slot >= 0 ? slots[slot].path : "";
}

/* Zero at the prompt, and otherwise one more than the slot of the program
 * asking - which is what this returned when a slot was a depth, and is what
 * every caller actually wanted: an identity for "whose is this?" that is zero
 * for the kernel's own. */
int program_depth(void) { return current_slot() + 1; }

int program_owner(void) { return current_slot() + 1; }

/* Is the program that is running now at ring 3?
 *
 * Asked by the calls that hand a program memory - a block it allocated, the
 * screen it is about to draw on. Memory given to a program that cannot reach
 * it is not a gift, it is a page fault with a delay, and the kernel is the
 * only side that can say which ring the receiver is in. */
int program_is_user(void) {
    int slot = current_slot();
    return slot >= 0 ? slots[slot].user : 0;
}

/* The tables the running program is using, so that the calls which hand it
   memory mark it where that program will look. */
static PAGING_SPACE* current_space(void) {
    int slot = current_slot();
    return slot >= 0 ? slots[slot].space : (PAGING_SPACE*)0;
}

/* Called by the scheduler on every switch, before the task it names runs.
 *
 * The address space is the one thing that cannot be left until the new task
 * asks for it: the first instruction it executes is already being fetched
 * through whatever tables CR3 points at, and if those are somebody else's the
 * program is reading somebody else's memory - or, on a good day, faulting. */
void task_switched_to(int slot) {
    paging_space_enter(slot >= 0 ? slots[slot].space : (PAGING_SPACE*)0);
}

int program_allow_user(boot_uint64_t base, boot_uint64_t size) {
    PAGING_SPACE* space = current_space();

    if (space) return paging_space_allow_user(space, base, size);
    return paging_allow_user(base, size);
}

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
    int leaving = current_slot();

    /* The shell does not exit. Nothing should call this from there, and the
       one thing that might - a stray SYS_EXIT from kernel code - would
       otherwise end the task the whole machine is standing in. */
    if (leaving < 0) {
        serial_write("PROGRAM: exit outside a program, ignored\n");
        for (;;) __asm__ volatile ("hlt");
    }

    slots[leaving].exit_code = code;
    /* The slot stays claimed until somebody else clears it: its memory, its
       tables and its exit code are all still needed, and the tables in
       particular cannot be freed by the task that is standing in them. */
    slots[leaving].finished = 1;
    task_finish();
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
                    boot_uint64_t base, boot_uint64_t limit) {
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
        if (where < base || where + 8 > base + limit) return 0;
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
                         boot_uint64_t base, boot_uint64_t limit,
                         boot_uint64_t* entry_point, const char** reason) {
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
        if (segment->p_vaddr + segment->p_memsz > limit) {
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

    if (header->e_entry >= limit) return 0;
    if (!relocate(contents, length, base, limit)) {
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

/* ---- Modules -------------------------------------------------------------
 *
 * The same loader, stopping one step short: segments copied, relocations
 * applied, interface checked - and then the entry point handed back instead
 * of jumped to. Everything above already did all of that; what is new is not
 * finishing.
 *
 * A module is not given a program slot. Slots are for programs that are
 * running, and the loading program is itself in one - so a module in the next
 * slot would be overwritten by the first thing that program ran, which is
 * exactly what a desktop does all day. Pages are asked for instead, sized to
 * the image, which a position-independent file does not mind in the least.
 *
 * They are recorded so that unloading can refuse a pointer this never handed
 * out. Freeing an address a program invented would hand the page allocator
 * memory belonging to somebody else, and nothing would notice until it was
 * handed out twice. */
#define MODULE_MAX 8

typedef struct {
    boot_uint64_t base;
    boot_uint64_t pages;
} MODULE;

static MODULE modules[MODULE_MAX];

/* How much memory an image needs: the highest address any segment reaches,
   rounded up to whole pages. .bss is included - p_memsz, not p_filesz - which
   is the one thing a loader that reads only the file's own size gets wrong,
   and gets wrong silently. */
static boot_uint64_t image_span(const boot_uint8_t* contents,
                                boot_uint32_t length) {
    const ELF64_HEADER* header = (const ELF64_HEADER*)contents;
    const ELF64_PROGRAM_HEADER* segments;
    boot_uint64_t span = 0;

    if (length < sizeof(ELF64_HEADER)) return 0;
    if (header->e_phoff > length ||
        (boot_uint64_t)header->e_phnum * sizeof(ELF64_PROGRAM_HEADER) >
            length - header->e_phoff) return 0;
    segments = (const ELF64_PROGRAM_HEADER*)(contents + header->e_phoff);
    for (elf_uint16_t index = 0; index < header->e_phnum; index++) {
        const ELF64_PROGRAM_HEADER* segment = &segments[index];
        boot_uint64_t end;

        if (segment->p_type != PT_LOAD || !segment->p_memsz) continue;
        end = segment->p_vaddr + segment->p_memsz;
        if (end > span) span = end;
    }
    return span;
}

int program_load(VOLUME* volume, const char* path, boot_uint64_t* base_out,
                 boot_uint64_t* size_out, boot_uint64_t* entry_out) {
    boot_uint8_t* contents;
    boot_uint32_t length = 0;
    boot_uint64_t span, pages, base, entry_point = 0;
    const char* reason = "the file is damaged";
    int slot;

    for (slot = 0; slot < MODULE_MAX; slot++) if (!modules[slot].base) break;
    if (slot == MODULE_MAX) {
        console_write("Too many modules loaded at once.\n");
        return PROGRAM_REFUSED;
    }

    contents = read_program(volume, path, &length);
    if (!contents) return PROGRAM_NOT_LOADABLE;

    span = image_span(contents, length);
    if (!span) { kfree(contents); return PROGRAM_REFUSED; }
    pages = (span + PAGE_SIZE - 1) / PAGE_SIZE;
    base = (boot_uint64_t)(unsigned long long)alloc_pages(pages);
    if (!base) {
        kfree(contents);
        console_write("Not enough memory to load this module.\n");
        return PROGRAM_REFUSED;
    }

    if (!load_segments(contents, length, base, pages * PAGE_SIZE,
                       &entry_point, &reason) ||
        !abi_is_acceptable(base, &reason)) {
        kfree(contents);
        free_pages((void*)(unsigned long long)base, pages);
        console_write("Cannot load this module: ");
        console_write(reason);
        console_write(".\n");
        serial_write("MODULE: refused - ");
        serial_write(reason);
        serial_write("\n");
        return PROGRAM_REFUSED;
    }
    kfree(contents);

    /* A module loaded by a program at ring 3 is code that program is about to
       call, so it has to live where that program can reach - and execute. The
       desktop and its applications end up in the same ring, which is the
       shape the plan asks for: one boundary, around all of Mizu, with the
       kernel on the other side of it. */
    if (program_is_user())
        (void)program_allow_user(base, pages * PAGE_SIZE);

    modules[slot].base = base;
    modules[slot].pages = pages;
    if (base_out) *base_out = base;
    if (size_out) *size_out = pages * PAGE_SIZE;
    if (entry_out) *entry_out = entry_point;
    return PROGRAM_OK;
}

int program_unload(boot_uint64_t base) {
    for (int slot = 0; slot < MODULE_MAX; slot++) {
        if (modules[slot].base != base || !base) continue;
        free_pages((void*)(unsigned long long)modules[slot].base,
                   modules[slot].pages);
        modules[slot].base = 0;
        modules[slot].pages = 0;
        return 1;
    }
    return 0;
}

/* The kernel stack each program runs on lives in task.c now.
 *
 * TSS.RSP0 is the stack the processor switches to when an interrupt arrives
 * from ring 3, and there was one of them for the whole machine. That is
 * enough for one program and wrong for two: a desktop at ring 3 makes a
 * system call, the kernel runs on that stack, and while it is still in the
 * middle of it, it starts a second program at ring 3 - whose first system
 * call arrives on the same stack and writes over the frames of the first.
 *
 * What that looks like is a machine that survives everything it was supposed
 * to survive and then dies on the way home: the inner program faulted, was
 * stopped correctly, and the kernel returned into a stack frame that no
 * longer existed. RIP 0x798B, somewhere below the first megabyte, in nothing.
 *
 * That stack turned out to be the same object a task needs to be suspended
 * on, so there is one array of them and the scheduler owns it. */

/* The top of a program's stack: below the command line, aligned. */
static boot_uint64_t program_stack_top(int slot) {
    return (PROGRAM_SLOT_TOP(slot) - PROGRAM_ARGUMENTS_MAX) & ~15ULL;
}

/* Whether the program now starting runs at ring 3.
 *
 * One flag rather than a parameter threaded through everything, because the
 * thing that needs to know is enter_program at the very bottom and the thing
 * that decides is the shell at the very top. It is set for one program and
 * cleared as soon as that program is entered, so it cannot leak into whatever
 * that program runs next. */
static int enter_at_ring3;

void program_run_next_at_ring3(void) { enter_at_ring3 = 1; }

/* Whether the next program is one to wait for.
 *
 * Set for one program and cleared as it starts, for the same reason as the
 * flag above: what decides is the shell at the top and what acts on it is the
 * bottom of program_run, and threading a parameter through everything between
 * them would mean changing every caller to say "no, the usual" - which is how
 * an option becomes a thing nobody can read. */
static int background;

void program_run_next_in_background(void) { background = 1; }

/* Whether such a request is waiting to be acted on. Asked by the shell before
   it runs a program, because what it does afterwards depends on whether the
   program has ended or has only started: taking the screen and the colours
   back is right for the first and takes them from a running program in the
   second. */
int program_run_next_is_background(void) { return background; }

/* Which program the last background start actually started.
 *
 * A caller that does not wait gets no exit code, so what it needs back is a
 * name for the thing it started - something to ask about later, when it wants
 * to know whether that program is still running.
 *
 * The slot number alone will not do. Slots are reused, so a desktop holding
 * the number of a program that ended would be told "still running" about
 * whatever was started next - and would go on waiting for the wrong thing, or
 * refuse to take its screen back. The count of programs the slot has held is
 * folded in, which makes the name unique for as long as anybody could still be
 * holding it. */
static int last_started;

static int program_name_of(int slot) {
    return ((slots[slot].generation & 0xFFFFFF) << 8) | (slot + 1);
}

int program_last_started(void) { return last_started; }

int program_is_running(int name) {
    int slot = (name & 0xFF) - 1;

    if (slot < 0 || slot >= PROGRAM_SLOTS) return 0;
    if (program_name_of(slot) != name) return 0;
    return slots[slot].used && !slots[slot].finished;
}

/* Both of the above, undone. For a caller that asked for something unusual
   and then found there was no program to run: an option that outlives the
   command that set it is worse than no option at all. */
void program_run_next_cancel(void) {
    background = 0;
    enter_at_ring3 = 0;
}

/* Give back what a finished program was holding.
 *
 * Not done by the program itself: the last thing it needs is the address
 * space it is standing in, and a task cannot free the tables the processor is
 * walking for it. So the slot stays claimed until somebody who is not it says
 * otherwise - the caller that was waiting, or the shell on its way round the
 * loop for a program nobody waited for. */
static void reap(int slot) {
    if (slot < 0 || slot >= PROGRAM_SLOTS) return;
    if (!slots[slot].used || !slots[slot].finished) return;
    /* The files it left open, the memory it asked for, the sound it was
       playing. Here rather than in the shell, because a program nobody waited
       for has no shell to notice that it ended. */
    syscall_close_owner(slot + 1);
    if (slots[slot].space) {
        paging_space_destroy(slots[slot].space);
        slots[slot].space = (PAGING_SPACE*)0;
    }
    slots[slot].used = 0;
    slots[slot].finished = 0;
}

void program_reap(void) {
    for (int slot = 0; slot < PROGRAM_SLOTS; slot++)
        if (slots[slot].used && slots[slot].finished) reap(slot);
}

/* Where a task begins.
 *
 * Everything that had to happen before the program's first instruction has
 * happened by now, and most of it happened in the scheduler rather than here:
 * the kernel stack this is standing on is the slot's own, and CR3 already
 * points at the slot's tables. What is left is the jump. */
/* ---- Threads --------------------------------------------------------------
 *
 * A thread is a task that shares everything with the one that made it: the
 * same program, the same memory, the same open files. What it does not share
 * is the two stacks - one at ring 3 for its own code, one in the kernel for
 * the system calls it makes - because a stack is the one thing that cannot be
 * shared by two things running at once.
 *
 * The user stack is the program's own: it allocates it and passes the top in.
 * That is deliberate. The kernel does not know how much stack a thread of
 * somebody else's program needs, the program does, and a kernel that guessed
 * would be a kernel that guessed wrong for somebody.
 *
 * What this buys, and it is the whole reason it exists: a desktop that goes on
 * drawing while one of its threads sits inside a system call waiting for a
 * server on the other side of the sea to answer.
 */
typedef struct {
    boot_uint64_t entry;
    boot_uint64_t stack;
    boot_uint64_t argument;
    int slot;
    int taken;
} THREAD_REQUEST;

/* One at a time, and read by the new task the moment it starts. The task is
   created and then runs immediately or soon; nothing else may ask for a
   thread until this one has been picked up, which the flag below enforces. */
static THREAD_REQUEST pending_thread;

__attribute__((noreturn)) static void enter_thread(boot_uint64_t entry_point,
                                                   boot_uint64_t stack_top,
                                                   boot_uint64_t argument) {
    __asm__ volatile (
        "movq %0, %%rsp\n"
        "andq $-16, %%rsp\n"
        "xorl %%ebp, %%ebp\n"
        "movq %2, %%rdi\n"
        "callq *%1\n"
        /* A thread that returns instead of asking to end still has to end. */
        "call program_thread_finish\n"
        : : "r"(stack_top), "r"(entry_point), "r"(argument) : "memory");
    __builtin_unreachable();
}

static void program_thread_entry(void) {
    boot_uint64_t entry = pending_thread.entry;
    boot_uint64_t stack = pending_thread.stack;
    boot_uint64_t argument = pending_thread.argument;
    int slot = pending_thread.slot;

    pending_thread.taken = 1;
    /* A thread runs where its program runs. Most programs here are at ring 0 -
       the DOS contract, free of the machine - and a thread of one entered at
       ring 3 would fault on the first thing its program does. */
    if (slot >= 0 && slots[slot].user)
        cpu_enter_user_with(entry, stack, argument);
    enter_thread(entry, stack, argument);
}

int program_thread_start(boot_uint64_t entry, boot_uint64_t stack,
                         boot_uint64_t argument) {
    int slot = task_current_slot();

    if (slot < 0) return 0;
    if (!slots[slot].used || slots[slot].finished) return 0;
    if (!entry || !stack) return 0;
    /* Both addresses must be memory this program can already reach at ring 3.
     *
     * Not "inside the program's slot", which was the first version and was
     * wrong: what koi_alloc hands back is a kernel page marked reachable, and
     * it lives nowhere near the slot - so every thread was refused, and the
     * check was measuring the wrong thing anyway. The question that matters
     * is whether the program could touch this memory itself. If it could,
     * running a thread there grants nothing new; if it could not, the kernel
     * would be handing ring 3 a page nobody gave it. */
    /* For a program at ring 3, both addresses must be memory it can already
     * reach there - otherwise the kernel would be handing ring 3 a page
     * nobody gave it. For a program at ring 0 there is nothing to check: it
     * can already reach everything, which is what ring 0 means and why this
     * system says so out loud rather than pretending otherwise. */
    if (slots[slot].user) {
        if (!paging_space_is_user(slots[slot].space, entry, 16)) {
            serial_write("THREAD: refused - the entry point is not memory "
                         "this program can reach at ring 3\n");
            return 0;
        }
        if (!paging_space_is_user(slots[slot].space, stack - PAGE_SIZE,
                                  PAGE_SIZE)) {
            serial_write("THREAD: refused - the stack is not memory this "
                         "program can reach at ring 3\n");
            return 0;
        }
    }

    pending_thread.entry = entry;
    pending_thread.stack = stack;
    pending_thread.argument = argument;
    pending_thread.slot = slot;
    pending_thread.taken = 0;

    if (!task_start(slot, program_thread_entry)) {
        serial_write("THREAD: refused - no room for another task\n");
        return 0;
    }
    slots[slot].threads++;

    /* Wait until the new task has picked its request up.
     *
     * There is one request at a time, and the first version returned as soon
     * as the task existed - so a program starting two threads in a row
     * overwrote the first one's entry and argument before it had run. Both
     * threads then started as the second, which the test program noticed by
     * printing the same number twice. Handing over one at a time is the whole
     * fix, and it costs one yield. */
    while (!pending_thread.taken) task_yield();
    return 1;
}

/* The thread is over. Its program is not: only the last task out turns off
   the lights, and that is the one program_run is waiting for. */
__attribute__((noreturn)) void program_thread_finish(void) {
    int slot = task_current_slot();

    if (slot >= 0 && slots[slot].threads) slots[slot].threads--;
    task_finish();
}

static void program_task_entry(void) {
    int slot = task_current_slot();

    if (slot < 0) task_finish();
    if (slots[slot].user)
        cpu_enter_user(slots[slot].entry, program_stack_top(slot));
    enter_program(slots[slot].entry, program_stack_top(slot));
}

int program_run(VOLUME* volume, const char* path, const char* arguments,
                int* exit_code_out) {
    boot_uint8_t* contents;
    boot_uint32_t length = 0;
    boot_uint64_t entry_point = 0;
    int slot;
    boot_uint64_t base;
    /* Both requests are taken here, at the top, rather than at the point they
       are acted on: a program that turns out not to exist would otherwise
       leave one of them set for whatever is typed next, and "the next program
       you run happens to be at ring 3" is a bug nobody would connect to the
       command that caused it. */
    int at_ring3 = enter_at_ring3;
    int waiting = !background;

    enter_at_ring3 = 0;
    background = 0;

    /* Anything that finished while nobody was waiting for it is cleared away
       first, so that "no free slot" means four programs are running rather
       than four have run. */
    program_reap();
    for (slot = 0; slot < PROGRAM_SLOTS; slot++)
        if (!slots[slot].used) break;

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
        if (!load_segments(contents, length, base,
                           PROGRAM_SLOT_SIZE - PROGRAM_STACK_SIZE,
                           &entry_point, &reason)) {
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
    /* The command line goes into the program's own memory, at the top of its
     * slot, and the pointer it is given points there.
     *
     * It used to point into the shell's buffer - kernel memory - and every
     * program read it happily, because at ring 0 there is nothing to stop
     * one. The first program run at ring 3 printed its greeting and then took
     * a page fault on its own arguments, which is the ABI being caught
     * handing out an address the program has no right to. DOS put the command
     * line in the PSP, inside the program's own memory, for the same reason
     * it needed to be somewhere the program could reach.
     *
     * The stack starts below it, so the two cannot meet. */
    {
        char* into = (char*)(unsigned long long)
                     (PROGRAM_SLOT_TOP(slot) - PROGRAM_ARGUMENTS_MAX);
        const char* from = arguments ? arguments : "";
        boot_uint64_t index = 0;

        while (from[index] && index < PROGRAM_ARGUMENTS_MAX - 1) {
            into[index] = from[index];
            index++;
        }
        into[index] = 0;
        slots[slot].arguments = into;
    }
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
    slots[slot].user = 0;
    slots[slot].finished = 0;
    slots[slot].entry = entry_point;
    slots[slot].space = (PAGING_SPACE*)0;
    slots[slot].generation++;
    slots[slot].used = 1;
    /* A Ctrl+C nobody acted on belongs to whatever it was aimed at, which is
       not this. Pressed at the prompt it stops here; pressed at a program that
       had already finished, likewise. Without this it would be waiting for the
       next program to make its first system call, and stop that one instead. */
    keyboard_break_clear();

    if (at_ring3) {
        slots[slot].user = 1;
        /* Its own tables, and in them its own window of memory. Everything
           else - the kernel, the other slots, the framebuffer - is mapped
           where the kernel has it and is reachable by nothing at ring 3. */
        slots[slot].space = paging_space_create();
        if (!slots[slot].space ||
            !paging_space_allow_user(slots[slot].space, base,
                                     PROGRAM_SLOT_SIZE)) {
            console_write("Cannot give this program its own memory.\n");
            if (slots[slot].space) paging_space_destroy(slots[slot].space);
            slots[slot].space = (PAGING_SPACE*)0;
            slots[slot].used = 0;
            return PROGRAM_REFUSED;
        }
        serial_write("PROGRAM: entering at ring 3, in its own space\n");
    }

    {
        TASK* task = task_start(slot, program_task_entry);

        if (!task) {
            console_write("Cannot start this program.\n");
            if (slots[slot].space) paging_space_destroy(slots[slot].space);
            slots[slot].space = (PAGING_SPACE*)0;
            slots[slot].used = 0;
            return PROGRAM_REFUSED;
        }
        if (!waiting) {
            /* Nobody is waiting. The program is runnable and gets its turn
               from the scheduler like everything else; whoever asked for it
               carries on with the next line. */
            last_started = program_name_of(slot);
            if (exit_code_out) *exit_code_out = 0;
            return PROGRAM_OK;
        }
        /* Waiting, which is what "run this and come back" means. The stack
           this is standing on is the caller's own and stays exactly as it is
           until the task it is waiting for has ended. */
        task_wait(task);
    }

    /* Back, and the program is gone. The tables it was using were left behind
       by the scheduler on the way here - which is why they can be freed now
       and could not have been freed by the program itself. */
    if (exit_code_out) *exit_code_out = slots[slot].exit_code;
    reap(slot);
    return PROGRAM_OK;
}
