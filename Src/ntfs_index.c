#include "ntfs_index.h"

// Lifecycle management
NtfsIndex* NtfsIndex_Create(wchar_t driveLetter) {
    NtfsIndex* index = (NtfsIndex*)malloc(sizeof(NtfsIndex));
    if (!index) return NULL;

    index->driveLetter = driveLetter;
    index->recordsByFrs = NULL;
    index->recordsCount = 0;
    index->activeRecords = NULL;
    index->activeCount = 0;
    index->activeCapacity = 0;
    index->totalFiles = 0;
    index->totalFolders = 0;
    index->isIndexed = false;
    
    InitializeSRWLock(&index->lock);
    return index;
}

void NtfsIndex_Destroy(NtfsIndex* index) {
    if (!index) return;
    
    NtfsIndex_LockExclusive(index);
    if (index->recordsByFrs) {
        for (size_t i = 0; i < index->recordsCount; i++) {
            FileRecord* rec = index->recordsByFrs[i];
            while (rec) {
                FileRecord* next = rec->Next;
                if (rec->Name) {
                    free(rec->Name);
                }
                free(rec);
                rec = next;
            }
        }
        free(index->recordsByFrs);
    }
    if (index->activeRecords) {
        free(index->activeRecords);
    }
    index->recordsByFrs = NULL;
    index->activeRecords = NULL;
    index->recordsCount = 0;
    index->activeCount = 0;
    index->activeCapacity = 0;
    
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
    
    if (index->recordsByFrs) {
        for (size_t i = 0; i < index->recordsCount; i++) {
            FileRecord* rec = index->recordsByFrs[i];
            while (rec) {
                FileRecord* next = rec->Next;
                if (rec->Name) free(rec->Name);
                free(rec);
                rec = next;
            }
        }
        free(index->recordsByFrs);
    }
    
    index->recordsCount = maxFrsCount;
    index->recordsByFrs = (FileRecord**)calloc(maxFrsCount, sizeof(FileRecord*));
    
    if (index->activeRecords) {
        free(index->activeRecords);
        index->activeRecords = NULL;
    }
    index->activeCount = 0;
    index->activeCapacity = 0;
    index->totalFiles = 0;
    index->totalFolders = 0;
    index->isIndexed = false;
    
    // Setup NTFS Root Directory (FRS 5) representation
    if (5 < index->recordsCount) {
        index->recordsByFrs[5] = (FileRecord*)malloc(sizeof(FileRecord));
        if (index->recordsByFrs[5]) {
            index->recordsByFrs[5]->Name = _wcsdup(L"");
            index->recordsByFrs[5]->Frs = 5;
            index->recordsByFrs[5]->ParentFrs = 5;
            index->recordsByFrs[5]->IsDirectory = true;
            index->recordsByFrs[5]->Size = 0;
            index->recordsByFrs[5]->SizeOnDisk = 0;
            index->recordsByFrs[5]->DateCreated = 0;
            index->recordsByFrs[5]->DateModified = 0;
            index->recordsByFrs[5]->DateAccessed = 0;
            index->recordsByFrs[5]->Attributes = FILE_ATTRIBUTE_DIRECTORY;
            index->recordsByFrs[5]->Next = NULL;
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
        FileRecord** grown = (FileRecord**)realloc(index->recordsByFrs, new_size * sizeof(FileRecord*));
        if (grown) {
            memset(grown + index->recordsCount, 0, (new_size - index->recordsCount) * sizeof(FileRecord*));
            index->recordsByFrs = grown;
            index->recordsCount = new_size;
        }
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
        
        // Clean old records first if any exist
        if (index->recordsByFrs[frsIndex]) {
            FileRecord* rec = index->recordsByFrs[frsIndex];
            while (rec) {
                FileRecord* next = rec->Next;
                if (rec->Name) free(rec->Name);
                free(rec);
                rec = next;
            }
            index->recordsByFrs[frsIndex] = NULL;
        }
        
        // Allocate and link records
        if (fnInfosCount > 0) {
            for (int k = 0; k < fnInfosCount; ++k) {
                const FN_INFORMATION* fnInfo = fnInfos[k];
                if (fnInfo->FileNameLength > 0 && fnInfo->FileNameLength < 260) {
                    FileRecord* item = (FileRecord*)malloc(sizeof(FileRecord));
                    if (item) {
                        item->IsDirectory = isDirectory;
                        item->Name = (wchar_t*)malloc((fnInfo->FileNameLength + 1) * sizeof(wchar_t));
                        if (item->Name) {
                            wcsncpy_s(item->Name, fnInfo->FileNameLength + 1, fnInfo->FileName, fnInfo->FileNameLength);
                            item->Name[fnInfo->FileNameLength] = L'\0';
                        }
                        item->Frs = frsIndex;
                        item->ParentFrs = (unsigned int)(fnInfo->ParentDirectory & 0x0000FFFFFFFFFFFFLL);
                        item->DateCreated = dateCreated;
                        item->DateModified = dateModified;
                        item->DateAccessed = dateAccessed;
                        item->Attributes = attributes;
                        item->Size = size;
                        item->SizeOnDisk = sizeOnDisk;
                        
                        // Link as head
                        item->Next = index->recordsByFrs[frsIndex];
                        index->recordsByFrs[frsIndex] = item;
                        
                        if (isDirectory) index->totalFolders++;
                        else index->totalFiles++;
                    }
                }
            }
        }
        else if (dosFnInfo != NULL) {
            const FN_INFORMATION* fnInfo = dosFnInfo;
            if (fnInfo->FileNameLength > 0 && fnInfo->FileNameLength < 260) {
                FileRecord* item = (FileRecord*)malloc(sizeof(FileRecord));
                if (item) {
                    item->IsDirectory = isDirectory;
                    item->Name = (wchar_t*)malloc((fnInfo->FileNameLength + 1) * sizeof(wchar_t));
                    if (item->Name) {
                        wcsncpy_s(item->Name, fnInfo->FileNameLength + 1, fnInfo->FileName, fnInfo->FileNameLength);
                        item->Name[fnInfo->FileNameLength] = L'\0';
                    }
                    item->Frs = frsIndex;
                    item->ParentFrs = (unsigned int)(fnInfo->ParentDirectory & 0x0000FFFFFFFFFFFFLL);
                    item->DateCreated = dateCreated;
                    item->DateModified = dateModified;
                    item->DateAccessed = dateAccessed;
                    item->Attributes = attributes;
                    item->Size = size;
                    item->SizeOnDisk = sizeOnDisk;
                    
                    // Link as head
                    item->Next = index->recordsByFrs[frsIndex];
                    index->recordsByFrs[frsIndex] = item;
                    
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
            for (size_t i = 0; i < index->recordsCount; i++) {
                FileRecord* rec = index->recordsByFrs[i];
                while (rec) {
                    if (rec->Name && rec->Name[0] != L'\0') {
                        if (index->activeCount < index->activeCapacity) {
                            index->activeRecords[index->activeCount++] = rec;
                        }
                    }
                    rec = rec->Next;
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
        const FileRecord* pRec = index->recordsByFrs[current];
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
    if (recordIndex >= index->recordsCount) {
        if (maxChars > 0) outBuf[0] = L'\0';
        return 0;
    }
    return NtfsIndex_ResolveFullPathToBuf(index, index->recordsByFrs[recordIndex], outBuf, maxChars);
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
        FileRecord** grown = (FileRecord**)realloc(index->recordsByFrs, new_size * sizeof(FileRecord*));
        if (grown) {
            memset(grown + index->recordsCount, 0, (new_size - index->recordsCount) * sizeof(FileRecord*));
            index->recordsByFrs = grown;
            index->recordsCount = new_size;
        }
    }
    
    // Clean old records first to avoid duplicates or memory leaks
    FileRecord* oldRec = index->recordsByFrs[recordIndex];
    while (oldRec) {
        FileRecord* next = oldRec->Next;
        
        int exact = 0;
        int actIdx = FindActiveRecordIndex(index->activeRecords, index->activeCount, oldRec, &exact);
        if (exact && actIdx >= 0 && actIdx < (int)index->activeCount) {
            memmove(index->activeRecords + actIdx, index->activeRecords + actIdx + 1, (index->activeCount - actIdx - 1) * sizeof(FileRecord*));
            index->activeCount--;
        }
        
        if (oldRec->IsDirectory) index->totalFolders--;
        else index->totalFiles--;
        
        if (oldRec->Name) free(oldRec->Name);
        free(oldRec);
        
        oldRec = next;
    }
    index->recordsByFrs[recordIndex] = NULL;
    
    // Add the new record
    FileRecord* item = (FileRecord*)malloc(sizeof(FileRecord));
    if (item) {
        item->IsDirectory = isDirectory;
        item->Name = _wcsdup(name);
        item->Frs = recordIndex;
        item->ParentFrs = parentFrs;
        item->Size = size;
        item->SizeOnDisk = sizeOnDisk;
        item->DateCreated = dateCreated;
        item->DateModified = dateModified;
        item->DateAccessed = dateAccessed;
        item->Attributes = attributes;
        item->Next = NULL;
        
        index->recordsByFrs[recordIndex] = item;
        
        if (isDirectory) index->totalFolders++;
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
        FileRecord* rec = index->recordsByFrs[recordIndex];
        while (rec) {
            FileRecord* next = rec->Next;
            
            // Remove from activeRecords
            int exact = 0;
            int actIdx = FindActiveRecordIndex(index->activeRecords, index->activeCount, rec, &exact);
            if (exact && actIdx >= 0 && actIdx < (int)index->activeCount) {
                memmove(index->activeRecords + actIdx, index->activeRecords + actIdx + 1, (index->activeCount - actIdx - 1) * sizeof(FileRecord*));
                index->activeCount--;
            }
            
            if (rec->IsDirectory) index->totalFolders--;
            else index->totalFiles--;
            
            if (rec->Name) free(rec->Name);
            free(rec);
            
            rec = next;
        }
        index->recordsByFrs[recordIndex] = NULL;
    }
    
    NtfsIndex_UnlockExclusive(index);
}
