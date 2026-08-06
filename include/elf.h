#ifndef ELF_H
#define ELF_H

/* Minimal ELF64 definitions - only what a loader needs to walk program
   headers. Used by the bootloader today; the program loader in Stage 3 will
   want the same structures. Deliberately free of UEFI and kernel types so
   both sides can include it. */

typedef unsigned char      elf_uint8_t;
typedef unsigned short     elf_uint16_t;
typedef unsigned int       elf_uint32_t;
typedef unsigned long long elf_uint64_t;

#define ELF_IDENT_SIZE 16

/* e_ident indices and their expected values. */
#define ELF_INDEX_CLASS 4
#define ELF_INDEX_DATA 5
#define ELF_INDEX_VERSION 6

#define ELF_CLASS_64 2      /* 64-bit objects */
#define ELF_DATA_LSB 1      /* little endian */
#define ELF_VERSION_CURRENT 1

#define ELF_TYPE_EXEC 2     /* e_type: executable file */
#define ELF_MACHINE_X86_64 62

#define PT_LOAD 1           /* p_type: segment to be mapped into memory */

typedef struct {
    elf_uint8_t  e_ident[ELF_IDENT_SIZE];
    elf_uint16_t e_type;
    elf_uint16_t e_machine;
    elf_uint32_t e_version;
    elf_uint64_t e_entry;
    elf_uint64_t e_phoff;
    elf_uint64_t e_shoff;
    elf_uint32_t e_flags;
    elf_uint16_t e_ehsize;
    elf_uint16_t e_phentsize;
    elf_uint16_t e_phnum;
    elf_uint16_t e_shentsize;
    elf_uint16_t e_shnum;
    elf_uint16_t e_shstrndx;
} ELF64_HEADER;

typedef struct {
    elf_uint32_t p_type;
    elf_uint32_t p_flags;
    elf_uint64_t p_offset;   /* byte offset of the segment in the file */
    elf_uint64_t p_vaddr;
    elf_uint64_t p_paddr;    /* where to place it; the kernel is not relocated */
    elf_uint64_t p_filesz;   /* bytes present in the file */
    elf_uint64_t p_memsz;    /* bytes to occupy in memory; the excess is .bss */
    elf_uint64_t p_align;
} ELF64_PROGRAM_HEADER;

#endif
