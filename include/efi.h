#ifndef EFI_H
#define EFI_H

typedef unsigned short     CHAR16;
typedef unsigned long long UINT64;
typedef unsigned int       UINT32;
typedef unsigned short     UINT16;
typedef unsigned char      UINT8;
typedef unsigned long long UINTN;
typedef long long          INTN;
typedef long long          EFI_STATUS;
typedef void*              EFI_HANDLE;
typedef UINT8              BOOLEAN;
typedef UINT64             EFI_LBA;
typedef UINT64             EFI_PHYSICAL_ADDRESS;

#define NULL  ((void*)0)
#define TRUE  1
#define FALSE 0

#define EFI_BLACK        0x00
#define EFI_BLUE         0x01
#define EFI_GREEN        0x02
#define EFI_CYAN         0x03
#define EFI_RED          0x04
#define EFI_MAGENTA      0x05
#define EFI_BROWN        0x06
#define EFI_LIGHTGRAY    0x07
#define EFI_YELLOW       0x0E
#define EFI_WHITE        0x0F
#define EFI_BGBLACK      0x00
#define EFI_BGBLUE       0x10

#define EFI_SUCCESS      0
#define EFI_BUFFER_TOO_SMALL ((EFI_STATUS)(1ULL << 63 | 5))
#define EFI_NOT_READY    ((EFI_STATUS)(1ULL << 63 | 6))
#define EFI_NOT_FOUND    ((EFI_STATUS)(1ULL << 63 | 14))
#define EFI_OUT_OF_RESOURCES ((EFI_STATUS)(1ULL << 63 | 9))
#define EFI_INVALID_PARAMETER ((EFI_STATUS)(1ULL << 63 | 2))

#define EfiReservedMemoryType       0
#define EfiLoaderCode               1
#define EfiLoaderData               2
#define EfiBootServicesCode         3
#define EfiBootServicesData         4
#define EfiRuntimeServicesCode      5
#define EfiRuntimeServicesData      6
#define EfiConventionalMemory       7
#define EfiUnusableMemory           8
#define EfiACPIReclaimMemory        9
#define EfiACPIMemoryNVS            10
#define EfiMemoryMappedIO           11
#define EfiMemoryMappedIOPortSpace  12
#define EfiPalCode                  13
#define EfiPersistentMemory         14

#define AllocateAnyPages            0
#define AllocateAddress             2

#define EFI_FILE_MODE_READ    0x0000000000000001ULL
#define EFI_FILE_MODE_WRITE   0x0000000000000002ULL
#define EFI_FILE_MODE_CREATE  0x8000000000000000ULL

#define EFI_FILE_READ_ONLY  0x0000000000000001ULL
#define EFI_FILE_DIRECTORY  0x0000000000000010ULL

#define EFI_LOADED_IMAGE_PROTOCOL_GUID \
    { 0x5B1B31A1, 0x9562, 0x11D2, { 0x8E, 0x3F, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B } }
#define EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID \
    { 0x964E5B22, 0x6459, 0x11D2, { 0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B } }
#define EFI_FILE_INFO_GUID \
    { 0x09576E92, 0x6D3F, 0x11D2, { 0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B } }
#define EFI_BLOCK_IO_PROTOCOL_GUID \
    { 0x964E5B21, 0x6459, 0x11D2, { 0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B } }

typedef struct {
    UINT32 Data1; UINT16 Data2; UINT16 Data3; UINT8 Data4[8];
} EFI_GUID;

typedef struct {
    UINT32 MediaId;
    BOOLEAN RemovableMedia;
    BOOLEAN MediaPresent;
    BOOLEAN LogicalPartition;
    BOOLEAN ReadOnly;
    BOOLEAN WriteCaching;
    UINT32 BlockSize;
    UINT32 IoAlign;
    UINT64 LastBlock;
} EFI_BLOCK_IO_MEDIA;

typedef struct EFI_BLOCK_IO_PROTOCOL {
    UINT64 Revision;
    EFI_BLOCK_IO_MEDIA* Media;
    EFI_STATUS (*Reset)(struct EFI_BLOCK_IO_PROTOCOL*, BOOLEAN);
    EFI_STATUS (*ReadBlocks)(struct EFI_BLOCK_IO_PROTOCOL*, UINT32, UINT64, UINTN, void*);
    EFI_STATUS (*WriteBlocks)(struct EFI_BLOCK_IO_PROTOCOL*, UINT32, UINT64, UINTN, void*);
    EFI_STATUS (*FlushBlocks)(struct EFI_BLOCK_IO_PROTOCOL*);
} EFI_BLOCK_IO_PROTOCOL;

typedef struct {
    UINT64 Signature; UINT32 Revision; UINT32 HeaderSize;
    UINT32 CRC32; UINT32 Reserved;
} EFI_TABLE_HEADER;

typedef struct {
    UINT32 Type;
    UINT32 Pad;
    EFI_PHYSICAL_ADDRESS PhysicalStart;
    EFI_PHYSICAL_ADDRESS VirtualStart;
    UINT64 NumberOfPages;
    UINT64 Attribute;
} EFI_MEMORY_DESCRIPTOR;

typedef struct {
    EFI_GUID VendorGuid;
    void* VendorTable;
} EFI_CONFIGURATION_TABLE;

typedef EFI_STATUS (*EFI_GET_MEMORY_MAP)(UINTN* MemoryMapSize,
                                         EFI_MEMORY_DESCRIPTOR* MemoryMap,
                                         UINTN* MapKey,
                                         UINTN* DescriptorSize,
                                         UINT32* DescriptorVersion);
typedef EFI_STATUS (*EFI_ALLOCATE_POOL)(UINT32 PoolType, UINTN Size,
                                        void** Buffer);
typedef EFI_STATUS (*EFI_FREE_POOL)(void* Buffer);
typedef EFI_STATUS (*EFI_ALLOCATE_PAGES)(UINTN Type, UINT32 MemoryType,
                                         UINTN Pages,
                                         EFI_PHYSICAL_ADDRESS* Memory);
typedef EFI_STATUS (*EFI_EXIT_BOOT_SERVICES)(EFI_HANDLE ImageHandle,
                                             UINTN MapKey);
typedef EFI_STATUS (*EFI_LOCATE_PROTOCOL)(EFI_GUID* Protocol,
                                          void* Registration,
                                          void** Interface);

typedef struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
    void*      Reset;
    EFI_STATUS (*OutputString)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL*, CHAR16*);
    void*      TestString;
    void*      QueryMode;
    void*      SetMode;
    EFI_STATUS (*SetAttribute)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL*, UINTN);
    EFI_STATUS (*ClearScreen)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL*);
} EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;

typedef struct {
    UINT16 ScanCode;
    CHAR16 UnicodeChar;
} EFI_INPUT_KEY;

typedef struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL {
    void*      Reset;
    EFI_STATUS (*ReadKeyStroke)(struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL*, EFI_INPUT_KEY*);
    void*      WaitForKey;
} EFI_SIMPLE_TEXT_INPUT_PROTOCOL;

typedef struct {
    UINT64 Size; UINT64 FileSize; UINT64 PhysicalSize;
    UINT8  CreateTime[16]; UINT8 LastAccessTime[16]; UINT8 ModificationTime[16];
    UINT64 Attribute;
    CHAR16 FileName[256];
} EFI_FILE_INFO;

typedef struct EFI_FILE_PROTOCOL {
    UINT64     Revision;
    EFI_STATUS (*Open)(struct EFI_FILE_PROTOCOL*, struct EFI_FILE_PROTOCOL**, CHAR16*, UINT64, UINT64);
    EFI_STATUS (*Close)(struct EFI_FILE_PROTOCOL*);
    EFI_STATUS (*Delete)(struct EFI_FILE_PROTOCOL*);
    EFI_STATUS (*Read)(struct EFI_FILE_PROTOCOL*, UINTN*, void*);
    EFI_STATUS (*Write)(struct EFI_FILE_PROTOCOL*, UINTN*, void*);
    void*      GetPosition;
    void*      SetPosition;
    EFI_STATUS (*GetInfo)(struct EFI_FILE_PROTOCOL*, EFI_GUID*, UINTN*, void*);
    EFI_STATUS (*SetInfo)(struct EFI_FILE_PROTOCOL*, EFI_GUID*, UINTN, void*);
    EFI_STATUS (*Flush)(struct EFI_FILE_PROTOCOL*);
} EFI_FILE_PROTOCOL;

typedef struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL {
    UINT64     Revision;
    EFI_STATUS (*OpenVolume)(struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL*, EFI_FILE_PROTOCOL**);
} EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;

typedef struct {
    UINT32 RedMask;
    UINT32 GreenMask;
    UINT32 BlueMask;
    UINT32 ReservedMask;
} EFI_PIXEL_BITMASK;

typedef struct {
    UINT32 Version;
    UINT32 HorizontalResolution;
    UINT32 VerticalResolution;
    UINT32 PixelFormat;
    EFI_PIXEL_BITMASK PixelInformation;
    UINT32 PixelsPerScanLine;
} EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;

typedef struct {
    UINT32 MaxMode;
    UINT32 Mode;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION* Info;
    UINTN SizeOfInfo;
    EFI_PHYSICAL_ADDRESS FrameBufferBase;
    UINTN FrameBufferSize;
} EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE;

typedef struct EFI_GRAPHICS_OUTPUT_PROTOCOL {
    void* QueryMode;
    void* SetMode;
    void* Blt;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE* Mode;
} EFI_GRAPHICS_OUTPUT_PROTOCOL;

#define EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID \
    { 0x9042A9DE, 0x23DC, 0x4A38, { 0x96, 0xFB, 0x7A, 0xDE, 0xD0, 0x80, 0x51, 0x6A } }

typedef struct {
    UINT32 Revision; EFI_HANDLE ParentHandle; void* SystemTable;
    EFI_HANDLE DeviceHandle; void* FilePath; void* Reserved;
    UINT32 LoadOptionsSize; void* LoadOptions; void* ImageBase;
    UINT64 ImageSize; UINT32 ImageCodeType; UINT32 ImageDataType; void* Unload;
} EFI_LOADED_IMAGE_PROTOCOL;

typedef struct {
    EFI_TABLE_HEADER Hdr;
    void* RaiseTPL; void* RestoreTPL;
    EFI_ALLOCATE_PAGES AllocatePages; void* FreePages; EFI_GET_MEMORY_MAP GetMemoryMap;
    EFI_ALLOCATE_POOL AllocatePool; EFI_FREE_POOL FreePool;
    void* CreateEvent; void* SetTimer; void* WaitForEvent;
    void* SignalEvent; void* CloseEvent; void* CheckEvent;
    void* InstallProtocolInterface; void* ReinstallProtocolInterface;
    void* UninstallProtocolInterface;
    EFI_STATUS (*HandleProtocol)(EFI_HANDLE, EFI_GUID*, void**);
    void* Reserved; void* RegisterProtocolNotify; void* LocateHandle;
    void* LocateDevicePath; void* InstallConfigurationTable;
    void* LoadImage; void* StartImage; void* Exit;
    void* UnloadImage; EFI_EXIT_BOOT_SERVICES ExitBootServices;
    void* GetNextMonotonicCount; void* Stall; void* SetWatchdogTimer;
    void* ConnectController; void* DisconnectController; void* OpenProtocol;
    void* CloseProtocol; void* OpenProtocolInformation; void* ProtocolsPerHandle;
    void* LocateHandleBuffer; EFI_LOCATE_PROTOCOL LocateProtocol;
} EFI_BOOT_SERVICES;

// Warning: runtime services are vibe-coded, subject to change (probably)

typedef void (*EFI_RESET_SYSTEM)(UINTN ResetType, EFI_STATUS ResetStatus, UINTN DataSize, void *ResetData);

typedef struct {
    char Hdr[24];
    void* _pad[10];
    EFI_RESET_SYSTEM ResetSystem;
} EFI_RUNTIME_SERVICES;

typedef struct {
    EFI_TABLE_HEADER                 Hdr;
    CHAR16*                          FirmwareVendor;
    UINT32                           FirmwareRevision;
    EFI_HANDLE                       ConsoleInHandle;
    EFI_SIMPLE_TEXT_INPUT_PROTOCOL*  ConIn;
    EFI_HANDLE                       ConsoleOutHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL* ConOut;
    EFI_HANDLE                       StandardErrorHandle;
    void*                            StdErr;
    // void*                            RuntimeServices;
    EFI_RUNTIME_SERVICES*            RuntimeServices;
    EFI_BOOT_SERVICES*               BootServices;
    UINTN                            NumberOfTableEntries;
    EFI_CONFIGURATION_TABLE*         ConfigurationTable;
} EFI_SYSTEM_TABLE;

#endif
