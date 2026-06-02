#ifndef NTFS_INDEX_H
#define NTFS_INDEX_H

#include "fs_common.h"

// In-memory representation of a single file or directory record
typedef struct FileRecord {
    unsigned long long Size;               // File logical size in bytes
    unsigned long long DateModified;       // File modification date
    unsigned int Frs;                      // Its own FRS record index
    unsigned int ParentFrs;                // Parent folder record reference FRS number
    unsigned int Attributes;               // Win32 file attribute flags
    wchar_t Name[];                        // Flexible array member for inline name allocation
} FileRecord;

typedef struct ArenaPage {
    struct ArenaPage* Next;
    size_t Used;
    unsigned char Data[];
} ArenaPage;

typedef struct {
    unsigned int Frs;
    FileRecord* Record;
} HardLinkEntry;

#define FRS_PAGE_SHIFT 12               // 4096 entries per page
#define FRS_PAGE_SIZE (1 << FRS_PAGE_SHIFT)
#define FRS_PAGE_MASK (FRS_PAGE_SIZE - 1)

// The NTFS index structure managing the in-memory records
typedef struct {
    wchar_t driveLetter;
    FileRecord*** recordsByFrsPages;       // Paged sparse lookup table (2-level page table)
    size_t recordsCount;                   // Total allocated size of the sparse array
    
    FileRecord** activeRecords;            // Contiguous pre-sorted array for ultra-fast scanning
    size_t activeCount;                    // Number of elements currently in the active array
    size_t activeCapacity;                 // Maximum capacity of the active array
    
    ArenaPage* arenaPages;                 // Page-pool allocator pages list
    HardLinkEntry* hardLinks;              // Dynamic list of rare hard links
    size_t hardLinksCount;
    size_t hardLinksCapacity;

    unsigned int totalFiles;
    unsigned int totalFolders;
    bool isIndexed;
    
    SRWLOCK lock;                          // High-concurrency Slim Reader-Writer lock
} NtfsIndex;

// Fast inline O(1) paged record lookup
static inline FileRecord* NtfsIndex_GetRecord(const NtfsIndex* index, unsigned int frs) {
    if (frs >= index->recordsCount || !index->recordsByFrsPages) return NULL;
    unsigned int pageIdx = frs >> FRS_PAGE_SHIFT;
    FileRecord** page = index->recordsByFrsPages[pageIdx];
    if (!page) return NULL;
    return page[frs & FRS_PAGE_MASK];
}

static inline void NtfsIndex_SetRecord(NtfsIndex* index, unsigned int frs, FileRecord* rec) {
    if (frs >= index->recordsCount || !index->recordsByFrsPages) return;
    unsigned int pageIdx = frs >> FRS_PAGE_SHIFT;
    if (!index->recordsByFrsPages[pageIdx]) {
        index->recordsByFrsPages[pageIdx] = (FileRecord**)calloc(FRS_PAGE_SIZE, sizeof(FileRecord*));
    }
    index->recordsByFrsPages[pageIdx][frs & FRS_PAGE_MASK] = rec;
}

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
