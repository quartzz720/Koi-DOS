#include "../include/efi.h"
#include "../include/bootinfo.h"
#include "../include/elf.h"

_Static_assert(sizeof(BOOT_MEMORY_DESCRIPTOR) == sizeof(EFI_MEMORY_DESCRIPTOR),
               "Boot memory descriptor must match the UEFI descriptor layout");

#define KERNEL_PATH L"\\BOOT\\KERNEL.ELF"
#define KERNEL_MAX_IMAGE (16ULL * 1024ULL * 1024ULL)
#define KERNEL_STACK_PAGES 16ULL
#define MEMORY_MAP_SLACK 8ULL
#define PAGE_SIZE 4096ULL

static BOOT_INFO boot_info;

static EFI_SYSTEM_TABLE* system_table;

/* The kernel entry point taken from the ELF header, not the image base. */
static UINT64 kernel_entry;

/* GCC turns byte loops and zero-initialised aggregates into calls to these
   even under -ffreestanding, so the freestanding bootloader has to supply
   them itself. */
void* memset(void* destination, int value, UINTN size) {
    UINT8* bytes = (UINT8*)destination;
    for (UINTN i = 0; i < size; i++) bytes[i] = (UINT8)value;
    return destination;
}

void* memcpy(void* destination, const void* source, UINTN size) {
    UINT8* to = (UINT8*)destination;
    const UINT8* from = (const UINT8*)source;
    for (UINTN i = 0; i < size; i++) to[i] = from[i];
    return destination;
}

/* Raw COM1 output. Unlike ConOut this keeps working after ExitBootServices,
   which is the one window where the bootloader can still fail with nothing
   else able to report it.
   The spin is bounded: on a machine with no COM1 the transmit-empty bit never
   arrives, and an unbounded wait would hang the boot instead of logging it. */
static void trace(const char* text) {
    while (*text) {
        UINT8 line_status = 0;
        for (UINTN spin = 0; spin < 100000; spin++) {
            __asm__ volatile ("inb %1, %0" : "=a"(line_status) : "Nd"((UINT16)0x3FD));
            if (line_status & 0x20) break;
        }
        if (!(line_status & 0x20)) return;
        __asm__ volatile ("outb %0, %1" : : "a"((UINT8)*text), "Nd"((UINT16)0x3F8));
        text++;
    }
}

static EFI_STATUS boot_fail(CHAR16* stage, EFI_STATUS status) {
    static const CHAR16 hex[] = L"0123456789ABCDEF";
    CHAR16 value[19];

    value[0] = L'0';
    value[1] = L'x';
    for (UINTN i = 0; i < 16; i++)
        value[2 + i] = hex[((UINT64)status >> ((15 - i) * 4)) & 0xF];
    value[18] = 0;
    trace("KOI BOOT: FAILED\r\n");
    system_table->ConOut->OutputString(system_table->ConOut, L"\r\nBOOT FAIL: ");
    system_table->ConOut->OutputString(system_table->ConOut, stage);
    system_table->ConOut->OutputString(system_table->ConOut, L" ");
    system_table->ConOut->OutputString(system_table->ConOut, value);
    system_table->ConOut->OutputString(system_table->ConOut, L"\r\n");
    return status;
}

/* Read the whole kernel file into pool memory. The caller frees it. */
static EFI_STATUS read_kernel_file(EFI_HANDLE image_handle, UINT8** contents,
                                   UINTN* length) {
    EFI_GUID loaded_image_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_GUID filesystem_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_GUID file_info_guid = EFI_FILE_INFO_GUID;
    EFI_LOADED_IMAGE_PROTOCOL* image = NULL;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL* filesystem = NULL;
    EFI_FILE_PROTOCOL* root = NULL;
    EFI_FILE_PROTOCOL* kernel = NULL;
    EFI_FILE_INFO info;
    UINTN info_size = sizeof(info);
    UINT8* buffer = NULL;
    UINTN size;
    EFI_STATUS status;

    status = system_table->BootServices->HandleProtocol(image_handle,
        &loaded_image_guid, (void**)&image);
    if (status != EFI_SUCCESS) return status;
    status = system_table->BootServices->HandleProtocol(image->DeviceHandle,
        &filesystem_guid, (void**)&filesystem);
    if (status != EFI_SUCCESS) return status;
    status = filesystem->OpenVolume(filesystem, &root);
    if (status != EFI_SUCCESS) return status;
    status = root->Open(root, &kernel, KERNEL_PATH, EFI_FILE_MODE_READ, 0);
    root->Close(root);
    if (status != EFI_SUCCESS) return status;

    status = kernel->GetInfo(kernel, &file_info_guid, &info_size, &info);
    if (status != EFI_SUCCESS) { kernel->Close(kernel); return status; }
    size = (UINTN)info.FileSize;
    if (size < sizeof(ELF64_HEADER) || size > KERNEL_MAX_IMAGE) {
        kernel->Close(kernel);
        return EFI_OUT_OF_RESOURCES;
    }

    status = system_table->BootServices->AllocatePool(EfiLoaderData, size,
        (void**)&buffer);
    if (status != EFI_SUCCESS) { kernel->Close(kernel); return status; }

    status = kernel->Read(kernel, &size, buffer);
    kernel->Close(kernel);
    if (status != EFI_SUCCESS) {
        system_table->BootServices->FreePool(buffer);
        return status;
    }

    *contents = buffer;
    *length = size;
    return EFI_SUCCESS;
}

/* Read the boot sector of the device we were loaded from and take the FAT
   volume serial out of it, so the kernel can recognise the same volume among
   whatever its own drivers find. BlockIO lives on the device handle, not on
   the image handle. */
static void get_boot_volume_serial(EFI_HANDLE image_handle) {
    EFI_GUID loaded_image_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_GUID block_io_guid = EFI_BLOCK_IO_PROTOCOL_GUID;
    EFI_LOADED_IMAGE_PROTOCOL* image = NULL;
    EFI_BLOCK_IO_PROTOCOL* block_io = NULL;
    UINT8 sector[512];

    boot_info.boot_volume_known = 0;
    if (system_table->BootServices->HandleProtocol(image_handle,
            &loaded_image_guid, (void**)&image) != EFI_SUCCESS || !image)
        return;
    if (system_table->BootServices->HandleProtocol(image->DeviceHandle,
            &block_io_guid, (void**)&block_io) != EFI_SUCCESS || !block_io)
        return;
    if (!block_io->Media || block_io->Media->BlockSize > sizeof(sector)) return;
    if (block_io->ReadBlocks(block_io, block_io->Media->MediaId, 0,
                             block_io->Media->BlockSize, sector) != EFI_SUCCESS)
        return;
    /* Offset 67 in a FAT32 BPB. Signature 0x29 at offset 66 says the extended
       fields - serial and label - are actually there. */
    if (sector[66] != 0x29) return;
    boot_info.boot_volume_serial = (UINT32)sector[67] |
                                   ((UINT32)sector[68] << 8) |
                                   ((UINT32)sector[69] << 16) |
                                   ((UINT32)sector[70] << 24);
    boot_info.boot_volume_known = 1;
}

static EFI_STATUS validate_elf(const ELF64_HEADER* header, UINTN length) {
    if (header->e_ident[0] != 0x7F || header->e_ident[1] != 'E' ||
        header->e_ident[2] != 'L' || header->e_ident[3] != 'F')
        return EFI_INVALID_PARAMETER;
    if (header->e_ident[ELF_INDEX_CLASS] != ELF_CLASS_64 ||
        header->e_ident[ELF_INDEX_DATA] != ELF_DATA_LSB ||
        header->e_ident[ELF_INDEX_VERSION] != ELF_VERSION_CURRENT)
        return EFI_INVALID_PARAMETER;
    if (header->e_type != ELF_TYPE_EXEC ||
        header->e_machine != ELF_MACHINE_X86_64)
        return EFI_INVALID_PARAMETER;
    if (header->e_phnum == 0 ||
        header->e_phentsize != sizeof(ELF64_PROGRAM_HEADER))
        return EFI_INVALID_PARAMETER;
    if (header->e_phoff > length ||
        (UINT64)header->e_phnum * sizeof(ELF64_PROGRAM_HEADER) >
            length - header->e_phoff)
        return EFI_INVALID_PARAMETER;
    return EFI_SUCCESS;
}

/* Load the kernel where it was linked to run.
 *
 * The kernel is a -no-pie image with absolute addresses baked in, so it has to
 * be placed at its link address. The whole PT_LOAD span is allocated in one
 * AllocateAddress call and zeroed first: that covers .bss (which occupies no
 * space in the file) and any padding between segments, so nothing is left
 * holding firmware garbage. */
static EFI_STATUS load_kernel(EFI_HANDLE image_handle) {
    UINT8* contents = NULL;
    UINTN length = 0;
    const ELF64_HEADER* header;
    const ELF64_PROGRAM_HEADER* segments;
    EFI_PHYSICAL_ADDRESS lowest = ~0ULL;
    EFI_PHYSICAL_ADDRESS highest = 0;
    EFI_PHYSICAL_ADDRESS base;
    UINTN pages;
    EFI_STATUS status;

    status = read_kernel_file(image_handle, &contents, &length);
    if (status != EFI_SUCCESS) return status;

    header = (const ELF64_HEADER*)contents;
    status = validate_elf(header, length);
    if (status != EFI_SUCCESS) {
        system_table->BootServices->FreePool(contents);
        return status;
    }
    segments = (const ELF64_PROGRAM_HEADER*)(contents + header->e_phoff);

    for (UINT16 i = 0; i < header->e_phnum; i++) {
        const ELF64_PROGRAM_HEADER* segment = &segments[i];
        if (segment->p_type != PT_LOAD || segment->p_memsz == 0) continue;
        if (segment->p_filesz > segment->p_memsz ||
            segment->p_offset > length ||
            segment->p_filesz > length - segment->p_offset) {
            system_table->BootServices->FreePool(contents);
            return EFI_INVALID_PARAMETER;
        }
        if (segment->p_paddr < lowest) lowest = segment->p_paddr;
        if (segment->p_paddr + segment->p_memsz > highest)
            highest = segment->p_paddr + segment->p_memsz;
    }
    if (lowest > highest) {
        system_table->BootServices->FreePool(contents);
        return EFI_INVALID_PARAMETER;
    }

    base = lowest & ~(PAGE_SIZE - 1);
    highest = (highest + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    if (highest - base > KERNEL_MAX_IMAGE) {
        system_table->BootServices->FreePool(contents);
        return EFI_OUT_OF_RESOURCES;
    }
    pages = (UINTN)((highest - base) / PAGE_SIZE);

    status = system_table->BootServices->AllocatePages(AllocateAddress,
        EfiLoaderData, pages, &base);
    if (status != EFI_SUCCESS) {
        system_table->BootServices->FreePool(contents);
        return status;
    }

    memset((void*)(UINTN)base, 0, (UINTN)(highest - base));
    for (UINT16 i = 0; i < header->e_phnum; i++) {
        const ELF64_PROGRAM_HEADER* segment = &segments[i];
        if (segment->p_type != PT_LOAD || segment->p_filesz == 0) continue;
        memcpy((void*)(UINTN)segment->p_paddr, contents + segment->p_offset,
               (UINTN)segment->p_filesz);
    }

    /* Take the entry point before releasing the buffer: `header` points into
       it, and firmware poisons freed pool memory. */
    kernel_entry = header->e_entry;
    system_table->BootServices->FreePool(contents);

    boot_info.kernel_image_base = base;
    boot_info.kernel_image_size = highest - base;
    return EFI_SUCCESS;
}

static EFI_STATUS get_framebuffer(void) {
    EFI_GUID graphics_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    EFI_GRAPHICS_OUTPUT_PROTOCOL* graphics = NULL;
    EFI_STATUS status = system_table->BootServices->LocateProtocol(
        &graphics_guid, NULL, (void**)&graphics);
    if (status != EFI_SUCCESS || !graphics || !graphics->Mode || !graphics->Mode->Info)
        return EFI_NOT_FOUND;

    boot_info.framebuffer_base = (UINT64)graphics->Mode->FrameBufferBase;
    boot_info.framebuffer_size = graphics->Mode->FrameBufferSize;
    boot_info.framebuffer_width = graphics->Mode->Info->HorizontalResolution;
    boot_info.framebuffer_height = graphics->Mode->Info->VerticalResolution;
    boot_info.framebuffer_pixels_per_scan_line = graphics->Mode->Info->PixelsPerScanLine;
    boot_info.framebuffer_pixel_format = graphics->Mode->Info->PixelFormat;
    return EFI_SUCCESS;
}

static EFI_STATUS get_final_memory_map(UINTN* map_key) {
    EFI_MEMORY_DESCRIPTOR* map = NULL;
    UINTN map_size = 0;
    UINTN descriptor_size = 0;
    UINT32 descriptor_version;
    EFI_STATUS status;

    for (;;) {
        status = system_table->BootServices->GetMemoryMap(&map_size, map, map_key,
            &descriptor_size, &descriptor_version);
        if (status != EFI_BUFFER_TOO_SMALL) {
            if (map) system_table->BootServices->FreePool(map);
            return status;
        }
        if (map) system_table->BootServices->FreePool(map);
        map = NULL;
        map_size += descriptor_size * MEMORY_MAP_SLACK;
        status = system_table->BootServices->AllocatePool(EfiLoaderData, map_size,
            (void**)&map);
        if (status != EFI_SUCCESS) return status;
        status = system_table->BootServices->GetMemoryMap(&map_size, map, map_key,
            &descriptor_size, &descriptor_version);
        if (status == EFI_SUCCESS) {
            boot_info.memory_map = (UINT64)(UINTN)map;
            boot_info.memory_map_size = map_size;
            boot_info.memory_map_descriptor_size = descriptor_size;
            boot_info.memory_map_descriptor_version = descriptor_version;
            return EFI_SUCCESS;
        }
        if (status != EFI_BUFFER_TOO_SMALL) {
            system_table->BootServices->FreePool(map);
            return status;
        }
    }
}

static BOOLEAN guid_equal(const EFI_GUID* left, const EFI_GUID* right) {
    const UINT8* a = (const UINT8*)left;
    const UINT8* b = (const UINT8*)right;
    for (UINTN i = 0; i < sizeof(EFI_GUID); i++) if (a[i] != b[i]) return FALSE;
    return TRUE;
}

static void get_acpi_rsdp(void) {
    EFI_GUID acpi20_guid =
        { 0x8868E871, 0xE4F1, 0x11D3, { 0xBC, 0x22, 0x00, 0x80, 0xC7, 0x3C, 0x88, 0x81 } };
    EFI_GUID acpi10_guid =
        { 0xEB9D2D30, 0x2D88, 0x11D3, { 0x9A, 0x16, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0x4D } };

    for (UINTN i = 0; i < system_table->NumberOfTableEntries; i++) {
        EFI_CONFIGURATION_TABLE* table = &system_table->ConfigurationTable[i];
        if (guid_equal(&table->VendorGuid, &acpi20_guid) ||
            guid_equal(&table->VendorGuid, &acpi10_guid)) {
            boot_info.acpi_rsdp = (UINT64)(UINTN)table->VendorTable;
            return;
        }
    }
}

__attribute__((noreturn)) static void jump_to_kernel(void* entry, BOOT_INFO* info,
                                                       UINT64 stack_top) {
    __asm__ volatile (
        "mov %0, %%rsp\n"
        "and $-16, %%rsp\n"
        "sub $8, %%rsp\n"
        "mov %1, %%rdi\n"
        "jmp *%2\n"
        : : "r"(stack_top), "r"(info), "r"(entry) : "memory");
    __builtin_unreachable();
}

EFI_STATUS efi_main(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE* st) {
    EFI_PHYSICAL_ADDRESS stack = 0;
    UINTN map_key;
    EFI_STATUS status;

    system_table = st;
    trace("\r\nKOI BOOT: start\r\n");
    get_acpi_rsdp();
    status = get_framebuffer();
    if (status != EFI_SUCCESS) return boot_fail(L"framebuffer", status);
    get_boot_volume_serial(image_handle);
    status = load_kernel(image_handle);
    if (status != EFI_SUCCESS) return boot_fail(L"kernel load", status);
    trace("KOI BOOT: kernel loaded\r\n");
    status = st->BootServices->AllocatePages(AllocateAnyPages, EfiLoaderData,
        KERNEL_STACK_PAGES, &stack);
    if (status != EFI_SUCCESS) return boot_fail(L"kernel stack", status);

    for (;;) {
        status = get_final_memory_map(&map_key);
        if (status != EFI_SUCCESS) return boot_fail(L"memory map", status);
        status = st->BootServices->ExitBootServices(image_handle, map_key);
        if (status == EFI_SUCCESS) break;
        /* EFI_INVALID_PARAMETER means the map changed under us; take a fresh
           one and retry. Anything else is fatal. */
        if (status != EFI_INVALID_PARAMETER) return boot_fail(L"ExitBootServices", status);
    }
    /* No UEFI service may be called past this point - ConOut included. */
    trace("KOI BOOT: boot services off, entering kernel\r\n");
    boot_info.kernel_stack_base = stack;
    boot_info.kernel_stack_size = KERNEL_STACK_PAGES * PAGE_SIZE;
    jump_to_kernel((void*)(UINTN)kernel_entry, &boot_info,
                   stack + KERNEL_STACK_PAGES * PAGE_SIZE);
}
