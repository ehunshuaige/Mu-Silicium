/** @file
  Read-only RUYIESP FAT payload from the Android bootloader initrd.

  Ported from UniXNU-OnePlus-Ace5's GiuliaInitrdDxe (BSD-2-Clause-Patent). The
  structure is the same: take the device-tree pointer the Silicium entry args
  hand over, read /chosen/linux,initrd-start and -end, and publish that memory
  as a read-only Block I/O device so PartitionDxe/Fat can mount the FAT volume
  UniXNU loads its configuration from.

  Differences from the giulia driver, for the first bring-up on ruyi:
    - no payload size/CRC identity: any FAT32 volume labelled RUYIESP inside the
      accepted window is taken (the CRC check needs PayloadIdentity.h generated
      from the built payload, which this board does not generate yet);
    - no board hardware hooks (touch, hall, Wi-Fi, GPU) -- those belong to the
      board drivers, not here;
    - the accepted window is the DDR the bootloader can put the ramdisk in,
      judged from the device tree memory nodes, with the Kernel reservation of
      the UEFI memory map as the fallback scan.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Protocol/BlockIo.h>
#include <Protocol/DevicePath.h>

#define RAM_BLOCK_SIZE  512

/* The volume label the payload FAT carries (board.toml [payload] volume). */
STATIC CONST CHAR8  mVolumeLabel[11] = "RUYIESP    ";

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

    mRamBase          = Sector;
    mMedia.MediaId    = 1;
    mMedia.MediaPresent = TRUE;
    mMedia.ReadOnly   = TRUE;
    mMedia.BlockSize  = RAM_BLOCK_SIZE;
    mMedia.IoAlign    = 1;
    mMedia.LastBlock  = Bytes / RAM_BLOCK_SIZE - 1;

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

/*
 * Memory the firmware has not described in its memory map is not necessarily
 * mapped, so a blind scan of physical DDR faults (data abort, translation
 * fault). Walk the memory map instead and scan the descriptors: every byte a
 * descriptor covers is mapped.
 */
STATIC
EFI_STATUS
ScanMemoryMap (
  VOID
  )
{
  EFI_STATUS             Status;
  EFI_MEMORY_DESCRIPTOR  *Map;
  EFI_MEMORY_DESCRIPTOR  *Entry;
  UINTN                  Size;
  UINTN                  Key;
  UINTN                  DescriptorSize;
  UINTN                  At;
  UINT32                 Version;
  UINT64                 Start;
  UINT64                 End;
  UINTN                  Pass;

  Size = 0;
  Status = gBS->GetMemoryMap (&Size, NULL, &Key, &DescriptorSize, &Version);
  if (Status != EFI_BUFFER_TOO_SMALL) {
    return Status;
  }

  Size += 16 * DescriptorSize;
  Map = AllocatePool (Size);
  if (Map == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Status = gBS->GetMemoryMap (&Size, Map, &Key, &DescriptorSize, &Version);
  if (EFI_ERROR (Status) || (DescriptorSize < sizeof (EFI_MEMORY_DESCRIPTOR))) {
    FreePool (Map);
    return EFI_DEVICE_ERROR;
  }

  /*
   * Pass 0 looks at reserved memory only (where a bootloader leaves the boot
   * image), pass 1 at everything else. A large descriptor is more likely to be
   * the payload than a small one, but keep the order simple.
   */
  for (Pass = 0; Pass < 2; Pass++) {
    for (At = 0; At + DescriptorSize <= Size; At += DescriptorSize) {
      Entry = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)Map + At);
      if (Entry->NumberOfPages == 0) {
        continue;
      }

      if (Pass == 0) {
        if (Entry->Type != EfiReservedMemoryType) {
          continue;
        }
      } else {
        if (Entry->Type == EfiReservedMemoryType) {
          continue;
        }
        /* MMIO and the firmware itself are not candidates. */
        if ((Entry->Type == EfiMemoryMappedIO) ||
            (Entry->Type == EfiMemoryMappedIOPortSpace) ||
            (Entry->Type == EfiPalCode) ||
            (Entry->Type == EfiUnacceptedMemoryType))
        {
          continue;
        }
      }

      Start = Entry->PhysicalStart;
      End   = Start + LShiftU64 (Entry->NumberOfPages, EFI_PAGE_SHIFT);
      if ((End - Start) < (32ULL * 1024 * 1024)) {
        continue;
      }

      DEBUG ((DEBUG_ERROR, "RUYIINITRD: scan 0x%lx-0x%lx type %u\n", Start, End, (UINT32)Entry->Type));
      Status = ScanRamRange (Start, End);
      if (!EFI_ERROR (Status)) {
        FreePool (Map);
        return Status;
      }
    }
  }

  FreePool (Map);
  return EFI_NOT_FOUND;
}

EFI_STATUS
EFIAPI
RuyiInitrdDxeEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;

  Report (1, 0, 0, EFI_NOT_READY);

  Status = ScanMemoryMap ();
  if (EFI_ERROR (Status)) {
    Report (17, 0, 0, Status);
  }

  /*
   * A missing payload must not stop the firmware: UniXNU then shows its
   * built-in menu (actions only), as it does without a configuration.
   */
  return EFI_SUCCESS;
}
