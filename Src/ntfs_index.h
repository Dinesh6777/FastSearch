#ifndef NTFS_INDEX_H
#define NTFS_INDEX_H

#include "fs_common.h"

// In-memory representation of a single file or directory record
typedef struct FileRecord {
    wchar_t* Name;                         // Contiguous string pointer
    unsigned int Frs;                      // Its own FRS record index
    unsigned int ParentFrs;                // Parent folder record reference FRS number
    unsigned long long Size;               // File logical size in bytes
    unsigned long long SizeOnDisk;         // File size on disk in bytes
    unsigned long long DateCreated;
    unsigned long long DateModified;
    unsigned long long DateAccessed;
    unsigned int Attributes;               // Win32 file attribute flags
    bool IsDirectory;
    struct FileRecord* Next;               // Pointer to the next hard link record
} FileRecord;

// The NTFS index structure managing the in-memory records
typedef struct {
    wchar_t driveLetter;
    FileRecord** recordsByFrs;             // Sparse lookup array (index = FRS number)
    size_t recordsCount;                   // Total allocated size of the sparse array
    
    FileRecord** activeRecords;            // Contiguous pre-sorted array for ultra-fast scanning
    size_t activeCount;                    // Number of elements currently in the active array
    size_t activeCapacity;                 // Maximum capacity of the active array
    
    unsigned int totalFiles;
    unsigned int totalFolders;
    bool isIndexed;
    
    SRWLOCK lock;                          // High-concurrency Slim Reader-Writer lock
} NtfsIndex;

// Index lifecycle and concurrency management
NtfsIndex* NtfsIndex_Create(wchar_t driveLetter);
void NtfsIndex_Destroy(NtfsIndex* index);

void NtfsIndex_LockShared(NtfsIndex* index);
void NtfsIndex_UnlockShared(NtfsIndex* index);
void NtfsIndex_LockExclusive(NtfsIndex* index);
void NtfsIndex_UnlockExclusive(NtfsIndex* index);

// Index building and updating
void NtfsIndex_InitializeSize(NtfsIndex* index, size_t maxFrsCount);
void NtfsIndex_ProcessMftChunk(NtfsIndex* index, unsigned char* buffer, size_t bytesRead, unsigned int startFrs, unsigned int recordSize);
void NtfsIndex_FinalizeIndex(NtfsIndex* index);

// Zero-allocation canonical path resolution
size_t NtfsIndex_ResolveFullPathToBuf(const NtfsIndex* index, const FileRecord* item, wchar_t* outBuf, size_t maxChars);
size_t NtfsIndex_ResolveFullPathToBufByFrs(const NtfsIndex* index, unsigned int recordIndex, wchar_t* outBuf, size_t maxChars);
size_t NtfsIndex_ResolveFullPathFromParent(const NtfsIndex* index, unsigned int parentFrs, const wchar_t* name, wchar_t* outBuf, size_t maxChars);

// Incremental changes (USN updates)
void NtfsIndex_AddOrUpdateRecord(NtfsIndex* index, unsigned int recordIndex, const wchar_t* name, unsigned int parentFrs, 
                                 unsigned long long size, unsigned long long sizeOnDisk, 
                                 unsigned long long dateCreated, unsigned long long dateModified, unsigned long long dateAccessed, 
                                 unsigned int attributes, bool isDirectory);
void NtfsIndex_DeleteRecord(NtfsIndex* index, unsigned int recordIndex);

#endif // NTFS_INDEX_H
