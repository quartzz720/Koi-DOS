#ifndef KERNEL_H
#define KERNEL_H

// ============================================================================
// Kernel Entry Point
// Called after HAL initialization
// ============================================================================

#include "../include/bootinfo.h"

void kernel_main(BOOT_INFO* info);

#endif
