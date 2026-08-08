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
BUILD_HEADER = kernel/build.h

KERNEL_SOURCES = kernel/kernel.c kernel/console.c kernel/font.c kernel/serial.c \
                 kernel/string.c kernel/memory.c kernel/cpu.c kernel/idt.c \
                 kernel/isr.S kernel/pic.c kernel/acpi.c kernel/keyboard.c \
                 kernel/heap.c kernel/paging.c kernel/rtc.c kernel/block.c \
                 kernel/partition.c kernel/fat32.c kernel/command.c \
                 kernel/syscall.c kernel/program.c kernel/config.c kernel/xhci.c \
                 kernel/timer.c kernel/pci.c kernel/ahci.c kernel/nvme.c kernel/hpet.c \
                 kernel/apic.c kernel/graphics.c kernel/hda.c kernel/audio.c kernel/net.c kernel/ehci.c kernel/e1000.c kernel/tftp.c
KERNEL_HEADERS = kernel/kernel.h kernel/console.h kernel/font.h kernel/serial.h \
                 kernel/string.h kernel/io.h kernel/memory.h kernel/cpu.h \
                 kernel/idt.h kernel/pic.h kernel/acpi.h kernel/keyboard.h \
                 kernel/heap.h kernel/paging.h kernel/rtc.h kernel/block.h \
                 kernel/partition.h kernel/fat32.h kernel/command.h \
                 kernel/syscall.h kernel/program.h kernel/config.h kernel/xhci.h \
                 kernel/graphics.h kernel/hda.h kernel/audio.h kernel/net.h kernel/ehci.h kernel/e1000.h kernel/tftp.h \
                 include/syscall.h $(BUILD_HEADER) \
                 kernel/timer.h kernel/pci.h kernel/ahci.h kernel/nvme.h kernel/hpet.h kernel/apic.h \
                 include/bootinfo.h

# Programs are built with the same compiler as the kernel but their own linker
# script, and end up as .EXE files on the disk image. The format underneath is
# ELF64; the extension is the DOS-familiar one.
PROGRAM_CFLAGS = -ffreestanding -fno-stack-protector -fno-stack-check \
                 -fno-builtin -fno-pie -mno-red-zone -mgeneral-regs-only \
                 -fno-asynchronous-unwind-tables \
                 -ffunction-sections -fdata-sections \
                 -Wall -Wextra -Werror -std=c11 -I . -I include
# --gc-sections drops the parts of koilib.c a program never calls, so a program
# that wants strlen does not also carry a formatter and a heap. Safe with our
# linker script because it KEEPs .koi_abi, which nothing references and which
# would otherwise be the first thing collected.
PROGRAM_LDFLAGS = -nostdlib -no-pie -Wl,-T,programs/program.ld \
                  -Wl,--build-id=none -Wl,-z,max-page-size=0x1000 \
                  -Wl,-z,noexecstack -Wl,--gc-sections

PROGRAM_SOURCES = $(wildcard programs/*.c)
PROGRAMS = $(patsubst programs/%.c,build/%.EXE,\
             $(filter-out programs/start.c programs/koilib.c,$(PROGRAM_SOURCES)))

all: $(EFI_DIR)/BOOTX64.EFI $(KERNEL_IMAGE) $(PROGRAMS) sdk

# The build stamp: commit count as a build number, the date, and the commit
# itself with a `+` when the tree is dirty. Generated rather than checked in,
# and rewritten only when it actually changes - otherwise every `make` would
# relink the kernel whether or not anything moved. Outside a git checkout the
# numbers fall back to zero rather than failing the build.
$(BUILD_HEADER): FORCE
	@printf '#ifndef KERNEL_BUILD_H\n#define KERNEL_BUILD_H\n#define KOI_BUILD_NUMBER %s\n#define KOI_BUILD_DATE "%s"\n#define KOI_BUILD_COMMIT "%s"\n#endif\n' \
	  "$$(git rev-list --count HEAD 2>/dev/null || echo 0)" \
	  "$$(date -u +%Y-%m-%d)" \
	  "$$(git describe --always --dirty=+ --abbrev=7 2>/dev/null || echo unknown)" > $@.tmp
	@cmp -s $@.tmp $@ || mv -f $@.tmp $@
	@rm -f $@.tmp

FORCE:

build/%.EXE: programs/%.c programs/start.c programs/koilib.c programs/koi.h programs/program.ld include/syscall.h
	mkdir -p build
	$(KERNEL_CC) $(PROGRAM_CFLAGS) $(PROGRAM_LDFLAGS) -o $@ $< programs/start.c programs/koilib.c

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

# The SDK is a copy of the files a third-party program needs, kept where it can
# be handed to someone without the kernel source.
#
# Part of `all` on purpose. Kept as a manual step it drifted within a day - the
# linker script gained a section, the copy did not, and programs built with the
# SDK were quietly missing it. Copying four files on every build costs nothing
# and makes that impossible.
sdk:
	cp programs/koi.h programs/start.c programs/koilib.c programs/program.ld sdk/
	cp include/syscall.h sdk/syscall.h
	@echo "sdk/ refreshed"

clean:
	rm -f $(EFI_DIR)/BOOTX64.EFI $(KERNEL_IMAGE) $(EFI_DIR)/KERNEL.BIN $(KERNEL_ELF)
	rm -f $(BUILD_HEADER)
	rm -rf build

.PHONY: all check clean sdk FORCE
