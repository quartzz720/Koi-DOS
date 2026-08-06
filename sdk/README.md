# Writing programs for Koi-DOS

> ## ⚠ ALPHA
>
> **The interface is not frozen. Function numbers can still change, and when
> they do, programs built against an older version stop running.**
>
> This is on purpose and it is enforced, not left to chance: every program
> records the interface version it was built against, and the kernel refuses to
> start one it cannot honour — in both directions. A program built for a newer
> Koi-DOS would call functions that do not exist. A program built for an older
> one is refused too, because a function number that has changed meaning does
> not fail when called. It quietly does something else, which is far worse than
> not starting.
>
> **What that means for you:** rebuild your programs when the system version
> changes. That is the whole cost, and it is preferable to the alternative.
>
> **What it will mean later:** once the numbering is frozen, `KOI_ABI_MINIMUM`
> stops moving and everything built from that day forward keeps working
> indefinitely. **From then on, function numbers are never reused** — a removed
> call leaves a hole rather than being recycled. That promise is what makes old
> programs safe, and it is cheap to keep: there are 256 numbers and twenty are
> in use.

Everything needed to build a program is in this directory. You do not need the
kernel source, and you do not need a special compiler — an ordinary x86-64 GCC
is enough, because a Koi-DOS program is a freestanding ELF64 binary and nothing
more exotic than that.

```bash
./koicc hello.c          # produces HELLO.EXE
```

Copy the result to the root of a Koi-DOS drive and type its name at the prompt.

**Programs you write are yours.** Including these headers does not bring your
program under the kernel's licence — see the "Programs written for Koi-DOS"
section of the project's LICENSE. Licence your own work however you like.

## The smallest complete program

```c
#include "koi.h"

int main(const char* arguments) {
    koi_print("Hello.\n");
    if (arguments[0]) {
        koi_print("You gave me: ");
        koi_print(arguments);
        koi_print("\n");
    }
    return 0;
}
```

`main` receives the rest of the command line as a single string — the shell does
not split it for you, in the DOS tradition. Returning from `main` exits with
that value as the exit code.

## What you get, and what you do not

There is **no C library**. No `printf`, no `malloc`, no `strlen`. You get the
system calls in `koi.h` and whatever you write yourself. This is deliberate:
Koi-DOS is a DOS-like system, and DOS programs brought their own runtime too.

`koi.h` provides console output, keyboard input, files, directory enumeration,
colours, system information, and a decimal printer, because every program needs
that one immediately.

## The interface

| | |
|---|---|
| `koi_print(text)`, `koi_putchar(c)`, `koi_print_dec(n)` | output |
| `koi_getchar()`, `koi_readline(buffer, size)` | input |
| `koi_cls()`, `koi_color(fg, bg)`, `koi_set_theme(...)` | the screen |
| `koi_open/close/read/write/size` | files |
| `koi_findfirst/findnext/findclose` | directory enumeration |
| `koi_arguments()`, `koi_version()`, `koi_exit(code)` | environment |
| `koi_sysinfo(item, index)`, `koi_systext(item, index, buffer, size)` | what the system knows about itself |
| `koi_cpu_name(buffer)` | the processor's brand string, straight from CPUID |

`KOI_ABI_VERSION` in `syscall.h` is the number your program is stamped with; the
build script prints it. You do not have to check it yourself — the kernel does,
before your first instruction runs.

**DOSFETCH** is the worked example, and it lives in its own repository rather
than in the kernel tree — precisely so that it builds the way your program will:
with this SDK and nothing else. Every line it prints comes through this
interface.

## The ABI, if you want to bypass the header

Calls are made with `int 0x40`, in the spirit of DOS's `int 21h`. The vector is
`0x40` rather than `0x21` because in protected mode `0x21` is the keyboard IRQ —
a collision DOS never had, since it lived in real mode.

```
RAX                  function number
RDI, RSI, RDX, RCX   arguments, in that order
RAX                  return value
```

Every other register is preserved. Function numbers and structures are in
`syscall.h`, which the kernel and every program include from the same copy, so
the two cannot drift apart.

`SYSCALL`/`SYSRET` is not used, and the reason is worth knowing: its whole value
is a fast ring 3 to ring 0 transition, and Koi-DOS is a ring-0 monolith where
that transition does not happen.

## Where your program lives

`program.ld` links at a fixed **16 MiB**, and the kernel's page allocator is
told never to hand out that window. Programs are not relocatable and do not need
to be. One program runs at a time.

Your program runs in **ring 0 with no memory protection**, exactly as a DOS
program did. That means two things. You can talk to hardware directly if you
want to — `fetch` uses `CPUID` with no system call, because there is no reason
for one. And nothing stops you from corrupting the kernel: a stray pointer is a
panic screen, not a segmentation fault. The stack is yours and is not large;
recursion and multi-kilobyte locals are not free.

## Rebuilding this directory

These files are copies of the kernel tree's originals, refreshed by every build
of the kernel rather than by hand. Kept as a manual step it drifted within a
day - the linker script gained a section, the copy did not, and programs built
here were quietly missing it. Four `cp` lines on every build cost nothing and
make that impossible.
