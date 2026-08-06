# legacy/ — the old UEFI Boot Services shell

This code is **not built and is not part of the OS**. It is kept here as a reference.

## What it is

The first version of Koi-DOS was a UEFI application: it ran inside Boot Services and did
everything through someone else's hands — `ConOut->OutputString` for output,
`ConIn->ReadKeyStroke` for input, `EFI_FILE_PROTOCOL` for files. Once the project moved to a real
kernel (`ExitBootServices` plus its own drivers), this whole layer stopped compiling: `console.h`
was rewritten around the framebuffer, and `print`, `println`, `set_color` and `read_line` no
longer exist.

## Why it was kept

All the command semantics that have to be reproduced on top of the new kernel were worked out
here:

- [shell.c](shell.c) — the command table and dispatcher;
- [fs.c](fs.c) — relative and absolute path resolution (`build_path`), `cwd`, and the behaviour
  of `dir`, `cd`, `type`, `more`, `tree`, `copy`, `move`, `ren`, `del`, `mkdir`, `rmdir`, plus
  the line editor `edit`;
- [commands.c](commands.c) — `help`, `ver`, `echo`, `color`;
- [hal.c](hal.c) — an attempt to abstract UEFI away. The new kernel does not need it, but it
  shows which operations were considered primitives.

When `kernel/command.c` gets written (Stage 2.4), take the command logic from here — only the
layer underneath it changes.

## What is not here

The `Z:\>` prompt from [shell.c:70](shell.c#L70) is the one thing that carried over as-is: the
system volume in Koi-DOS is called `Z:`, not `C:`.
