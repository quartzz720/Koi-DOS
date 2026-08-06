#ifndef HAL_H
#define HAL_H

#include "../include/efi.h"

// ============================================================================
// Hardware Abstraction Layer (HAL)
// Provides abstraction from UEFI for console, disk I/O
// ============================================================================

// Initialize HAL with UEFI system table and image handle
void hal_init(EFI_SYSTEM_TABLE* ST, EFI_HANDLE IH);

// Get UEFI system table and image handle (for compatibility during Phase 1)
EFI_SYSTEM_TABLE* hal_get_system_table(void);
EFI_HANDLE hal_get_image_handle(void);

// ============================================================================
// Console I/O Functions
// ============================================================================

void hal_console_write(CHAR16* str);
void hal_console_write_char(CHAR16 ch);
void hal_console_clear(void);
void hal_console_set_color(UINTN fg, UINTN bg);
void hal_console_read_key(EFI_INPUT_KEY* key);

// Shutdown the system (UEFI ResetSystem) (forgot about this one)
void hal_quit(void);

// ============================================================================
// Memory Functions
// ============================================================================

void hal_memcpy(void* dst, void* src, UINTN size);
void hal_memset(void* dst, UINT8 value, UINTN size);

// ============================================================================
// Disk I/O Functions
// ============================================================================

// Read sector(s) from disk
// Returns EFI_SUCCESS on success
EFI_STATUS hal_disk_read(UINT64 lba, UINTN num_sectors, void* buffer);

// Write sector(s) to disk
// Returns EFI_SUCCESS on success
EFI_STATUS hal_disk_write(UINT64 lba, UINTN num_sectors, void* buffer);

#endif

