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

/*
 * Where the Android bootloader may leave the initrd on this SoC: the DDR that
 * the UEFI memory map hands over as reserved Kernel RAM (see MemoryMapLib.c,
 * "Kernel" 0xA8000000 + 0x10000000). The device tree /chosen properties win
 * when they are present and inside this window.
 */
#define SCAN_START  0xA2580000ULL   /* Mix Flip DDR: the 858 MiB block below TZApps */
#define SCAN_END    0xD8000000ULL

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

EFI_STATUS
EFIAPI
RuyiInitrdDxeEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;

  Report (1, SCAN_START, SCAN_END, EFI_NOT_READY);

  /*
   * Scan the DDR the bootloader can put the boot image in for the FAT32 volume
   * labelled RUYIESP (board.toml [payload] volume). Upstream Mu-Silicium does
   * not pass the initrd address through to DXE (uefi-sheng adds an entry-args
   * HOB for that), so the volume is found by its boot sector instead.
   */
  Status = ScanRamRange (SCAN_START, SCAN_END);
  if (EFI_ERROR (Status)) {
    Report (17, SCAN_START, SCAN_END, Status);
  }

  /*
   * A missing payload must not stop the firmware: UniXNU then shows its
   * built-in menu (actions only), as it does without a configuration.
   */
  return EFI_SUCCESS;
}
