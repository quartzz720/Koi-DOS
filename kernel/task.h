#ifndef KERNEL_TASK_H
#define KERNEL_TASK_H

#include "../include/bootinfo.h"
#include "idt.h"

/* Tasks: more than one program with a turn of its own.
 *
 * Until now a program that ran another program stopped inside the call. That
 * is what DOS's EXEC did, it is honest, and it is why this machine can hold
 * four programs in memory and still only ever be running one of them. The
 * caller is not waiting because it wants to - it is waiting because there was
 * no mechanism by which it could be anywhere else.
 *
 * This is that mechanism. A task is a program that the processor can be taken
 * away from and given back to: a stack of its own to be taken away in the
 * middle of, and enough saved state to be resumed as if nothing had happened.
 * Nothing above it changes shape - a program that runs another still waits for
 * it, because that is what "run this and come back" means - but the waiting is
 * now a decision rather than the only thing available.
 *
 * What a task is NOT, deliberately: a thread. There is one task per program
 * slot and no way to ask for a second one in the same program. Mizu's
 * applications stay cooperative inside their one process, and that is the
 * right place for them: they share every global they have, and preemption
 * between two of them would corrupt the desktop rather than speed it up.
 *
 * ---- What may be preempted, and what may not -----------------------------
 *
 * The kernel is not reentrant. One console, one FAT driver, one heap, one USB
 * controller, and none of them expect a second caller to arrive in the middle.
 * So the rule is narrow and checkable: a task is taken off the processor only
 * when the timer interrupt finds it at ring 3 - in its own code, in its own
 * address space, holding nothing of the kernel's.
 *
 * A task inside a system call is at ring 0 and is left alone until it comes
 * out. A program running at ring 0 - the DOS contract, the one that may write
 * to the screen and the hardware directly - is never preempted at all, for
 * the same reason: at ring 0 it *is* the kernel.
 *
 * This is a big lock without a lock: the ring the processor is in already
 * records whether the kernel is busy, and it cannot be got wrong by
 * forgetting to release it.
 */

typedef struct TASK TASK;

/* Make the context this is called from into the first task - the kernel's own,
   the one the shell runs in. Nothing is switched; this only gives the thing
   already running a name, so that everything after it can be switched away
   from and back to. */
void task_init(void);

/* The slot of the program running now, or -1 when that is the shell. */
int task_current_slot(void);

/* Start `entry` as a task for program slot `slot`, on that slot's kernel
   stack. The task is runnable immediately; the caller keeps running until it
   yields or waits. Returns null when the slot already has a task. */
TASK* task_start(int slot, void (*entry)(void));

/* The top of a slot's kernel stack - where interrupts arriving from ring 3
   land, which is what TSS.RSP0 has to be pointed at while that slot runs. */
boot_uint64_t task_kernel_stack_top(int slot);

/* Stop until `task` has finished. This is what a program calling a program
   does, and the only reason the caller stops. */
void task_wait(TASK* task);

/* Give up the rest of this turn but stay runnable. */
void task_yield(void);

/* The current task is over: whoever was waiting for it becomes runnable, and
   this never returns - the stack it is standing on is abandoned with it. */
__attribute__((noreturn)) void task_finish(void);

/* ---- Waiting inside the kernel without stopping the machine --------------
 *
 * The rule above - a task is only taken off the processor at ring 3 - is what
 * makes this kernel safe without locks, and it has one cost: a task waiting
 * inside a system call holds the machine. That was invisible while waits were
 * short and became the whole problem the moment a system call meant "ask a
 * server on the other side of the sea and wait": the desktop stopped, the
 * music stopped, everything stopped, and a thread of its own did not help
 * because the thread was inside the kernel too.
 *
 * So a wait may hand the processor over deliberately - at a point of its own
 * choosing, where it knows what state it is in. That is not preemption and it
 * is not a lock; it is a place in the code that says "nothing of mine is half
 * done here".
 *
 * What it does need is a gate. Once one task can be inside a system call while
 * another runs, two of them can arrive in the same driver, and this kernel has
 * one of everything. A gate is held by a task rather than counted, so a
 * routine that calls another routine holding the same gate does not deadlock
 * with itself - which is TLS calling TCP, and the reason it is written this
 * way rather than as a flag. */
typedef struct {
    TASK* owner;
    int depth;
} KERNEL_GATE;

/* Take the gate, giving up turns until it is free. Safe to call when this
   task already holds it. */
void gate_enter(KERNEL_GATE* gate);
void gate_leave(KERNEL_GATE* gate);

/* From the timer, once per quantum: ask for the current task to be taken off
   the processor at the first safe moment. */
void task_want_reschedule(void);

/* From the interrupt dispatcher, after the end-of-interrupt has been sent and
 * before the frame is returned to. Both halves of that sentence matter:
 * switching away before the EOI leaves the controller believing the interrupt
 * is still in service, and nothing of that priority is ever delivered again -
 * which is a scheduler that runs exactly one context switch and then stops.
 *
 * Does nothing unless a reschedule was asked for and `frame` came from ring 3.
 */
void task_preempt(const INTERRUPT_FRAME* frame);

/* How many program tasks exist, the shell not counted. Zero at the prompt. */
int task_running_count(void);

/* Implemented by program.c, called on every switch with the slot that is about
   to run (-1 for the shell): the address space that slot uses has to be the
   one the processor is walking before that slot runs a single instruction. */
void task_switched_to(int slot);

#endif
