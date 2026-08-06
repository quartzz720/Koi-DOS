BOOT_CC = x86_64-w64-mingw32-gcc
KERNEL_CC = gcc

BOOT_CFLAGS = -ffreestanding -fno-stack-protector -fshort-wchar -mno-red-zone \
              -fno-builtin -fno-stack-check -Wall -Wextra -Werror -std=c11 -I include
BOOT_LDFLAGS = -nostdlib -Wl,--subsystem,10 -Wl,-e,efi_main -shared

# -fno-pie: the kernel is placed at its link address by the bootloader's ELF
#   loader, so absolute addresses are correct and no relocation is needed.
# -mgeneral-regs-only: forbid GCC from emitting SSE for struct copies. After
#   ExitBootServices the SSE state is whatever the firmware left behind, and
#   nothing in the kernel has set it up yet.
KERNEL_CFLAGS = -ffreestanding -fno-stack-protector -fno-stack-check -fno-builtin \
                -fno-pie -mno-red-zone -mgeneral-regs-only \
                -fno-asynchronous-unwind-tables \
                -Wall -Wextra -Werror -std=c11 -I include
# max-page-size=0x1000 keeps PT_LOAD segments 4 KiB aligned; the x86-64 default
# of 2 MiB would pad the image out with megabytes of zeroes.
KERNEL_LDFLAGS = -nostdlib -no-pie -Wl,-T,linker.ld -Wl,--build-id=none \
                 -Wl,-z,max-page-size=0x1000 -Wl,-z,noexecstack

EFI_DIR = boot/efi/boot
KERNEL_ELF = kernel/kernel.elf
KERNEL_IMAGE = $(EFI_DIR)/KERNEL.ELF

KERNEL_SOURCES = kernel/kernel.c kernel/console.c kernel/font.c kernel/serial.c \
                 kernel/string.c kernel/memory.c kernel/cpu.c kernel/idt.c \
                 kernel/isr.S kernel/pic.c kernel/acpi.c kernel/keyboard.c \
                 kernel/heap.c kernel/paging.c kernel/rtc.c kernel/block.c \
                 kernel/partition.c kernel/fat32.c kernel/command.c \
                 kernel/syscall.c kernel/program.c kernel/config.c \
                 kernel/timer.c kernel/pci.c kernel/ahci.c
KERNEL_HEADERS = kernel/kernel.h kernel/console.h kernel/font.h kernel/serial.h \
                 kernel/string.h kernel/io.h kernel/memory.h kernel/cpu.h \
                 kernel/idt.h kernel/pic.h kernel/acpi.h kernel/keyboard.h \
                 kernel/heap.h kernel/paging.h kernel/rtc.h kernel/block.h \
                 kernel/partition.h kernel/fat32.h kernel/command.h \
                 kernel/syscall.h kernel/program.h kernel/config.h include/syscall.h \
                 kernel/timer.h kernel/pci.h kernel/ahci.h include/bootinfo.h

# Programs are built with the same compiler as the kernel but their own linker
# script, and end up as .EXE files on the disk image. The format underneath is
# ELF64; the extension is the DOS-familiar one.
PROGRAM_CFLAGS = -ffreestanding -fno-stack-protector -fno-stack-check \
                 -fno-builtin -fno-pie -mno-red-zone -mgeneral-regs-only \
                 -fno-asynchronous-unwind-tables \
                 -Wall -Wextra -Werror -std=c11 -I .
PROGRAM_LDFLAGS = -nostdlib -no-pie -Wl,-T,programs/program.ld \
                  -Wl,--build-id=none -Wl,-z,max-page-size=0x1000 \
                  -Wl,-z,noexecstack

PROGRAM_SOURCES = $(wildcard programs/*.c)
PROGRAMS = $(patsubst programs/%.c,build/%.EXE,\
             $(filter-out programs/start.c,$(PROGRAM_SOURCES)))

all: $(EFI_DIR)/BOOTX64.EFI $(KERNEL_IMAGE) $(PROGRAMS)

build/%.EXE: programs/%.c programs/start.c programs/koi.h programs/program.ld include/syscall.h
	mkdir -p build
	$(KERNEL_CC) $(PROGRAM_CFLAGS) $(PROGRAM_LDFLAGS) -o $@ $< programs/start.c

$(EFI_DIR)/BOOTX64.EFI: boot/bootloader.c include/efi.h include/bootinfo.h include/elf.h
	mkdir -p $(EFI_DIR)
	$(BOOT_CC) $(BOOT_CFLAGS) $(BOOT_LDFLAGS) -o $@ boot/bootloader.c

$(KERNEL_ELF): $(KERNEL_SOURCES) $(KERNEL_HEADERS) linker.ld
	$(KERNEL_CC) $(KERNEL_CFLAGS) $(KERNEL_LDFLAGS) -o $@ $(KERNEL_SOURCES)

$(KERNEL_IMAGE): $(KERNEL_ELF)
	mkdir -p $(EFI_DIR)
	cp $(KERNEL_ELF) $@

# Sanity checks that catch the failure modes this build is prone to:
# unresolved symbols (a missing memset/memcpy) and relocations left in the
# image (which would mean the kernel is not actually position-fixed).
check: $(KERNEL_ELF)
	@echo "--- undefined symbols (must be empty) ---"
	@nm -u $(KERNEL_ELF)
	@echo "--- relocations (must be none) ---"
	@readelf -r $(KERNEL_ELF)
	@echo "--- program headers ---"
	@readelf -lW $(KERNEL_ELF)

clean:
	rm -f $(EFI_DIR)/BOOTX64.EFI $(KERNEL_IMAGE) $(EFI_DIR)/KERNEL.BIN $(KERNEL_ELF)
	rm -rf build

.PHONY: all check clean
