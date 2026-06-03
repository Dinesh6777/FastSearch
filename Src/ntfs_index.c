#include "ntfs_index.h"

#define ARENA_PAGE_SIZE (4 * 1024 * 1024) // 4MB pages

static FileRecord* NtfsIndex_ArenaAlloc(NtfsIndex* index, size_t size) {
    // Align size to 8-byte boundary
    size = (size + 7) & ~7;
    
    ArenaPage* page = index->arenaPages;
    if (!page || page->Used + size > ARENA_PAGE_SIZE) {
        size_t allocSize = sizeof(ArenaPage) + ARENA_PAGE_SIZE;
        ArenaPage* newPage = (ArenaPage*)malloc(allocSize);
        if (!newPage) return NULL;
        newPage->Used = 0;
        newPage->Next = index->arenaPages;
        index->arenaPages = newPage;
        page = newPage;
    }
    
    FileRecord* rec = (FileRecord*)(page->Data + page->Used);
    page->Used += size;
    return rec;
}

static void NtfsIndex_ArenaFreeAll(NtfsIndex* index) {
    ArenaPage* page = index->arenaPages;
    while (page) {
        ArenaPage* next = page->Next;
        free(page);
        page = next;
    }
    index->arenaPages = NULL;
}

static void NtfsIndex_AddHardLink(NtfsIndex* index, unsigned int frs, FileRecord* rec) {
    if (index->hardLinksCount >= index->hardLinksCapacity) {
        size_t newCap = index->hardLinksCapacity == 0 ? 32 : index->hardLinksCapacity * 2;
        HardLinkEntry* newLinks = (HardLinkEntry*)realloc(index->hardLinks, newCap * sizeof(HardLinkEntry));
        if (newLinks) {
            index->hardLinks = newLinks;
            index->hardLinksCapacity = newCap;
        }
    }
    if (index->hardLinksCount < index->hardLinksCapacity) {
        index->hardLinks[index->hardLinksCount].Frs = frs;
        index->hardLinks[index->hardLinksCount].Record = rec;
        index->hardLinksCount++;
    }
}

// Lifecycle management
NtfsIndex* NtfsIndex_Create(wchar_t driveLetter) {
    NtfsIndex* index = (NtfsIndex*)malloc(sizeof(NtfsIndex));
    if (!index) return NULL;

    index->driveLetter = driveLetter;
    index->recordsByFrsPages = NULL;
    index->recordsCount = 0;
    index->activeRecords = NULL;
    index->activeCount = 0;
    index->activeCapacity = 0;
    index->arenaPages = NULL;
    index->hardLinks = NULL;
    index->hardLinksCount = 0;
    index->hardLinksCapacity = 0;
    index->totalFiles = 0;
    index->totalFolders = 0;
    index->isIndexed = false;
    
    InitializeSRWLock(&index->lock);
    return index;
}

void NtfsIndex_Destroy(NtfsIndex* index) {
    if (!index) return;
    
    NtfsIndex_LockExclusive(index);
    if (index->recordsByFrsPages) {
        size_t pagesCount = (index->recordsCount + FRS_PAGE_SIZE - 1) >> FRS_PAGE_SHIFT;
        for (size_t p = 0; p < pagesCount; p++) {
            FileRecord** page = index->recordsByFrsPages[p];
            if (page) {
                free(page);
            }
        }
        free(index->recordsByFrsPages);
    }
    if (index->activeRecords) {
        free(index->activeRecords);
    }
    if (index->hardLinks) {
        free(index->hardLinks);
    }
    NtfsIndex_ArenaFreeAll(index);

    index->recordsByFrsPages = NULL;
    index->activeRecords = NULL;
    index->hardLinks = NULL;
    index->recordsCount = 0;
    index->activeCount = 0;
    index->activeCapacity = 0;
    index->hardLinksCount = 0;
    index->hardLinksCapacity = 0;
    
    NtfsIndex_UnlockExclusive(index);
    free(index);
}

void NtfsIndex_LockShared(NtfsIndex* index) {
    AcquireSRWLockShared(&index->lock);
}

void NtfsIndex_UnlockShared(NtfsIndex* index) {
    ReleaseSRWLockShared(&index->lock);
}

void NtfsIndex_LockExclusive(NtfsIndex* index) {
    AcquireSRWLockExclusive(&index->lock);
}

void NtfsIndex_UnlockExclusive(NtfsIndex* index) {
    ReleaseSRWLockExclusive(&index->lock);
}

void NtfsIndex_InitializeSize(NtfsIndex* index, size_t maxFrsCount) {
    NtfsIndex_LockExclusive(index);
    
    if (index->recordsByFrsPages) {
        size_t pagesCount = (index->recordsCount + FRS_PAGE_SIZE - 1) >> FRS_PAGE_SHIFT;
        for (size_t p = 0; p < pagesCount; p++) {
            FileRecord** page = index->recordsByFrsPages[p];
            if (page) {
                free(page);
            }
        }
        free(index->recordsByFrsPages);
        index->recordsByFrsPages = NULL;
    }
    if (index->hardLinks) {
        free(index->hardLinks);
        index->hardLinks = NULL;
    }
    NtfsIndex_ArenaFreeAll(index);

    index->recordsCount = maxFrsCount;
    size_t pagesCount = (maxFrsCount + FRS_PAGE_SIZE - 1) >> FRS_PAGE_SHIFT;
    index->recordsByFrsPages = (FileRecord***)calloc(pagesCount, sizeof(FileRecord**));
    
    if (index->activeRecords) {
        free(index->activeRecords);
        index->activeRecords = NULL;
    }
    index->activeCount = 0;
    index->activeCapacity = 0;
    index->hardLinksCount = 0;
    index->hardLinksCapacity = 0;
    index->totalFiles = 0;
    index->totalFolders = 0;
    index->isIndexed = false;
    
    // Setup NTFS Root Directory (FRS 5) representation
    if (5 < index->recordsCount) {
        FileRecord* root = NtfsIndex_ArenaAlloc(index, sizeof(FileRecord) + sizeof(wchar_t));
        if (root) {
            root->Name[0] = L'\0';
            root->Frs = 5;
            root->ParentFrs = 5;
            root->Size = 0;
            root->DateModified = 0;
            root->Attributes = FILE_ATTRIBUTE_DIRECTORY;
            
            NtfsIndex_SetRecord(index, 5, root);
            index->totalFolders = 1;
        }
    }
    
    NtfsIndex_UnlockExclusive(index);
}

// Internal structures mapping to attributes types
#define AttributeStandardInformation 0x10
#define AttributeFileName 0x30
#define AttributeData 0x80
#define AttributeEnd 0xFFFFFFFF

#pragma pack(push, 1)
typedef struct {
    unsigned long Magic;
    unsigned short USAOffset;
    unsigned short USACount;
} M_SECTOR_HEADER;

typedef struct {
    unsigned long Type;
    unsigned long Length;
    unsigned char IsNonResident;
    unsigned char NameLength;
    unsigned short NameOffset;
    unsigned short Flags;
    unsigned short Instance;
    union {
        struct {
            unsigned long ValueLength;
            unsigned short ValueOffset;
            unsigned short Flags;
        } Resident;
        struct {
            long long LowestVCN;
            long long HighestVCN;
            unsigned short MappingPairsOffset;
            unsigned char CompressionUnit;
            unsigned char Reserved[5];
            long long AllocatedSize;
            long long DataSize;
            long long InitializedSize;
            long long CompressedSize;
        } NonResident;
    } u;
} A_RECORD_HEADER;

typedef struct {
    M_SECTOR_HEADER MultiSectorHeader;
    unsigned long long LogFileSequenceNumber;
    unsigned short SequenceNumber;
    unsigned short LinkCount;
    unsigned short FirstAttributeOffset;
    unsigned short Flags;
    unsigned long BytesInUse;
    unsigned long BytesAllocated;
    unsigned long long BaseFileRecordSegment;
    unsigned short NextAttributeNumber;
    unsigned short USA_or_Reserved;
    unsigned long SegmentNumberLower;
} F_RECORD_HEADER;

typedef struct {
    unsigned long long ParentDirectory;
    long long CreationTime;
    long long LastModificationTime;
    long long LastChangeTime;
    long long LastAccessTime;
    long long AllocatedLength;
    long long FileSize;
    unsigned long FileAttributes;
    unsigned short PackedEaSize;
    unsigned short Reserved;
    unsigned char FileNameLength;
    unsigned char Flags;
    WCHAR FileName[1];
} FN_INFORMATION;

typedef struct {
    long long CreationTime;
    long long LastModificationTime;
    long long LastChangeTime;
    long long LastAccessTime;
    unsigned long FileAttributes;
} S_INFORMATION;
#pragma pack(pop)

static inline bool UnfixupRecord(F_RECORD_HEADER* header, size_t recordSize) {
    if (header->MultiSectorHeader.USAOffset + header->MultiSectorHeader.USACount * sizeof(unsigned short) > recordSize) {
        return false;
    }
    
    unsigned short* usa = (unsigned short*)((unsigned char*)header + header->MultiSectorHeader.USAOffset);
    unsigned short const usa0 = usa[0];
    
    for (unsigned short i = 1; i < header->MultiSectorHeader.USACount; i++) {
        const size_t offset = i * 512 - sizeof(unsigned short);
        if (offset + sizeof(unsigned short) <= recordSize) {
            unsigned short* const sector_end = (unsigned short*)((unsigned char*)header + offset);
            if (*sector_end == usa0) {
                *sector_end = usa[i];
            } else {
                return false; //USA safety mismatch
            }
        } else {
            break;
        }
    }
    return true;
}

void NtfsIndex_ProcessMftChunk(NtfsIndex* index, unsigned char* chunkBuffer, size_t chunkSize, unsigned int startFrs, unsigned int recordSize) {
    NtfsIndex_LockExclusive(index);
    
    unsigned int numRecords = (unsigned int)(chunkSize / recordSize);
    if (startFrs + numRecords > index->recordsCount) {
        size_t new_size = startFrs + numRecords + 4096;
        size_t oldPagesCount = (index->recordsCount + FRS_PAGE_SIZE - 1) >> FRS_PAGE_SHIFT;
        size_t newPagesCount = (new_size + FRS_PAGE_SIZE - 1) >> FRS_PAGE_SHIFT;
        
        if (newPagesCount > oldPagesCount) {
            FileRecord*** grown = (FileRecord***)realloc(index->recordsByFrsPages, newPagesCount * sizeof(FileRecord**));
            if (grown) {
                memset(grown + oldPagesCount, 0, (newPagesCount - oldPagesCount) * sizeof(FileRecord**));
                index->recordsByFrsPages = grown;
            }
        }
        index->recordsCount = new_size;
    }
    
    for (unsigned int i = 0; i < numRecords; ++i) {
        unsigned int frsIndex = startFrs + i;
        unsigned char* pRecordBytes = chunkBuffer + (i * recordSize);
        F_RECORD_HEADER* frsh = (F_RECORD_HEADER*)pRecordBytes;
        
        // Skip unused/deleted records
        if (frsh->MultiSectorHeader.Magic != 0x454C4946 || !(frsh->Flags & 0x0001)) {
            continue;
        }
        
        // Repair sectors
        if (!UnfixupRecord(frsh, recordSize)) {
            continue;
        }
        
        bool isDirectory = (frsh->Flags & 0x0002) != 0;
        
        const FN_INFORMATION* fnInfos[32];
        int fnInfosCount = 0;
        const FN_INFORMATION* dosFnInfo = NULL;
        
        unsigned long long dateCreated = 0;
        unsigned long long dateModified = 0;
        unsigned long long dateAccessed = 0;
        unsigned int attributes = 0;
        unsigned long long size = 0;
        unsigned long long sizeOnDisk = 0;
        
        A_RECORD_HEADER* attr = (A_RECORD_HEADER*)(pRecordBytes + frsh->FirstAttributeOffset);
        unsigned char* recordEnd = pRecordBytes + recordSize;
        
        while ((unsigned char*)attr + sizeof(A_RECORD_HEADER) <= recordEnd && attr->Type != AttributeEnd) {
            if (attr->Length == 0 || (unsigned char*)attr + attr->Length > recordEnd) {
                break;
            }
            
            if (attr->Type == AttributeStandardInformation) {
                if (!attr->IsNonResident) {
                    const S_INFORMATION* stdInfo = (const S_INFORMATION*)((unsigned char*)attr + attr->u.Resident.ValueOffset);
                    dateCreated = stdInfo->CreationTime;
                    dateModified = stdInfo->LastModificationTime;
                    dateAccessed = stdInfo->LastAccessTime;
                    attributes = stdInfo->FileAttributes;
                }
            }
            else if (attr->Type == AttributeFileName) {
                if (!attr->IsNonResident) {
                    const FN_INFORMATION* fnInfo = (const FN_INFORMATION*)((unsigned char*)attr + attr->u.Resident.ValueOffset);
                    if (fnInfo->Flags == 2) {
                        dosFnInfo = fnInfo;
                    } else {
                        if (fnInfosCount < 32) {
                            fnInfos[fnInfosCount++] = fnInfo;
                        }
                    }
                }
            }
            else if (attr->Type == AttributeData) {
                if (attr->IsNonResident) {
                    size = attr->u.NonResident.DataSize;
                    sizeOnDisk = attr->u.NonResident.AllocatedSize;
                } else {
                    size = attr->u.Resident.ValueLength;
                    sizeOnDisk = attr->Length;
                }
            }
            
            attr = (A_RECORD_HEADER*)((unsigned char*)attr + attr->Length);
        }
        
        // Clean old records first if any exist (though rare in initial index, handled for safety)
        FileRecord* oldRec = NtfsIndex_GetRecord(index, frsIndex);
        if (oldRec) {
            // Note: Since we are using the Arena Allocator, we cannot free individual FileRecords.
            // But we remove them from the sparse table to keep the index correct.
            NtfsIndex_SetRecord(index, frsIndex, NULL);
        }
        
        // Allocate and link records
        if (fnInfosCount > 0) {
            for (int k = 0; k < fnInfosCount; ++k) {
                const FN_INFORMATION* fnInfo = fnInfos[k];
                if (fnInfo->FileNameLength > 0 && fnInfo->FileNameLength < 260) {
                    size_t nameLen = fnInfo->FileNameLength;
                    FileRecord* item = NtfsIndex_ArenaAlloc(index, sizeof(FileRecord) + (nameLen + 1) * sizeof(wchar_t));
                    if (item) {
                        memcpy(item->Name, fnInfo->FileName, nameLen * sizeof(wchar_t));
                        item->Name[nameLen] = L'\0';
                        item->Frs = frsIndex;
                        item->ParentFrs = (unsigned int)(fnInfo->ParentDirectory & 0x0000FFFFFFFFFFFFLL);
                        item->DateModified = dateModified;
                        item->Attributes = attributes;
                        item->Size = size;
                        
                        FileRecord* existing = NtfsIndex_GetRecord(index, frsIndex);
                        if (!existing) {
                            NtfsIndex_SetRecord(index, frsIndex, item);
                        } else {
                            NtfsIndex_AddHardLink(index, frsIndex, item);
                        }
                        
                        if (isDirectory) index->totalFolders++;
                        else index->totalFiles++;
                    }
                }
            }
        }
        else if (dosFnInfo != NULL) {
            const FN_INFORMATION* fnInfo = dosFnInfo;
            if (fnInfo->FileNameLength > 0 && fnInfo->FileNameLength < 260) {
                size_t nameLen = fnInfo->FileNameLength;
                FileRecord* item = NtfsIndex_ArenaAlloc(index, sizeof(FileRecord) + (nameLen + 1) * sizeof(wchar_t));
                if (item) {
                    memcpy(item->Name, fnInfo->FileName, nameLen * sizeof(wchar_t));
                    item->Name[nameLen] = L'\0';
                    item->Frs = frsIndex;
                    item->ParentFrs = (unsigned int)(fnInfo->ParentDirectory & 0x0000FFFFFFFFFFFFLL);
                    item->DateModified = dateModified;
                    item->Attributes = attributes;
                    item->Size = size;
                    
                    FileRecord* existing = NtfsIndex_GetRecord(index, frsIndex);
                    if (!existing) {
                        NtfsIndex_SetRecord(index, frsIndex, item);
                    } else {
                        NtfsIndex_AddHardLink(index, frsIndex, item);
                    }
                    
                    if (isDirectory) index->totalFolders++;
                    else index->totalFiles++;
                }
            }
        }
    }
    
    NtfsIndex_UnlockExclusive(index);
}

// Alphabetical pre-sorting comparator
static int CompareFileRecords(const void* a, const void* b) {
    const FileRecord* recA = *(const FileRecord**)a;
    const FileRecord* recB = *(const FileRecord**)b;
    
    // Always keep standard system '$' metadata files at the end
    bool aStartsWithDollar = (recA->Name && recA->Name[0] == L'$');
    bool bStartsWithDollar = (recB->Name && recB->Name[0] == L'$');
    if (aStartsWithDollar != bStartsWithDollar) {
        return aStartsWithDollar ? 1 : -1;
    }
    
    return _wcsicmp(recA->Name, recB->Name);
}

void NtfsIndex_FinalizeIndex(NtfsIndex* index) {
    NtfsIndex_LockExclusive(index);
    
    index->activeCount = 0;
    size_t count = index->totalFiles + index->totalFolders;
    if (count > 0) {
        index->activeCapacity = count + 1024;
        index->activeRecords = (FileRecord**)malloc(index->activeCapacity * sizeof(FileRecord*));
        
        if (index->activeRecords) {
            size_t pagesCount = (index->recordsCount + FRS_PAGE_SIZE - 1) >> FRS_PAGE_SHIFT;
            for (size_t p = 0; p < pagesCount; p++) {
                FileRecord** page = index->recordsByFrsPages[p];
                if (page) {
                    for (int i = 0; i < FRS_PAGE_SIZE; i++) {
                        FileRecord* rec = page[i];
                        if (rec && rec->Name && rec->Name[0] != L'\0') {
                            if (index->activeCount < index->activeCapacity) {
                                index->activeRecords[index->activeCount++] = rec;
                            }
                        }
                    }
                }
            }
            
            // Add all hard link records
            for (size_t h = 0; h < index->hardLinksCount; h++) {
                FileRecord* rec = index->hardLinks[h].Record;
                if (rec && rec->Name && rec->Name[0] != L'\0') {
                    if (index->activeCount < index->activeCapacity) {
                        index->activeRecords[index->activeCount++] = rec;
                    }
                }
            }
            
            // High-performance pre-sorting of all active records alphabetically by name
            qsort(index->activeRecords, index->activeCount, sizeof(FileRecord*), CompareFileRecords);
        }
    }
    
    index->isIndexed = true;
    NtfsIndex_UnlockExclusive(index);
}

size_t NtfsIndex_ResolveFullPathToBuf(const NtfsIndex* index, const FileRecord* item, wchar_t* outBuf, size_t maxChars) {
    if (!item) {
        if (maxChars > 0) outBuf[0] = L'\0';
        return 0;
    }
    
    // Collect parent references on the stack (max depth 256)
    const FileRecord* pathRecords[256];
    int depth = 0;
    
    pathRecords[depth++] = item;
    unsigned int current = item->ParentFrs;
    
    // Walk up the parent tree recursively to root FRS (5 or 0)
    while (current < index->recordsCount && current != 5 && current != 0 && depth < 256) {
        const FileRecord* pRec = NtfsIndex_GetRecord(index, current);
        if (!pRec || !pRec->Name || pRec->Name[0] == L'\0') {
            break;
        }
        if (pRec->ParentFrs == 0xFFFFFFFF || pRec->ParentFrs == current) {
            break;
        }
        pathRecords[depth++] = pRec;
        current = pRec->ParentFrs;
    }
    
    size_t written = 0;
    if (maxChars > 4) {
        outBuf[0] = index->driveLetter;
        outBuf[1] = L':';
        outBuf[2] = L'\\';
        outBuf[3] = L'\0';
        written = 3;
    }
    
    // Join paths forwards: parents to children
    for (int i = depth - 1; i >= 0; --i) {
        const FileRecord* pRec = pathRecords[i];
        size_t nameLen = wcslen(pRec->Name);
        if (written + nameLen + 2 > maxChars) {
            break;
        }
        if (i < depth - 1) {
            outBuf[written++] = L'\\';
        }
        wcscpy_s(outBuf + written, maxChars - written, pRec->Name);
        written += nameLen;
    }
    outBuf[written] = L'\0';
    return written;
}

size_t NtfsIndex_ResolveFullPathToBufByFrs(const NtfsIndex* index, unsigned int recordIndex, wchar_t* outBuf, size_t maxChars) {
    NtfsIndex_LockShared((NtfsIndex*)index);
    if (recordIndex >= index->recordsCount) {
        if (maxChars > 0) outBuf[0] = L'\0';
        NtfsIndex_UnlockShared((NtfsIndex*)index);
        return 0;
    }
    size_t result = NtfsIndex_ResolveFullPathToBuf(index, NtfsIndex_GetRecord(index, recordIndex), outBuf, maxChars);
    NtfsIndex_UnlockShared((NtfsIndex*)index);
    return result;
}

size_t NtfsIndex_ResolveFullPathFromParent(const NtfsIndex* index, unsigned int parentFrs, const wchar_t* name, wchar_t* outBuf, size_t maxChars) {
    if (maxChars == 0) return 0;
    
    // If parentFrs is 0 or 5 (root), path is just Drive:\name
    if (parentFrs == 0 || parentFrs == 5 || parentFrs == 0xFFFFFFFF) {
        size_t written = 0;
        if (maxChars > 4) {
            outBuf[0] = index->driveLetter;
            outBuf[1] = L':';
            outBuf[2] = L'\\';
            outBuf[3] = L'\0';
            written = 3;
        }
        size_t nameLen = name ? wcslen(name) : 0;
        if (written + nameLen + 1 <= maxChars) {
            wcscpy_s(outBuf + written, maxChars - written, name);
            written += nameLen;
        }
        outBuf[written] = L'\0';
        return written;
    }

    // Collect parent references on the stack (max depth 256)
    const FileRecord* pathRecords[256];
    int depth = 0;
    
    unsigned int current = parentFrs;
    
    // Walk up the parent tree recursively to root FRS (5 or 0)
    while (current < index->recordsCount && current != 5 && current != 0 && depth < 256) {
        const FileRecord* pRec = NtfsIndex_GetRecord(index, current);
        if (!pRec || !pRec->Name || pRec->Name[0] == L'\0') {
            break;
        }
        if (pRec->ParentFrs == 0xFFFFFFFF || pRec->ParentFrs == current) {
            break;
        }
        pathRecords[depth++] = pRec;
        current = pRec->ParentFrs;
    }
    
    size_t written = 0;
    if (maxChars > 4) {
        outBuf[0] = index->driveLetter;
        outBuf[1] = L':';
        outBuf[2] = L'\\';
        outBuf[3] = L'\0';
        written = 3;
    }
    
    // Join paths forwards: parents to children
    for (int i = depth - 1; i >= 0; --i) {
        const FileRecord* pRec = pathRecords[i];
        size_t nameLen = wcslen(pRec->Name);
        if (written + nameLen + 2 > maxChars) {
            break;
        }
        if (i < depth - 1) {
            outBuf[written++] = L'\\';
        }
        wcscpy_s(outBuf + written, maxChars - written, pRec->Name);
        written += nameLen;
    }

    // Append a slash and the leaf item name
    size_t nameLen = name ? wcslen(name) : 0;
    if (written + nameLen + 2 <= maxChars) {
        if (depth > 0) {
            outBuf[written++] = L'\\';
        }
        wcscpy_s(outBuf + written, maxChars - written, name);
        written += nameLen;
    }
    
    outBuf[written] = L'\0';
    return written;
}

// Binary search to find sorted index in activeRecords
static int FindActiveRecordIndex(FileRecord** active, size_t count, const FileRecord* target, int* out_exact) {
    int low = 0;
    int high = (int)count - 1;
    *out_exact = 0;
    
    while (low <= high) {
        int mid = low + (high - low) / 2;
        int cmp = CompareFileRecords(&active[mid], &target);
        
        if (cmp < 0) {
            low = mid + 1;
        } else if (cmp > 0) {
            high = mid - 1;
        } else {
            // Found alphabetical match, scan around to find exact pointer match
            int i = mid;
            while (i >= 0 && CompareFileRecords(&active[i], &target) == 0) {
                if (active[i] == target) {
                    *out_exact = 1;
                    return i;
                }
                i--;
            }
            i = mid + 1;
            while (i < (int)count && CompareFileRecords(&active[i], &target) == 0) {
                if (active[i] == target) {
                    *out_exact = 1;
                    return i;
                }
                i++;
            }
            return mid; // alphabetically correct insertion point
        }
    }
    return low;
}

void NtfsIndex_AddOrUpdateRecord(NtfsIndex* index, unsigned int recordIndex, const wchar_t* name, unsigned int parentFrs, 
                                 unsigned long long size, unsigned long long sizeOnDisk, 
                                 unsigned long long dateCreated, unsigned long long dateModified, unsigned long long dateAccessed, 
                                 unsigned int attributes, bool isDirectory) {
    NtfsIndex_LockExclusive(index);
    
    if (recordIndex >= index->recordsCount) {
        size_t new_size = recordIndex + 4096;
        size_t oldPagesCount = (index->recordsCount + FRS_PAGE_SIZE - 1) >> FRS_PAGE_SHIFT;
        size_t newPagesCount = (new_size + FRS_PAGE_SIZE - 1) >> FRS_PAGE_SHIFT;
        
        if (newPagesCount > oldPagesCount) {
            FileRecord*** grown = (FileRecord***)realloc(index->recordsByFrsPages, newPagesCount * sizeof(FileRecord**));
            if (grown) {
                memset(grown + oldPagesCount, 0, (newPagesCount - oldPagesCount) * sizeof(FileRecord**));
                index->recordsByFrsPages = grown;
            }
        }
        index->recordsCount = new_size;
    }
    
    // Clean old records first to avoid duplicates
    FileRecord* oldRec = NtfsIndex_GetRecord(index, recordIndex);
    if (oldRec) {
        int exact = 0;
        int actIdx = FindActiveRecordIndex(index->activeRecords, index->activeCount, oldRec, &exact);
        if (exact && actIdx >= 0 && actIdx < (int)index->activeCount) {
            memmove(index->activeRecords + actIdx, index->activeRecords + actIdx + 1, (index->activeCount - actIdx - 1) * sizeof(FileRecord*));
            index->activeCount--;
        }
        
        if (oldRec->Attributes & FILE_ATTRIBUTE_DIRECTORY) index->totalFolders--;
        else index->totalFiles--;
        
        NtfsIndex_SetRecord(index, recordIndex, NULL);
    }
    
    // Also remove any hard links associated with this FRS index
    for (size_t i = 0; i < index->hardLinksCount; ) {
        if (index->hardLinks[i].Frs == recordIndex) {
            FileRecord* hlRec = index->hardLinks[i].Record;
            int exact = 0;
            int actIdx = FindActiveRecordIndex(index->activeRecords, index->activeCount, hlRec, &exact);
            if (exact && actIdx >= 0 && actIdx < (int)index->activeCount) {
                memmove(index->activeRecords + actIdx, index->activeRecords + actIdx + 1, (index->activeCount - actIdx - 1) * sizeof(FileRecord*));
                index->activeCount--;
            }
            if (hlRec->Attributes & FILE_ATTRIBUTE_DIRECTORY) index->totalFolders--;
            else index->totalFiles--;
            
            // Swap-remove from hardLinks array
            if (i < index->hardLinksCount - 1) {
                index->hardLinks[i] = index->hardLinks[index->hardLinksCount - 1];
            }
            index->hardLinksCount--;
        } else {
            i++;
        }
    }
    
    // Add the new record
    size_t nameLen = name ? wcslen(name) : 0;
    FileRecord* item = NtfsIndex_ArenaAlloc(index, sizeof(FileRecord) + (nameLen + 1) * sizeof(wchar_t));
    if (item) {
        if (name) {
            wcscpy_s(item->Name, nameLen + 1, name);
        } else {
            item->Name[0] = L'\0';
        }
        item->Frs = recordIndex;
        item->ParentFrs = parentFrs;
        item->Size = size;
        item->DateModified = dateModified;
        item->Attributes = attributes;
        
        NtfsIndex_SetRecord(index, recordIndex, item);
        
        if (attributes & FILE_ATTRIBUTE_DIRECTORY) index->totalFolders++;
        else index->totalFiles++;
        
        // Insert into activeRecords
        if (index->activeCount >= index->activeCapacity) {
            index->activeCapacity = index->activeCount + 1024;
            index->activeRecords = (FileRecord**)realloc(index->activeRecords, index->activeCapacity * sizeof(FileRecord*));
        }
        
        if (index->activeRecords) {
            int unused_exact = 0;
            int insIdx = FindActiveRecordIndex(index->activeRecords, index->activeCount, item, &unused_exact);
            memmove(index->activeRecords + insIdx + 1, index->activeRecords + insIdx, (index->activeCount - insIdx) * sizeof(FileRecord*));
            index->activeRecords[insIdx] = item;
            index->activeCount++;
        }
    }
    
    NtfsIndex_UnlockExclusive(index);
}

void NtfsIndex_DeleteRecord(NtfsIndex* index, unsigned int recordIndex) {
    NtfsIndex_LockExclusive(index);
    
    if (recordIndex < index->recordsCount) {
        FileRecord* rec = NtfsIndex_GetRecord(index, recordIndex);
        if (rec) {
            // Remove from activeRecords
            int exact = 0;
            int actIdx = FindActiveRecordIndex(index->activeRecords, index->activeCount, rec, &exact);
            if (exact && actIdx >= 0 && actIdx < (int)index->activeCount) {
                memmove(index->activeRecords + actIdx, index->activeRecords + actIdx + 1, (index->activeCount - actIdx - 1) * sizeof(FileRecord*));
                index->activeCount--;
            }
            
            if (rec->Attributes & FILE_ATTRIBUTE_DIRECTORY) index->totalFolders--;
            else index->totalFiles--;
            
            NtfsIndex_SetRecord(index, recordIndex, NULL);
        }
        
        // Also delete hard links
        for (size_t i = 0; i < index->hardLinksCount; ) {
            if (index->hardLinks[i].Frs == recordIndex) {
                FileRecord* hlRec = index->hardLinks[i].Record;
                int exact = 0;
                int actIdx = FindActiveRecordIndex(index->activeRecords, index->activeCount, hlRec, &exact);
                if (exact && actIdx >= 0 && actIdx < (int)index->activeCount) {
                    memmove(index->activeRecords + actIdx, index->activeRecords + actIdx + 1, (index->activeCount - actIdx - 1) * sizeof(FileRecord*));
                    index->activeCount--;
                }
                if (hlRec->Attributes & FILE_ATTRIBUTE_DIRECTORY) index->totalFolders--;
                else index->totalFiles--;
                
                // Swap-remove from hardLinks array
                if (i < index->hardLinksCount - 1) {
                    index->hardLinks[i] = index->hardLinks[index->hardLinksCount - 1];
                }
                index->hardLinksCount--;
            } else {
                i++;
            }
        }
    }
    
    NtfsIndex_UnlockExclusive(index);
}
