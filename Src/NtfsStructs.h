#pragma once
#include <windows.h>

namespace Ntfs {

#pragma pack(push, 1)

    // The NTFS Volume Boot Sector (Sector 0 of an NTFS volume)
    // This structure contains the BIOS Parameter Block (BPB) and primary pointers to the MFT location.
    struct NTFS_BOOT_SECTOR {
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
        long long MftStartLcn;                 // Starting Logical Cluster Number (LCN) of the MFT ($MFT)
        long long Mft2StartLcn;                // Starting Logical Cluster Number of $MFTMirr (mirror copy)
        signed char ClustersPerFileRecordSegment; // Clusters per File Record Segment. If positive, represents clusters; if negative, FRS size is 2^(-value) bytes.
        unsigned char Padding3[3];
        unsigned long ClustersPerIndexBlock;   // Clusters per index block
        long long VolumeSerialNumber;          // Partition serial number
        unsigned long Checksum;
        unsigned char BootStrap[0x200 - 0x54]; // Rest of the 512-byte boot sector containing bootstrap code

        // Utility to compute actual size in bytes of a File Record Segment (usually 1024 bytes)
        unsigned int file_record_size() const {
            if (ClustersPerFileRecordSegment >= 0) {
                return (unsigned int)ClustersPerFileRecordSegment * SectorsPerCluster * BytesPerSector;
            } else {
                return 1U << static_cast<int>(-ClustersPerFileRecordSegment);
            }
        }

        // Utility to compute cluster size in bytes (usually 4096 bytes / 4KB)
        unsigned int cluster_size() const {
            return (unsigned int)SectorsPerCluster * BytesPerSector;
        }
    };

    // The Multi-Sector Header (also known as Record Header)
    // Every structural block in NTFS (FRS, Index, etc.) starts with this header.
    // It implements the "Update Sequence Array" (USA) sector safety mechanism:
    // When a block is written to disk, the last 2 bytes of every 512-byte sector are moved
    // into the USA array (stored at USAOffset), and a 2-byte signature (usa[0]) is written in their place.
    // This allows detection of partial or corrupted sector writes.
    struct MULTI_SECTOR_HEADER {
        unsigned long Magic;                   // Signature (e.g. "FILE", "INDX")
        unsigned short USAOffset;              // Offset to the Update Sequence Array (USA)
        unsigned short USACount;               // Number of elements in the USA array (1 signature + 1 per sector)

        // Verifies the sector integrity and restores the original end-of-sector bytes that were replaced by USA.
        // This process must be run on raw disk buffers before attempting to parse internal offsets.
        bool unfixup(size_t max_size) {
            if (USAOffset + USACount * sizeof(unsigned short) > max_size) return false;
            
            unsigned short* usa = reinterpret_cast<unsigned short*>(reinterpret_cast<unsigned char*>(this) + USAOffset);
            unsigned short const usa0 = usa[0]; // The safety signature written at the end of each sector
            
            for (unsigned short i = 1; i < USACount; i++) {
                const size_t offset = i * 512 - sizeof(unsigned short);
                if (offset + sizeof(unsigned short) <= max_size) {
                    unsigned short* const sector_end = reinterpret_cast<unsigned short*>(reinterpret_cast<unsigned char*>(this) + offset);
                    // If the safety signature matches, restore the original 2 bytes from the USA
                    if (*sector_end == usa0) {
                        *sector_end = usa[i];
                    } else {
                        // USA safety mismatch: Sector may have been partially written or corrupted
                        return false;
                    }
                } else {
                    break;
                }
            }
            return true;
        }
    };

    // Enumeration of all NTFS Attribute types inside a File Record
    enum AttributeTypeCode {
        AttributeStandardInformation = 0x10,   // Timestamps, attributes, security
        AttributeAttributeList = 0x20,         // List of attributes that span across multiple records
        AttributeFileName = 0x30,              // File name, length, times, sizes, parent folder ref
        AttributeObjectId = 0x40,              // GUID for object tracking
        AttributeSecurityDescriptor = 0x50,    // Windows ACL/security info
        AttributeVolumeName = 0x60,            // Volume name attribute
        AttributeVolumeInformation = 0x70,     // Volume properties
        AttributeData = 0x80,                  // The actual file contents (data stream)
        AttributeIndexRoot = 0x90,             // Directory indexes (root of tree)
        AttributeIndexAllocation = 0xA0,       // Directory indexes (sub-nodes of tree)
        AttributeBitmap = 0xB0,                // Allocation map for index directories
        AttributeReparsePoint = 0xC0,          // Symbolic links, junction points, mount points
        AttributeEAInformation = 0xD0,
        AttributeEA = 0xE0,
        AttributePropertySet = 0xF0,
        AttributeLoggedUtilityStream = 0x100,
        AttributeEnd = 0xFFFFFFFF,             // End of attributes marker (0xFFFFFFFF)
    };

    // Represents the header of an Attribute Record inside a FRS.
    // Attributes can be Resident (stored entirely inside the FRS) or
    // Non-Resident (stored in cluster runs outside the FRS).
    struct ATTRIBUTE_RECORD_HEADER {
        AttributeTypeCode Type;                // Attribute type code
        unsigned long Length;                  // Length of this attribute record (including header + value)
        unsigned char IsNonResident;           // 0 = Resident, 1 = Non-Resident
        unsigned char NameLength;              // Name length in characters (if named attribute, like alternate streams)
        unsigned short NameOffset;             // Offset to name characters
        unsigned short Flags;                  // Flags: 0x0001 = Compressed, 0x4000 = Encrypted, 0x8000 = Sparse
        unsigned short Instance;               // Attribute unique identifier in this record

        union {
            // Layout if attribute value is stored inline inside the record
            struct {
                unsigned long ValueLength;      // Length of the value in bytes
                unsigned short ValueOffset;     // Offset from the start of this header to the value
                unsigned short Flags;           // Resident specific flags
            } Resident;

            // Layout if attribute value is non-resident (stored in external cluster runs)
            struct {
                long long LowestVCN;            // Starting Virtual Cluster Number (VCN) for this segment
                long long HighestVCN;           // Ending VCN for this segment
                unsigned short MappingPairsOffset; // Offset from start of header to mapping pairs (runlist)
                unsigned char CompressionUnit;  // Compression unit (log2 of sectors/compression unit)
                unsigned char Reserved[5];
                long long AllocatedSize;        // Allocated size on disk (bytes)
                long long DataSize;             // Actual size of data in bytes
                long long InitializedSize;      // Initialized data size (usually same as data size)
                long long CompressedSize;       // Compressed size (only if compressed/sparse)
            } NonResident;
        };

        inline void* GetValue() {
            return reinterpret_cast<void*>(reinterpret_cast<char*>(this) + Resident.ValueOffset);
        }
        inline const void* GetValue() const {
            return reinterpret_cast<const void*>(reinterpret_cast<const char*>(this) + Resident.ValueOffset);
        }

        ATTRIBUTE_RECORD_HEADER* next() {
            return reinterpret_cast<ATTRIBUTE_RECORD_HEADER*>(reinterpret_cast<unsigned char*>(this) + Length);
        }
        const ATTRIBUTE_RECORD_HEADER* next() const {
            return reinterpret_cast<const ATTRIBUTE_RECORD_HEADER*>(reinterpret_cast<const unsigned char*>(this) + Length);
        }
        wchar_t* name() {
            return reinterpret_cast<wchar_t*>(reinterpret_cast<unsigned char*>(this) + NameOffset);
        }
        const wchar_t* name() const {
            return reinterpret_cast<const wchar_t*>(reinterpret_cast<const unsigned char*>(this) + NameOffset);
        }
    };

    enum FILE_RECORD_HEADER_FLAGS {
        FRH_IN_USE = 0x0001,                   // The record is currently allocated (not deleted)
        FRH_DIRECTORY = 0x0002,                // The record represents a folder/directory
    };

    // The File Record Segment (FRS) header
    // Each file or directory in NTFS has at least one FRS in the Master File Table.
    struct FILE_RECORD_SEGMENT_HEADER {
        MULTI_SECTOR_HEADER MultiSectorHeader; // Safety unfixup header ("FILE")
        unsigned long long LogFileSequenceNumber; // Transaction sequence number for journal safety
        unsigned short SequenceNumber;         // FRS reuse counter (incremented when record is deleted & reassigned)
        unsigned short LinkCount;              // Hard link count
        unsigned short FirstAttributeOffset;   // Offset to the first attribute record
        unsigned short Flags;                  // Flags (FILE_RECORD_HEADER_FLAGS: allocated, directory)
        unsigned long BytesInUse;              // Total bytes used in this record (header + attributes + end marker)
        unsigned long BytesAllocated;          // Maximum physical bytes allocated on disk for this FRS (usually 1024)
        unsigned long long BaseFileRecordSegment; // Reference to the base FRS (0 if this is the primary FRS)
        unsigned short NextAttributeNumber;    // ID of next attribute to allocate
        unsigned short SegmentNumberUpper_or_USA_or_UnknownReserved;
        unsigned long SegmentNumberLower;      // Lower 32-bits of this FRS's own index in the MFT

        ATTRIBUTE_RECORD_HEADER* begin() {
            return reinterpret_cast<ATTRIBUTE_RECORD_HEADER*>(reinterpret_cast<unsigned char*>(this) + FirstAttributeOffset);
        }
        const ATTRIBUTE_RECORD_HEADER* begin() const {
            return reinterpret_cast<const ATTRIBUTE_RECORD_HEADER*>(reinterpret_cast<const unsigned char*>(this) + FirstAttributeOffset);
        }
        void* end(size_t const max_buffer_size = ~size_t()) {
            return reinterpret_cast<unsigned char*>(this) + (max_buffer_size < BytesInUse ? max_buffer_size : BytesInUse);
        }
        const void* end(size_t const max_buffer_size = ~size_t()) const {
            return reinterpret_cast<const unsigned char*>(this) + (max_buffer_size < BytesInUse ? max_buffer_size : BytesInUse);
        }

        // Helper to get FRS number from FRS reference (low 48 bits is the FRS record index)
        unsigned long long GetRecordIndex() const {
            return SegmentNumberLower | ((unsigned long long)(SegmentNumberUpper_or_USA_or_UnknownReserved & 0xFFFF) << 32);
        }
    };

    // The `$FILE_NAME` Attribute value payload (Attribute type 0x30)
    // Stored inside the FRS. This contains the parent folder record reference, sizes, and file name.
    struct FILENAME_INFORMATION {
        unsigned long long ParentDirectory;    // Low 48 bits represents parent's FRS index, upper 16 bits is sequence number.
        long long CreationTime;                // Timestamp (100-nanosecond intervals since Jan 1, 1601)
        long long LastModificationTime;        // Date Modified
        long long LastChangeTime;              // Date last updated
        long long LastAccessTime;              // Date last accessed
        long long AllocatedLength;             // Size on disk in bytes
        long long FileSize;                    // Actual data length in bytes (logical size)
        unsigned long FileAttributes;          // Win32 file attributes (hidden, system, archive, etc.)
        unsigned short PackedEaSize;
        unsigned short Reserved;
        unsigned char FileNameLength;          // Character count of the file name
        unsigned char Flags;                   // Namespace flag (0 = POSIX, 1 = Win32, 2 = DOS, 3 = Win32 & DOS)
        WCHAR FileName[1];                     // Dynamic array containing the actual name
    };

    // The `$STANDARD_INFORMATION` Attribute value payload (Attribute type 0x10)
    // Contains the primary metadata such as timestamps and attributes.
    struct STANDARD_INFORMATION {
        long long CreationTime;                // Timestamp created
        long long LastModificationTime;        // Timestamp modified
        long long LastChangeTime;              // Timestamp of MFT change
        long long LastAccessTime;              // Timestamp of last read
        unsigned long FileAttributes;          // Win32 attributes
    };

#pragma pack(pop)

} // namespace Ntfs
