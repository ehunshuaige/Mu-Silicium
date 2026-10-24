/** @file
  Read-only RUYIESP FAT payload that the Android bootloader leaves in the
  boot image ramdisk.

  Ported from UniXNU-OnePlus-Ace5's GiuliaInitrdDxe (BSD-2-Clause-Patent). The
  structure is the same: find the ramdisk, publish it as a read-only Block I/O
  device, and let PartitionDxe/Fat mount the FAT volume UniXNU loads its
  configuration from.

  Two rules keep the scan from faulting, both learned on the device:

    1. Only scan memory the UEFI memory map describes. DDR that is not in the
       map is not mapped, and reading it is a translation fault (data abort).
    2. Only scan inside the DDR banks the device tree declares. The memory map
       can describe ranges past the end of a bank; reading those is a
       synchronous external abort.

  The banks below are the memory nodes of the phone's device tree (ruyi.dtb,
  the device_type = "memory" nodes), largest first. Banks smaller than a
  payload volume are left out.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/FdtLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Protocol/BlockIo.h>
#include <Protocol/DevicePath.h>

#define RAM_BLOCK_SIZE     512
#define MIN_BANK_BYTES     (32ULL * 1024 * 1024)

/*
 * BootShim stores the device tree pointer the bootloader passed in x0 at
 * FD_BASE - 0x10, inside the FD-reserved page of the memory map. Reading the
 * device tree gives the exact ramdisk range from /chosen instead of guessing.
 */
#define DTB_SLOT           (0x0A7100000ULL - 0x10)

/* The volume label the payload FAT carries (board.toml [payload] volume). */
STATIC CONST CHAR8  mVolumeLabel[11] = "RUYIESP    ";

typedef struct {
  UINT64    Base;
  UINT64    End;
} DDR_BANK;

/*
 * ruyi.dtb memory nodes. The bootloader puts the boot image in the low DDR, so
 * that bank is scanned first; the high banks follow in case it does not.
 */
STATIC CONST DDR_BANK  mScanBanks[] = {
  { 0x0A2580000ULL, 0x0D8000000ULL },          /* 858 MiB */
  { 0x0E34B0000ULL, 0x0F7C00000ULL },          /* 327 MiB */
  { 0x0FB800000ULL, 0x0FF100000ULL },          /* 57 MiB */
  { 0x082800000ULL, 0x084800000ULL },          /* 32 MiB */
  { 0x0880000000ULL, 0x08AF900000ULL },        /* 761 MiB */
  { 0x08B0000000ULL, 0x08B7F00000ULL },        /* 127 MiB */
  { 0x08B8100000ULL, 0x08B9F00000ULL },        /* 30 MiB */
  { 0x08C0000000ULL, 0x0980000000ULL },        /* 3 GiB */
  { 0x0980000000ULL, 0x0B00000000ULL },        /* 6 GiB */
};

#pragma pack(1)
typedef struct {
  VENDOR_DEVICE_PATH          Vendor;
  EFI_DEVICE_PATH_PROTOCOL    End;
} RAM_BLOCK_DEVICE_PATH;
#pragma pack()

STATIC RAM_BLOCK_DEVICE_PATH  mDevicePath = {
  {
    { HARDWARE_DEVICE_PATH, HW_VENDOR_DP, { (UINT8)sizeof (VENDOR_DEVICE_PATH), (UINT8)(sizeof (VENDOR_DEVICE_PATH) >> 8) } },
    { 0x8D5B2E31, 0x4C7A, 0x4F1B, { 0x9E, 0x02, 0x63, 0x11, 0xA4, 0xC8, 0x5D, 0x70 } }
  },
  { END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE, { sizeof (EFI_DEVICE_PATH_PROTOCOL), 0 } }
};

STATIC UINT8                    *mRamBase;
STATIC EFI_BLOCK_IO_MEDIA       mMedia;
STATIC EFI_BLOCK_IO_PROTOCOL    mBlockIo;

STATIC
VOID
Report (
  IN UINTN       Stage,
  IN UINT64      Start,
  IN UINT64      End,
  IN EFI_STATUS  Status
  )
{
  EFI_GUID  Guid = { 0x3f6d1c2a, 0x7b48, 0x4e19, { 0x85, 0x63, 0x2a, 0x91, 0xd4, 0x07, 0x6c, 0x5e } };
  CHAR16    Text[128];

  UnicodeSPrint (Text, sizeof (Text), L"initrd S%u %lx-%lx %r", (UINT32)Stage, Start, End, Status);
  DEBUG ((DEBUG_ERROR, "RUYIINITRD: %s\n", Text));
  if (gRT != NULL) {
    gRT->SetVariable (L"RuyiInitrdStatus", &Guid,
        EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS, StrSize (Text), Text);
  }
}

STATIC
EFI_STATUS
EFIAPI
RamBlockReset (
  IN EFI_BLOCK_IO_PROTOCOL  *This,
  IN BOOLEAN                ExtendedVerification
  )
{
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
RamBlockRead (
  IN  EFI_BLOCK_IO_PROTOCOL  *This,
  IN  UINT32                 MediaId,
  IN  EFI_LBA                Lba,
  IN  UINTN                  BufferSize,
  OUT VOID                   *Buffer
  )
{
  if (Buffer == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if (MediaId != mMedia.MediaId) {
    return EFI_MEDIA_CHANGED;
  }

  if ((BufferSize % RAM_BLOCK_SIZE) != 0) {
    return EFI_BAD_BUFFER_SIZE;
  }

  if ((Lba > mMedia.LastBlock) || ((BufferSize / RAM_BLOCK_SIZE) > (mMedia.LastBlock + 1 - Lba))) {
    return EFI_INVALID_PARAMETER;
  }

  CopyMem (Buffer, mRamBase + Lba * RAM_BLOCK_SIZE, BufferSize);
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
RamBlockWrite (
  IN EFI_BLOCK_IO_PROTOCOL  *This,
  IN UINT32                 MediaId,
  IN EFI_LBA                Lba,
  IN UINTN                  BufferSize,
  IN VOID                   *Buffer
  )
{
  return EFI_WRITE_PROTECTED;
}

STATIC
EFI_STATUS
EFIAPI
RamBlockFlush (
  IN EFI_BLOCK_IO_PROTOCOL  *This
  )
{
  return EFI_SUCCESS;
}

/* Publish a FAT32 volume labelled RUYIESP found inside [Start, End). */
STATIC
EFI_STATUS
ScanRamRange (
  IN UINT64  Start,
  IN UINT64  End
  )
{
  UINT64      At;
  UINT64      Bytes;
  UINT8      *Sector;
  EFI_HANDLE  Handle;
  EFI_STATUS  Status;

  if ((End <= Start) || ((End - Start) < RAM_BLOCK_SIZE)) {
    return EFI_NOT_FOUND;
  }

  for (At = ALIGN_VALUE (Start, (UINT64)RAM_BLOCK_SIZE); At + RAM_BLOCK_SIZE <= End; At += RAM_BLOCK_SIZE) {
    Sector = (UINT8 *)(UINTN)At;
    if ((Sector[510] != 0x55) || (Sector[511] != 0xAA)) {
      continue;
    }

    /* BPB: bytes per sector, FAT32 name, volume label. */
    if ((ReadUnaligned16 ((CONST UINT16 *)(Sector + 11)) != RAM_BLOCK_SIZE) ||
        (CompareMem (Sector + 82, "FAT32   ", 8) != 0) ||
        (CompareMem (Sector + 71, mVolumeLabel, 11) != 0))
    {
      continue;
    }

    Bytes = (UINT64)ReadUnaligned32 ((CONST UINT32 *)(Sector + 32)) * RAM_BLOCK_SIZE;
    if ((Bytes == 0) || (Bytes > (End - At))) {
      continue;
    }

    mRamBase            = Sector;
    mMedia.MediaId      = 1;
    mMedia.MediaPresent = TRUE;
    mMedia.ReadOnly     = TRUE;
    mMedia.BlockSize    = RAM_BLOCK_SIZE;
    mMedia.IoAlign      = 1;
    mMedia.LastBlock    = Bytes / RAM_BLOCK_SIZE - 1;

    mBlockIo.Revision    = EFI_BLOCK_IO_PROTOCOL_REVISION;
    mBlockIo.Media       = &mMedia;
    mBlockIo.Reset       = RamBlockReset;
    mBlockIo.ReadBlocks  = RamBlockRead;
    mBlockIo.WriteBlocks = RamBlockWrite;
    mBlockIo.FlushBlocks = RamBlockFlush;

    Handle = NULL;
    Status = gBS->InstallMultipleProtocolInterfaces (
                    &Handle,
                    &gEfiDevicePathProtocolGuid, &mDevicePath,
                    &gEfiBlockIoProtocolGuid,    &mBlockIo,
                    NULL);
    Report (10, At, At + Bytes, Status);
    return Status;
  }

  return EFI_NOT_FOUND;
}

STATIC
UINT64
ReadAddress (
  IN CONST VOID  *Property,
  IN INT32        Length
  )
{
  if (Length == 8) {
    return Fdt64ToCpu (ReadUnaligned64 ((CONST UINT64 *)Property));
  }
  return Fdt32ToCpu (ReadUnaligned32 ((CONST UINT32 *)Property));
}

/*
 * The exact answer when it exists: the bootloader patches /chosen with the
 * address of the ramdisk it loaded, and BootShim kept the device tree pointer.
 */
STATIC
EFI_STATUS
ScanFromDeviceTree (
  VOID
  )
{
  CONST VOID  *Fdt;
  CONST VOID  *StartProp;
  CONST VOID  *EndProp;
  INT32        Chosen;
  INT32        StartLength;
  INT32        EndLength;
  UINT64       Dtb;
  UINT64       Start;
  UINT64       End;

  Dtb = ReadUnaligned64 ((CONST UINT64 *)(UINTN)DTB_SLOT);
  if ((Dtb == 0) || ((Dtb & 3) != 0)) {
    DEBUG ((DEBUG_ERROR, "RUYIINTRD: no device tree at the shim slot (0x%lx)\n", Dtb));
    return EFI_NOT_FOUND;
  }

  Fdt = (CONST VOID *)(UINTN)Dtb;
  if (FdtCheckHeader (Fdt) != 0) {
    DEBUG ((DEBUG_ERROR, "RUYIINTRD: bad device tree header at 0x%lx\n", Dtb));
    return EFI_NOT_FOUND;
  }

  Chosen = FdtPathOffset (Fdt, "/chosen");
  if (Chosen < 0) {
    DEBUG ((DEBUG_ERROR, "RUYIINTRD: device tree 0x%lx has no /chosen\n", Dtb));
    return EFI_NOT_FOUND;
  }

  StartProp = FdtGetProp (Fdt, Chosen, "linux,initrd-start", &StartLength);
  EndProp   = FdtGetProp (Fdt, Chosen, "linux,initrd-end", &EndLength);
  if ((StartProp == NULL) || (EndProp == NULL) ||
      ((StartLength != 4) && (StartLength != 8)) ||
      ((EndLength != 4) && (EndLength != 8)))
  {
    DEBUG ((DEBUG_ERROR, "RUYIINTRD: no initrd range in /chosen of 0x%lx\n", Dtb));
    return EFI_NOT_FOUND;
  }

  Start = ReadAddress (StartProp, StartLength);
  End   = ReadAddress (EndProp, EndLength);
  DEBUG ((DEBUG_ERROR, "RUYIINTRD: /chosen initrd 0x%lx-0x%lx\n", Start, End));

  if ((End <= Start) || ((End - Start) < RAM_BLOCK_SIZE)) {
    return EFI_NOT_FOUND;
  }

  return ScanRamRange (Start, End);
}

/*
 * Scan every (device-tree DDR bank  x  UEFI memory-map descriptor) intersection.
 * Both conditions are needed: the map says the range is mapped, the bank says it
 * is DDR rather than device space.
 */STATIC
EFI_STATUS
ScanBanks (
  IN EFI_MEMORY_DESCRIPTOR  *Map,
  IN UINTN                   Size,
  IN UINTN                   DescriptorSize
  )
{
  UINTN                  At;
  UINTN                  Bank;
  EFI_MEMORY_DESCRIPTOR  *Entry;
  UINT64                 Start;
  UINT64                 End;
  EFI_STATUS             Status;

  for (Bank = 0; Bank < ARRAY_SIZE (mScanBanks); Bank++) {
    for (At = 0; At + DescriptorSize <= Size; At += DescriptorSize) {
      Entry = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)Map + At);
      if (Entry->NumberOfPages == 0) {
        continue;
      }

      /* Device space is never a payload. */
      if ((Entry->Type == EfiMemoryMappedIO) ||
          (Entry->Type == EfiMemoryMappedIOPortSpace) ||
          (Entry->Type == EfiPalCode) ||
          (Entry->Type == EfiUnacceptedMemoryType))
      {
        continue;
      }

      Start = MAX (Entry->PhysicalStart, mScanBanks[Bank].Base);
      End   = MIN (Entry->PhysicalStart + LShiftU64 (Entry->NumberOfPages, EFI_PAGE_SHIFT),
                   mScanBanks[Bank].End);
      if ((End <= Start) || ((End - Start) < MIN_BANK_BYTES)) {
        continue;
      }

      DEBUG ((DEBUG_ERROR, "RUYIINTRD: scan 0x%lx-0x%lx bank %u type %u\n",
              Start, End, (UINT32)Bank, (UINT32)Entry->Type));
      Status = ScanRamRange (Start, End);
      if (!EFI_ERROR (Status)) {
        return Status;
      }
    }
  }

  return EFI_NOT_FOUND;
}

EFI_STATUS
EFIAPI
RuyiInitrdDxeEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS             Status;
  EFI_MEMORY_DESCRIPTOR  *Map;
  UINTN                  Size;
  UINTN                  Key;
  UINTN                  DescriptorSize;
  UINT32                 Version;

  Report (1, 0, 0, EFI_NOT_READY);

  /* The exact range from the device tree, when the bootloader filled it in. */
  Status = ScanFromDeviceTree ();
  if (!EFI_ERROR (Status)) {
    return EFI_SUCCESS;
  }

  Size = 0;
  Status = gBS->GetMemoryMap (&Size, NULL, &Key, &DescriptorSize, &Version);
  if (Status != EFI_BUFFER_TOO_SMALL) {
    Report (2, 0, 0, Status);
    return EFI_SUCCESS;
  }

  Size += 16 * DescriptorSize;
  Map = AllocatePool (Size);
  if (Map == NULL) {
    Report (3, 0, 0, EFI_OUT_OF_RESOURCES);
    return EFI_SUCCESS;
  }

  Status = gBS->GetMemoryMap (&Size, Map, &Key, &DescriptorSize, &Version);
  if (EFI_ERROR (Status) || (DescriptorSize < sizeof (EFI_MEMORY_DESCRIPTOR))) {
    FreePool (Map);
    Report (4, 0, 0, EFI_DEVICE_ERROR);
    return EFI_SUCCESS;
  }

  Status = ScanBanks (Map, Size, DescriptorSize);
  FreePool (Map);

  if (EFI_ERROR (Status)) {
    Report (17, 0, 0, Status);
  }

  /*
   * A missing payload must not stop the firmware: UniXNU then shows its
   * built-in menu (actions only), as it does without a configuration.
   */
  return EFI_SUCCESS;
}
