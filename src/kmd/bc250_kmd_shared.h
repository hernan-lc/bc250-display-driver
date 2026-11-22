// SPDX-License-Identifier: MIT
// Data shared between BC250Kmd.sys and its user-mode driver (allocation private data, adapter info).
#pragma once

typedef unsigned __int32 bckmd_u32;
typedef unsigned __int64 bckmd_u64;

#define BC250_KMD_ALLOC_MAGIC  0x4B303532u   // '250K'

// pPrivateDriverData of DXGK_ALLOCATIONINFO (created by the UMD or by GetStandardAllocationDriverData)
typedef struct BC250_KMD_ALLOC_DESC {
    bckmd_u32 Magic;
    bckmd_u32 Flags;          // BC250_KMD_ALLOC_*
    bckmd_u64 Size;           // bytes
    bckmd_u32 Width, Height, Pitch;
    bckmd_u32 Format;         // D3DDDIFORMAT
    bckmd_u32 VidPnSourceId;  // primaries only
    bckmd_u32 RefreshNum, RefreshDen;
    bckmd_u32 Reserved;
} BC250_KMD_ALLOC_DESC;

#define BC250_KMD_ALLOC_PRIMARY    0x1u
#define BC250_KMD_ALLOC_SHADOW     0x2u
#define BC250_KMD_ALLOC_STAGING    0x4u   // CPU-visible system memory (aperture)
#define BC250_KMD_ALLOC_CPUVISIBLE 0x8u

// DXGKQAITYPE_UMDRIVERPRIVATE
typedef struct BC250_KMD_ADAPTER_INFO {
    bckmd_u32 Magic;
    bckmd_u32 Version;
    bckmd_u32 DeviceId;
    bckmd_u32 Reserved;
    bckmd_u64 VramSegmentMc, VramSegmentSize;
    bckmd_u64 ApertureBase, ApertureSize;
    bckmd_u64 ScanoutMc;
    bckmd_u32 ScanoutPitch;
    bckmd_u32 Engines;        // bit0 = SDMA0 (node 0, copy/fill), bit1 = CP graphics ring (node 1, PM4)
} BC250_KMD_ADAPTER_INFO;

// Segment ids reported to dxgkrnl (1-based)
#define BC250_KMD_SEGMENT_VRAM     1u
#define BC250_KMD_SEGMENT_APERTURE 2u
