#include "../include/efi.h"
#include "hal.h"

// ============================================================================
// Global UEFI state - stored only in hal.c
// ============================================================================

static EFI_SYSTEM_TABLE* hal_st = NULL;
static EFI_HANDLE hal_ih = NULL;
static EFI_BLOCK_IO_PROTOCOL* hal_block_io = NULL;

// ============================================================================
// HAL Initialization
// ============================================================================

void hal_init(EFI_SYSTEM_TABLE* ST, EFI_HANDLE IH) {
    hal_st = ST;
    hal_ih = IH;
    
    // Try to locate BlockIO protocol for disk access.
    // NOTE: BlockIO lives on the underlying DEVICE handle, not on the
    // loaded-image handle itself - same reasoning as open_root() in fs.c.
    // We have to go through LoadedImageProtocol->DeviceHandle first.
    EFI_GUID lip_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_LOADED_IMAGE_PROTOCOL* lip = NULL;
    EFI_STATUS status = ST->BootServices->HandleProtocol(
        IH, &lip_guid, (void**)&lip);
    
    if (status != EFI_SUCCESS || !lip) {
        hal_block_io = NULL;
        return;
    }
    
    EFI_GUID block_io_guid = EFI_BLOCK_IO_PROTOCOL_GUID;
    status = ST->BootServices->HandleProtocol(
        lip->DeviceHandle, &block_io_guid, (void**)&hal_block_io);
    
    if (status != EFI_SUCCESS) {
        hal_block_io = NULL;
    }
}

// Get UEFI system table and image handle (for compatibility during Phase 1)
EFI_SYSTEM_TABLE* hal_get_system_table(void) {
    return hal_st;
}

EFI_HANDLE hal_get_image_handle(void) {
    return hal_ih;
}

// ============================================================================
// Console I/O Implementation
// ============================================================================

void hal_console_write(CHAR16* str) {
    if (!hal_st) return;
    hal_st->ConOut->OutputString(hal_st->ConOut, str);
}

void hal_console_write_char(CHAR16 ch) {
    CHAR16 buf[2] = { ch, 0 };
    hal_console_write(buf);
}

void hal_console_clear(void) {
    if (!hal_st) return;
    hal_st->ConOut->ClearScreen(hal_st->ConOut);
}

void hal_console_set_color(UINTN fg, UINTN bg) {
    if (!hal_st) return;
    hal_st->ConOut->SetAttribute(hal_st->ConOut, fg | (bg << 4));
}

void hal_console_read_key(EFI_INPUT_KEY* key) {
    if (!hal_st) return;
    while (hal_st->ConIn->ReadKeyStroke(hal_st->ConIn, key) != EFI_SUCCESS);
}

// that fucking shutdown function I forgot about, whoops
void hal_quit(void) {
    if (hal_st != NULL && hal_st->RuntimeServices != NULL) {
        hal_st->RuntimeServices->ResetSystem(2, EFI_SUCCESS, 0, NULL);
    }
}

// ============================================================================
// Memory Functions
// ============================================================================

void hal_memcpy(void* dst, void* src, UINTN size) {
    UINT8* d = (UINT8*)dst;
    UINT8* s = (UINT8*)src;
    for (UINTN i = 0; i < size; i++) {
        d[i] = s[i];
    }
}

void hal_memset(void* dst, UINT8 value, UINTN size) {
    UINT8* d = (UINT8*)dst;
    for (UINTN i = 0; i < size; i++) {
        d[i] = value;
    }
}

/* GCC may emit this symbol for zero-initialized local aggregates. */
void* memset(void* dst, int value, UINTN size) {
    hal_memset(dst, (UINT8)value, size);
    return dst;
}

// ============================================================================
// Disk I/O Implementation
// ============================================================================

EFI_STATUS hal_disk_read(UINT64 lba, UINTN num_sectors, void* buffer) {
    if (!hal_block_io) return EFI_NOT_FOUND;
    
    // BlockIO::ReadBlocks() in UEFI
    return hal_block_io->ReadBlocks(
        hal_block_io,
        hal_block_io->Media->MediaId,
        lba,
        num_sectors * 512,
        buffer
    );
}

EFI_STATUS hal_disk_write(UINT64 lba, UINTN num_sectors, void* buffer) {
    if (!hal_block_io) return EFI_NOT_FOUND;
    
    // BlockIO::WriteBlocks() in UEFI
    return hal_block_io->WriteBlocks(
        hal_block_io,
        hal_block_io->Media->MediaId,
        lba,
        num_sectors * 512,
        buffer
    );
}
