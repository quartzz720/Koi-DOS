#include "task.h"
#include "cpu.h"
#include "program.h"
#include "string.h"
#include "serial.h"
#include "console.h"

/* One task per program slot, plus the kernel's own. */
/* One task per program was the whole story until threads: now a program may
   have several, so this is the number of tasks the machine can hold at once
   rather than the number of programs. Twelve is four programs with room for a
   few threads each - and each one costs a kernel stack, which is why it is
   not a hundred. */
#define TASK_MAX 12

/* The stack a task stands on while it is in the kernel.
 *
 * It was already here for a different reason: an interrupt arriving from ring
 * 3 needs a stack the program cannot have wrecked, and TSS.RSP0 has to point
 * at one per program rather than one for the machine. That per-program stack
 * is exactly what a task needs to be suspended on, so there is one array
 * rather than two.
 *
 * Sixteen kilobytes because the deepest thing that happens on it is a system
 * call into FAT32, and that is measured in hundreds of bytes per frame. */
/* Sixty-four kilobytes, and the reason is written here because the number
 * looks arbitrary and is not.
 *
 * Sixteen was enough for every driver and every file operation, and stopped
 * being enough the moment a certificate was checked: one ECDSA verification
 * asks for eleven kilobytes of locals, the point multiplication inside it
 * another five, and the point addition inside that another six. Twenty-two
 * kilobytes into sixteen does not go - it goes into whatever is underneath,
 * quietly, and the machine stops in a way that looks like a hang and cannot be
 * traced from the log.
 *
 * The immediate cause is that a big number here is sized for the largest RSA
 * key anybody issues - half a kilobyte each, a dozen of them per routine. The
 * curve does not need that and one day should say so; until then this is the
 * honest fix, and the canary below is how the next one of these gets found in
 * a second instead of an evening. */
#define KERNEL_STACK_SIZE 65536

/* A word at the bottom of each stack, checked when tasks change. If it is
   ever anything but this, something wrote past the end of a stack - and the
   machine says so rather than behaving strangely. */
#define STACK_CANARY 0x4B6F692D53746B21ULL   /* "Koi-Stk!" */

/* Where a task's floating-point registers live while it is not running.
 *
 * FXSAVE writes 512 bytes and requires them to be sixteen-byte aligned. One
 * area per task, which for five tasks is two and a half kilobytes - and the
 * alternative, saving nothing, is two programs doing arithmetic on each
 * other's numbers the first time the timer lands between them.
 *
 * Only tasks need this. A system call does not: the kernel is built without
 * floating point, so it cannot disturb what a program left in those
 * registers, and the state simply stays there across the call. That is not an
 * optimisation, it is the reason the kernel is built that way. */
typedef struct {
    boot_uint8_t bytes[512];
} FPU_STATE __attribute__((aligned(16)));

struct TASK {
    /* Where this task's kernel stack was left. Meaningless while it is
       running, which is why the switch writes it on the way out. */
    boot_uint64_t rsp;
    int used;
    int runnable;
    int slot;                  /* program slot, or -1 for the kernel's task */
    TASK* waiter;              /* who is stopped until this one finishes */
    FPU_STATE fpu;
};

static TASK tasks[TASK_MAX];
static TASK* current;
static int reschedule_wanted;

/* A kernel stack per task rather than per program. Threads in one program run
   in the same memory as each other and must not share the stack they stand on
   inside the kernel: two threads in one system call would be two callers on
   one stack, which is the shortest possible route to a machine that behaves
   impossibly. */
static boot_uint8_t stacks[TASK_MAX][KERNEL_STACK_SIZE]
    __attribute__((aligned(16)));
static int canaries_set;

static void set_canaries(void) {
    for (int at = 0; at < TASK_MAX; at++)
        *(boot_uint64_t*)&stacks[at][0] = STACK_CANARY;
    canaries_set = 1;
}

/* Checked whenever tasks change hands, which is often enough to catch an
   overflow near where it happened and cheap enough to do every time. */
static void check_canaries(void) {
    if (!canaries_set) { set_canaries(); return; }
    for (int at = 0; at < TASK_MAX; at++) {
        if (*(boot_uint64_t*)&stacks[at][0] == STACK_CANARY) continue;
        serial_write("TASK: the kernel stack of task ");
        serial_write_dec((boot_uint64_t)at);
        serial_write(" was written past its end - something in the kernel "
                     "used more stack than there is\n");
        console_write("\nA kernel stack overflowed. Something in the kernel "
                      "used more stack than it has.\n");
        *(boot_uint64_t*)&stacks[at][0] = STACK_CANARY;
    }
}

static boot_uint64_t stack_top_of(int index) {
    if (index < 0 || index >= TASK_MAX) return 0;
    return (boot_uint64_t)(unsigned long long)&stacks[index][0] +
           KERNEL_STACK_SIZE;
}

boot_uint64_t task_kernel_stack_top(int slot) {
    /* Kept for the one caller that asks about a program rather than a task:
       the first task of a slot is the one an interrupt from ring 3 lands on
       when that program has only one. */
    for (int at = 0; at < TASK_MAX; at++)
        if (tasks[at].used && tasks[at].slot == slot) return stack_top_of(at);
    return 0;
}

/* Switch stacks, and with the stack everything that was standing on it.
 *
 * The callee-saved registers are pushed because that is the whole of what one
 * C function owes another across a call; the caller-saved ones are already
 * wherever the compiler decided to keep them, which is on the stack we are
 * about to put down. The flags go too - a task suspended inside an interrupt
 * handler was running with interrupts off, and has to be given that back
 * rather than the flags of whoever happened to resume it.
 *
 * Nothing of the FPU or SSE is saved here, and that is not an omission: the
 * caller does it, in C, on either side of this call. See schedule().
 *
 * Not static: what follows is a global assembly label, and the assembler has
 * no way to satisfy a file-local declaration. */
void task_switch(boot_uint64_t* save_rsp, boot_uint64_t load_rsp);

__asm__(
".text\n"
".global task_switch\n"
"task_switch:\n"
"    pushq %rbp\n"
"    pushq %rbx\n"
"    pushq %r12\n"
"    pushq %r13\n"
"    pushq %r14\n"
"    pushq %r15\n"
"    pushfq\n"
"    movq %rsp, (%rdi)\n"
"    movq %rsi, %rsp\n"
"    popfq\n"
"    popq %r15\n"
"    popq %r14\n"
"    popq %r13\n"
"    popq %r12\n"
"    popq %rbx\n"
"    popq %rbp\n"
"    ret\n"
);

/* cli, and whether it was already off.
 *
 * The task table is read by the timer interrupt and written by everything
 * else, so every walk of it happens with interrupts off. Restoring rather
 * than unconditionally enabling, because the scheduler is called from inside
 * interrupt handlers as well as from ordinary code, and turning interrupts on
 * in the middle of one would be a nested handler on a stack that did not
 * expect one. */
static int interrupts_off(void) {
    boot_uint64_t flags;

    __asm__ volatile ("pushfq\n pop %0\n cli" : "=r"(flags) : : "memory");
    return (flags & 0x200) != 0;
}

static void interrupts_restore(int were_on) {
    if (were_on) __asm__ volatile ("sti" : : : "memory");
}

void task_init(void) {
    memset(tasks, 0, sizeof(tasks));
    /* A state to start every task from, rather than whatever the last one
       left. FXSAVE of a freshly initialised unit is what a task that has
       never run should be given. */
    for (int index = 0; index < TASK_MAX; index++)
        __asm__ volatile ("fxsave (%0)" : : "r"(tasks[index].fpu.bytes)
                          : "memory");
    tasks[0].used = 1;
    tasks[0].runnable = 1;
    tasks[0].slot = -1;
    current = &tasks[0];
}

int task_current_slot(void) {
    return current ? current->slot : -1;
}

int task_running_count(void) {
    int count = 0;

    for (int index = 1; index < TASK_MAX; index++)
        if (tasks[index].used) count++;
    return count;
}

/* Round robin, starting after whoever ran last: the point of starting there
   is that the task which just had a turn is the last one considered, so four
   runnable tasks get one turn each rather than the first one getting all of
   them. */
static TASK* next_runnable(void) {
    int start = (int)(current - tasks);

    for (int step = 1; step <= TASK_MAX; step++) {
        TASK* candidate = &tasks[(start + step) % TASK_MAX];
        if (candidate->used && candidate->runnable) return candidate;
    }
    return (TASK*)0;
}

/* Called with interrupts off. May return having switched away and back, which
   from the caller's side is indistinguishable from returning immediately -
   that is the entire trick and the reason the rest of the kernel does not
   have to know this exists. */
static void schedule(void) {
    check_canaries();
    TASK* next = next_runnable();
    TASK* previous = current;

    /* Everybody is waiting for something, so wait for the machine. This is
       the idle loop and it is not a busy one: hlt until an interrupt makes
       somebody runnable. Interrupts have to be on for that interrupt to
       arrive, and off again before the table is read. */
    while (!next) {
        __asm__ volatile ("sti\n hlt\n cli" : : : "memory");
        next = next_runnable();
    }
    if (next == current) return;

    current = next;
    /* Before a single instruction of the new task runs: the stack its
       interrupts land on, and the tables the processor walks for it. */
    /* The task's own stack, not its program's: with threads a program has
       several, and an interrupt arriving from ring 3 must land on the one
       belonging to the thread that was interrupted. */
    if (next->slot >= 0)
        cpu_set_kernel_stack(stack_top_of((int)(next - tasks)));
    task_switched_to(next->slot);

    /* And the floating-point registers, which belong to whoever was using
       them. Saved here rather than inside task_switch because this is C and
       that is assembly, and because each side restores its own on the way
       back - which is the same trick the general registers use one line
       below. */
    __asm__ volatile ("fxsave (%0)" : : "r"(previous->fpu.bytes) : "memory");
    task_switch(&previous->rsp, next->rsp);
    /* Arrived here again, which means this task has just been switched TO.
       Its registers come back before it does anything with them. */
    __asm__ volatile ("fxrstor (%0)" : : "r"(current->fpu.bytes) : "memory");
}

/* Where a task goes if its entry function returns instead of ending the
   program itself. Nothing should reach here - enter_program calls
   program_exit for exactly this case - but a stack whose last return address
   is garbage is a machine that dies without saying why, and one word of stack
   buys a sentence instead. */
static void task_fell_off_the_end(void) {
    serial_write("TASK: entry returned; ending the task\n");
    task_finish();
}

TASK* task_start(int slot, void (*entry)(void)) {
    TASK* task = (TASK*)0;
    boot_uint64_t* top;
    int index = -1;

    if (slot < 0 || slot >= PROGRAM_SLOTS) return (TASK*)0;
    /* The first free entry rather than one reserved for this program: a
       program may have several tasks now, and which entries they occupy is
       nobody's business but this file's. */
    for (int at = 1; at < TASK_MAX; at++)
        if (!tasks[at].used) { index = at; task = &tasks[at]; break; }
    if (!task) return (TASK*)0;

    /* The stack a task has never run on, built to look like one it was
       switched away from: a return address for task_switch to return to, and
       under it the registers and flags that switch pops on the way in.
     *
     * Two return addresses rather than one, and the spare is not only a
     * safety net - it is what makes the alignment right. A function entered
     * through `ret` must find the stack exactly as a `call` would have left
     * it, which for the x86-64 rules means the return address sits on a
     * sixteen-byte boundary. One word here would put it eight bytes out. */
    top = (boot_uint64_t*)(unsigned long long)stack_top_of(index);
    *--top = (boot_uint64_t)(unsigned long long)&task_fell_off_the_end;
    *--top = (boot_uint64_t)(unsigned long long)entry;
    *--top = 0;                    /* rbp */
    *--top = 0;                    /* rbx */
    *--top = 0;                    /* r12 */
    *--top = 0;                    /* r13 */
    *--top = 0;                    /* r14 */
    *--top = 0;                    /* r15 */
    /* Flags as the task will start with them: the reserved bit, and
       interrupts enabled. A task that began with them off would run its
       program with the clock stopped. */
    *--top = 0x202;

    task->rsp = (boot_uint64_t)(unsigned long long)top;
    task->slot = slot;
    task->waiter = (TASK*)0;
    task->runnable = 1;
    task->used = 1;
    return task;
}

void task_wait(TASK* task) {
    int were_on = interrupts_off();

    /* A loop rather than a single switch, because being woken is not proof
       that the thing waited for has ended - only that somebody thought it
       might have. */
    while (task->used) {
        task->waiter = current;
        current->runnable = 0;
        schedule();
    }
    current->runnable = 1;
    interrupts_restore(were_on);
}

void task_yield(void) {
    int were_on = interrupts_off();

    schedule();
    interrupts_restore(were_on);
}

__attribute__((noreturn)) void task_finish(void) {
    interrupts_off();

    if (current->waiter) current->waiter->runnable = 1;
    current->used = 0;
    current->runnable = 0;
    /* Switches away and does not come back: a task that is not `used` is
       never chosen again, and the stack this is standing on goes with it. */
    schedule();
    for (;;) __asm__ volatile ("hlt");
}

void task_want_reschedule(void) {
    reschedule_wanted = 1;
}

void task_preempt(const INTERRUPT_FRAME* frame) {
    if (!reschedule_wanted) return;
    reschedule_wanted = 0;
    /* Only from ring 3. The low two bits of the code segment are the
       privilege level the interrupted code was running at, and 3 is the only
       one that means "in a program, holding nothing of the kernel's". */
    if (!frame || (frame->cs & 3) != 3) return;
    if (!current || current->slot < 0) return;
    /* Interrupts are already off - we are inside an interrupt handler - so
       the table is safe to read, and the flags this task is carrying are the
       ones it gets back when it is resumed. */
    schedule();
}

/* ---- Gates ---------------------------------------------------------------
 *
 * One task at a time inside a subsystem, with the holder recorded so that the
 * same task may enter again - TLS asks TCP for bytes while holding the
 * network gate, and a counted lock would stop it dead.
 */
void gate_enter(KERNEL_GATE* gate) {
    while (gate->owner && gate->owner != current) task_yield();
    gate->owner = current;
    gate->depth++;
}

void gate_leave(KERNEL_GATE* gate) {
    if (gate->owner != current) return;
    if (--gate->depth <= 0) {
        gate->depth = 0;
        gate->owner = (TASK*)0;
    }
}
