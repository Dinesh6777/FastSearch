#ifndef NTFS_STRUCTS_H
#define NTFS_STRUCTS_H

#include "fs_common.h"

#pragma pack(push, 1)

// The NTFS Volume Boot Sector (Sector 0 of an NTFS volume)
typedef struct {
    unsigned char Jump[3];                 // Jump instruction to boot code
    unsigned char Oem[8];                  // OEM ID (should be "NTFS    ")
    unsigned short BytesPerSector;         // Bytes per sector (usually 512)
    unsigned char SectorsPerCluster;       // Sectors per cluster (usually 8)
    unsigned short ReservedSectors;        // Reserved sectors (always 0 on NTFS)
    unsigned char Padding1[3];
    unsigned short Unused1;
    unsigned char MediaDescriptor;         // Media descriptor (0xF8 for hard drives)
    unsigned short Padding2;
    unsigned short SectorsPerTrack;
    unsigned short NumberOfHeads;
    unsigned long HiddenSectors;
    unsigned long Unused2;
    unsigned long Unused3;
    long long TotalSectors;                // Total sectors on the partition
    long long MftStartLcn;                 // Starting Logical Cluster Number of the MFT ($MFT)
    long long Mft2StartLcn;                // Starting Logical Cluster Number of $MFTMirr (mirror copy)
    signed char ClustersPerFileRecordSegment; // Clusters per File Record Segment. If positive, represents clusters; if negative, FRS size is 2^(-value) bytes.
    unsigned char Padding3[3];
    unsigned long ClustersPerIndexBlock;   // Clusters per index block
    long long VolumeSerialNumber;          // Partition serial number
    unsigned long Checksum;
    unsigned char BootStrap[0x200 - 0x54]; // Rest of the 512-byte boot sector containing bootstrap code
} NTFS_BOOT_SECTOR;

// Helper to compute actual size in bytes of a File Record Segment (usually 1024 bytes)
static inline unsigned int Ntfs_FileRecordSize(const NTFS_BOOT_SECTOR* boot) {
    if (boot->ClustersPerFileRecordSegment >= 0) {
        return (unsigned int)boot->ClustersPerFileRecordSegment * boot->SectorsPerCluster * boot->BytesPerSector;
    } else {
        return 1U << (unsigned int)(-boot->ClustersPerFileRecordSegment);
    }
}

// Helper to compute cluster size in bytes (usually 4096 bytes)
static inline unsigned int Ntfs_ClusterSize(const NTFS_BOOT_SECTOR* boot) {
    return (unsigned int)boot->SectorsPerCluster * boot->BytesPerSector;
}

// The Multi-Sector Header (also known as Record Header)
typedef struct {
    unsigned long Magic;                   // Signature (e.g. "FILE", "INDX")
    unsigned short USAOffset;              // Offset to the Update Sequence Array (USA)
    unsigned short USACount;               // Number of elements in the USA array
} MULTI_SECTOR_HEADER;

// Verifies the sector integrity and restores the original end-of-sector bytes that were replaced by USA.
static inline bool Ntfs_Unfixup(MULTI_SECTOR_HEADER* header, size_t max_size) {
    if (header->USAOffset + header->USACount * sizeof(unsigned short) > max_size) return false;
    
    unsigned short* usa = (unsigned short*)((unsigned char*)header + header->USAOffset);
    unsigned short const usa0 = usa[0]; // The safety signature written at the end of each sector
    
    for (unsigned short i = 1; i < header->USACount; i++) {
        const size_t offset = i * 512 - sizeof(unsigned short);
        if (offset + sizeof(unsigned short) <= max_size) {
            unsigned short* const sector_end = (unsigned short*)((unsigned char*)header + offset);
            // If the safety signature matches, restore the original 2 bytes from the USA
            if (*sector_end == usa0) {
                *sector_end = usa[i];
            } else {
                return false; // USA safety mismatch: corrupted sector
            }
        } else {
            break;
        }
    }
    return true;
}

// Enumeration of all NTFS Attribute types inside a File Record
typedef enum {
    AttributeStandardInformation = 0x10,   // Timestamps, attributes, security
    AttributeAttributeList = 0x20,         // List of attributes
    AttributeFileName = 0x30,              // File name, parent folder ref
    AttributeObjectId = 0x40,              // GUID
    AttributeSecurityDescriptor = 0x50,    // Windows ACL/security info
    AttributeVolumeName = 0x60,            // Volume name
    AttributeVolumeInformation = 0x70,     // Volume properties
    AttributeData = 0x80,                  // File data stream
    AttributeIndexRoot = 0x90,             // Directory indexes (root)
    AttributeIndexAllocation = 0xA0,       // Directory indexes (subnodes)
    AttributeBitmap = 0xB0,                // Allocation map
    AttributeReparsePoint = 0xC0,          // Symlinks, junctions, mount points
    AttributeEAInformation = 0xD0,
    AttributeEA = 0xE0,
    AttributePropertySet = 0xF0,
    AttributeLoggedUtilityStream = 0x100,
    AttributeEnd = 0xFFFFFFFF
} AttributeTypeCode;

// Represents the header of an Attribute Record inside a FRS.
typedef struct {
    unsigned long Type;                    // Attribute type code (AttributeTypeCode)
    unsigned long Length;                  // Length of this attribute record
    unsigned char IsNonResident;           // 0 = Resident, 1 = Non-Resident
    unsigned char NameLength;              // Name length in characters
    unsigned short NameOffset;             // Offset to name characters
    unsigned short Flags;                  // Flags: compressed, encrypted, sparse
    unsigned short Instance;               // Attribute unique identifier

    union {
        // Layout if attribute value is stored inline inside the record
        struct {
            unsigned long ValueLength;      // Length of the value in bytes
            unsigned short ValueOffset;     // Offset from start of header to value
            unsigned short Flags;           // Resident specific flags
        } Resident;

        // Layout if attribute value is non-resident (external cluster runs)
        struct {
            long long LowestVCN;            // Starting Virtual Cluster Number
            long long HighestVCN;           // Ending VCN
            unsigned short MappingPairsOffset; // Offset from start of header to mapping pairs
            unsigned char CompressionUnit;  // Compression unit
            unsigned char Reserved[5];
            long long AllocatedSize;        // Allocated size on disk (bytes)
            long long DataSize;             // Actual size of data in bytes
            long long InitializedSize;      // Initialized data size
            long long CompressedSize;       // Compressed size
        } NonResident;
    } u;
} ATTRIBUTE_RECORD_HEADER;

static inline void* Ntfs_GetAttributeValue(ATTRIBUTE_RECORD_HEADER* attr) {
    return (void*)((unsigned char*)attr + attr->u.Resident.ValueOffset);
}

static inline const void* Ntfs_GetAttributeValueConst(const ATTRIBUTE_RECORD_HEADER* attr) {
    return (const void*)((const unsigned char*)attr + attr->u.Resident.ValueOffset);
}

static inline ATTRIBUTE_RECORD_HEADER* Ntfs_NextAttribute(ATTRIBUTE_RECORD_HEADER* attr) {
    return (ATTRIBUTE_RECORD_HEADER*)((unsigned char*)attr + attr->Length);
}

static inline const ATTRIBUTE_RECORD_HEADER* Ntfs_NextAttributeConst(const ATTRIBUTE_RECORD_HEADER* attr) {
    return (const ATTRIBUTE_RECORD_HEADER*)((const unsigned char*)attr + attr->Length);
}

static inline wchar_t* Ntfs_GetAttributeName(ATTRIBUTE_RECORD_HEADER* attr) {
    return (wchar_t*)((unsigned char*)attr + attr->NameOffset);
}

static inline const wchar_t* Ntfs_GetAttributeNameConst(const ATTRIBUTE_RECORD_HEADER* attr) {
    return (const wchar_t*)((const unsigned char*)attr + attr->NameOffset);
}

typedef enum {
    FRH_IN_USE = 0x0001,                   // The record is currently allocated
    FRH_DIRECTORY = 0x0002                 // The record represents a folder/directory
} FILE_RECORD_HEADER_FLAGS;

// The File Record Segment (FRS) header
typedef struct {
    MULTI_SECTOR_HEADER MultiSectorHeader; // Safety unfixup header ("FILE")
    unsigned long long LogFileSequenceNumber; // Transaction sequence number
    unsigned short SequenceNumber;         // FRS reuse counter
    unsigned short LinkCount;              // Hard link count
    unsigned short FirstAttributeOffset;   // Offset to the first attribute record
    unsigned short Flags;                  // Flags (FILE_RECORD_HEADER_FLAGS)
    unsigned long BytesInUse;              // Total bytes used in this record
    unsigned long BytesAllocated;          // Maximum physical bytes allocated on disk (usually 1024)
    unsigned long long BaseFileRecordSegment; // Reference to the base FRS
    unsigned short NextAttributeNumber;    // ID of next attribute to allocate
    unsigned short SegmentNumberUpper_or_USA_or_UnknownReserved;
    unsigned long SegmentNumberLower;      // Lower 32-bits of FRS's own index in the MFT
} FILE_RECORD_SEGMENT_HEADER;

static inline ATTRIBUTE_RECORD_HEADER* Ntfs_FirstAttribute(FILE_RECORD_SEGMENT_HEADER* frs) {
    return (ATTRIBUTE_RECORD_HEADER*)((unsigned char*)frs + frs->FirstAttributeOffset);
}

static inline const ATTRIBUTE_RECORD_HEADER* Ntfs_FirstAttributeConst(const FILE_RECORD_SEGMENT_HEADER* frs) {
    return (const ATTRIBUTE_RECORD_HEADER*)((const unsigned char*)frs + frs->FirstAttributeOffset);
}

static inline void* Ntfs_EndRecord(FILE_RECORD_SEGMENT_HEADER* frs, size_t max_buffer_size) {
    return (void*)((unsigned char*)frs + (max_buffer_size < frs->BytesInUse ? max_buffer_size : frs->BytesInUse));
}

static inline const void* Ntfs_EndRecordConst(const FILE_RECORD_SEGMENT_HEADER* frs, size_t max_buffer_size) {
    return (const void*)((const unsigned char*)frs + (max_buffer_size < frs->BytesInUse ? max_buffer_size : frs->BytesInUse));
}

// Helper to get FRS number from FRS reference (low 48 bits)
static inline unsigned long long Ntfs_GetRecordIndex(const FILE_RECORD_SEGMENT_HEADER* frs) {
    return frs->SegmentNumberLower | ((unsigned long long)(frs->SegmentNumberUpper_or_USA_or_UnknownReserved & 0xFFFF) << 32);
}

// The `$FILE_NAME` Attribute value payload (Attribute type 0x30)
typedef struct {
    unsigned long long ParentDirectory;    // Low 48 bits is parent's FRS index, upper 16 bits is sequence number.
    long long CreationTime;                // Timestamp
    long long LastModificationTime;        // Date Modified
    long long LastChangeTime;              // Date last updated
    long long LastAccessTime;              // Date last accessed
    long long AllocatedLength;             // Size on disk in bytes
    long long FileSize;                    // Actual data length in bytes (logical size)
    unsigned long FileAttributes;          // Win32 file attributes
    unsigned short PackedEaSize;
    unsigned short Reserved;
    unsigned char FileNameLength;          // Character count of the file name
    unsigned char Flags;                   // Namespace flag
    WCHAR FileName[1];                     // Dynamic array containing name
} FILENAME_INFORMATION;

// The `$STANDARD_INFORMATION` Attribute value payload (Attribute type 0x10)
typedef struct {
    long long CreationTime;                // Timestamp created
    long long LastModificationTime;        // Timestamp modified
    long long LastChangeTime;              // Timestamp of MFT change
    long long LastAccessTime;              // Timestamp of last read
    unsigned long FileAttributes;          // Win32 attributes
} STANDARD_INFORMATION;

#pragma pack(pop)

#endif // NTFS_STRUCTS_H
