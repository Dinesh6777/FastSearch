#include "search_engine.h"
#include "mft_reader.h"
#include <shlwapi.h>

#define WM_NTFS_INDEX_CHANGED (WM_USER + 102)
#define WM_SORTING_STATUS (WM_USER + 106)

SearchEngine* SearchEngine_Create(void) {
    SearchEngine* engine = (SearchEngine*)malloc(sizeof(SearchEngine));
    if (!engine) return NULL;

    engine->drivesCount = 0;
    engine->notifyWindow = NULL;
    engine->matchPath = false;
    StringMatcher_Init(&engine->lastMatcher);
    InitializeSRWLock(&engine->drivesLock);

    return engine;
}

void SearchEngine_Destroy(SearchEngine* engine) {
    if (!engine) return;

    AcquireSRWLockExclusive(&engine->drivesLock);
    for (int i = 0; i < engine->drivesCount; i++) {
        UsnJournalMonitor_Stop(engine->drives[i].Monitor);
        UsnJournalMonitor_Destroy(engine->drives[i].Monitor);
        NtfsIndex_Destroy(engine->drives[i].Index);
    }
    engine->drivesCount = 0;
    ReleaseSRWLockExclusive(&engine->drivesLock);

    StringMatcher_Free(&engine->lastMatcher);
    free(engine);
}

void SearchEngine_RegisterNotifyWindow(SearchEngine* engine, HWND hwnd) {
    AcquireSRWLockExclusive(&engine->drivesLock);
    engine->notifyWindow = hwnd;
    for (int i = 0; i < engine->drivesCount; i++) {
        UsnJournalMonitor_RegisterNotifyWindow(engine->drives[i].Monitor, hwnd);
    }
    ReleaseSRWLockExclusive(&engine->drivesLock);
}

// Category filter extensions helper
static bool HasExtension(const wchar_t* filename, const wchar_t** extensions, int extCount) {
    const wchar_t* dot = wcsrchr(filename, L'.');
    if (!dot) return false;

    wchar_t extLower[32];
    size_t len = wcslen(dot);
    if (len >= 32) len = 31;
    for (size_t i = 0; i < len; i++) {
        extLower[i] = towlower(dot[i]);
    }
    extLower[len] = L'\0';

    for (int i = 0; i < extCount; i++) {
        if (wcscmp(extLower, extensions[i]) == 0) {
            return true;
        }
    }
    return false;
}

static bool FilePassesFilter(const wchar_t* filename, bool isDirectory, FilterType filter) {
    if (filter == FilterType_All) return true;
    if (filter == FilterType_Folders) return isDirectory;
    if (filter == FilterType_Files) return !isDirectory;

    if (isDirectory) return false;

    switch (filter) {
        case FilterType_Documents: {
            const wchar_t* extensions[] = { L".doc", L".docx", L".xls", L".xlsx", L".ppt", L".pptx", 
                                             L".pdf", L".txt", L".rtf", L".odt", L".ods", L".odp", L".csv" };
            return HasExtension(filename, extensions, 13);
        }
        case FilterType_Executables: {
            const wchar_t* extensions[] = { L".exe", L".bat", L".cmd", L".msi", L".lnk", L".com", L".ps1" };
            return HasExtension(filename, extensions, 7);
        }
        case FilterType_Pictures: {
            const wchar_t* extensions[] = { L".png", L".jpg", L".jpeg", L".gif", L".bmp", L".svg", 
                                             L".ico", L".tiff", L".webp" };
            return HasExtension(filename, extensions, 9);
        }
        case FilterType_Audio: {
            const wchar_t* extensions[] = { L".mp3", L".wav", L".wma", L".m4a", L".flac", L".aac", 
                                             L".ogg", L".mid" };
            return HasExtension(filename, extensions, 8);
        }
        case FilterType_Video: {
            const wchar_t* extensions[] = { L".mp4", L".mkv", L".avi", L".mov", L".wmv", L".flv", 
                                             L".webm", L".mpg", L".mpeg" };
            return HasExtension(filename, extensions, 9);
        }
        default:
            break;
    }
    return true;
}

// Asynchronous raw indexing loader callback contexts
typedef struct {
    wchar_t drive;
    IIndexProgressCallback uiCallback;
    NtfsIndex* index;
    volatile LONG completed;
    volatile LONG success;
    unsigned int recSize;
    HWND notifyWindow;
} MftThreadCbContext;

static void OnMftStart(void* ctx, unsigned int totalExpected) {
    MftThreadCbContext* c = (MftThreadCbContext*)ctx;
    NtfsIndex_InitializeSize(c->index, totalExpected);
}

static void OnMftProgress(void* ctx, unsigned int current, unsigned int total) {
    MftThreadCbContext* c = (MftThreadCbContext*)ctx;
    if (c->uiCallback.OnIndexProgress) {
        c->uiCallback.OnIndexProgress(c->uiCallback.Context, c->drive, current, total);
    }
}

static void OnMftChunk(void* ctx, unsigned char* chunkBuffer, size_t chunkSize, unsigned int startFrs, unsigned int recordSize) {
    MftThreadCbContext* c = (MftThreadCbContext*)ctx;
    NtfsIndex_ProcessMftChunk(c->index, chunkBuffer, chunkSize, startFrs, recordSize);
}

static void OnMftComplete(void* ctx, bool success, unsigned int recordSize) {
    MftThreadCbContext* c = (MftThreadCbContext*)ctx;
    c->recSize = recordSize;
    InterlockedExchange(&c->success, success ? 1 : 0);
    if (success) {
        if (c->notifyWindow && IsWindow(c->notifyWindow)) {
            SendMessageW(c->notifyWindow, WM_SORTING_STATUS, 0, 0);
        }
        NtfsIndex_FinalizeIndex(c->index);
    }
    InterlockedExchange(&c->completed, 1);
}

typedef struct {
    SearchEngine* engine;
    wchar_t driveLetter;
    NtfsIndex* index;
    UsnJournalMonitor* monitor;
    IIndexProgressCallback uiCallback;
} IndexerThreadArgs;

static unsigned int __stdcall IndexerThreadProc(void* arg) {
    IndexerThreadArgs* args = (IndexerThreadArgs*)arg;
    
    MftThreadCbContext mftCb;
    mftCb.drive = args->driveLetter;
    mftCb.uiCallback = args->uiCallback;
    mftCb.index = args->index;
    mftCb.completed = 0;
    mftCb.success = 0;
    mftCb.recSize = 0;
    mftCb.notifyWindow = args->engine->notifyWindow;

    IMftProgressCallback cb;
    cb.Context = &mftCb;
    cb.OnStart = OnMftStart;
    cb.OnProgress = OnMftProgress;
    cb.OnChunk = OnMftChunk;
    cb.OnComplete = OnMftComplete;

    MftReader* reader = MftReader_Create();
    if (reader) {
        MftReader_ReadMftAsync(reader, args->driveLetter, cb);

        // Block background worker until MFT clusters are read
        while (!InterlockedOr(&mftCb.completed, 0)) {
            Sleep(50);
        }

        MftReader_Destroy(reader);
    }

    bool wasSuccess = InterlockedOr(&mftCb.success, 0) != 0;
    if (wasSuccess) {
        // Automatically start real-time USN journaling monitor
        UsnJournalMonitor_Start(args->monitor);
    }

    if (args->uiCallback.OnIndexComplete) {
        args->uiCallback.OnIndexComplete(args->uiCallback.Context, args->driveLetter, wasSuccess, 
                                         args->index->totalFiles, args->index->totalFolders);
    }

    free(args);
    return 0;
}

void SearchEngine_InitializeDrives(SearchEngine* engine, IIndexProgressCallback callback) {
    AcquireSRWLockExclusive(&engine->drivesLock);

    DWORD mask = GetLogicalDrives();
    wchar_t activeDrives[26];
    int activeDrivesCount = 0;

    // 1. Discover all fixed NTFS volumes
    for (int i = 0; i < 26; ++i) {
        if (mask & (1 << i)) {
            wchar_t driveLetter = L'A' + i;
            wchar_t drivePath[4];
            swprintf_s(drivePath, 4, L"%c:\\", driveLetter);

            if (GetDriveTypeW(drivePath) == DRIVE_FIXED) {
                wchar_t fsName[100] = { 0 };
                if (GetVolumeInformationW(drivePath, NULL, 0, NULL, NULL, NULL, fsName, 100)) {
                    if (wcscmp(fsName, L"NTFS") == 0) {
                        activeDrives[activeDrivesCount++] = driveLetter;
                    }
                }
            }
        }
    }

    // 2. Prune disconnected or ejected drives
    for (int i = engine->drivesCount - 1; i >= 0; i--) {
        wchar_t letter = engine->drives[i].Index->driveLetter;
        bool stillActive = false;
        for (int k = 0; k < activeDrivesCount; k++) {
            if (activeDrives[k] == letter) {
                stillActive = true;
                break;
            }
        }

        if (!stillActive) {
            UsnJournalMonitor_Stop(engine->drives[i].Monitor);
            UsnJournalMonitor_Destroy(engine->drives[i].Monitor);
            NtfsIndex_Destroy(engine->drives[i].Index);
            
            // Shift down remaining
            for (int k = i; k < engine->drivesCount - 1; k++) {
                engine->drives[k] = engine->drives[k + 1];
            }
            engine->drivesCount--;
        }
    }

    // 3. Spawns threads to index new active drives
    for (int k = 0; k < activeDrivesCount; k++) {
        wchar_t driveLetter = activeDrives[k];
        bool alreadyIndexed = false;
        for (int i = 0; i < engine->drivesCount; i++) {
            if (engine->drives[i].Index->driveLetter == driveLetter) {
                alreadyIndexed = true;
                break;
            }
        }

        if (alreadyIndexed) {
            continue;
        }

        if (engine->drivesCount < MAX_INDEXED_DRIVES) {
            int idx = engine->drivesCount;
            engine->drives[idx].Index = NtfsIndex_Create(driveLetter);
            
            if (engine->notifyWindow) {
                // Register notify target window
                engine->drives[idx].Index->isIndexed = false; // reset state
            }
            
            engine->drives[idx].Monitor = UsnJournalMonitor_Create(engine->drives[idx].Index);
            if (engine->notifyWindow) {
                UsnJournalMonitor_RegisterNotifyWindow(engine->drives[idx].Monitor, engine->notifyWindow);
            }
            
            engine->drivesCount++;

            IndexerThreadArgs* args = (IndexerThreadArgs*)malloc(sizeof(IndexerThreadArgs));
            if (args) {
                args->engine = engine;
                args->driveLetter = driveLetter;
                args->index = engine->drives[idx].Index;
                args->monitor = engine->drives[idx].Monitor;
                args->uiCallback = callback;

                HANDLE hThread = (HANDLE)_beginthreadex(
                    NULL,
                    0,
                    IndexerThreadProc,
                    args,
                    0,
                    NULL
                );
                if (hThread) {
                    CloseHandle(hThread); // Detach immediately
                } else {
                    free(args);
                }
            }
        }
    }

    ReleaseSRWLockExclusive(&engine->drivesLock);
}

bool SearchEngine_ExecuteSearch(SearchEngine* engine, const StringMatcher* matcher, wchar_t driveFilter, 
                                 FilterType filter, SearchResultList* outResults, volatile LONG* pCancelFlag) {
    AcquireSRWLockShared(&engine->drivesLock);

    if (pCancelFlag && InterlockedOr(pCancelFlag, 0)) {
        ReleaseSRWLockShared(&engine->drivesLock);
        return false;
    }

    // Accumulate results in a temporary buffer (double-buffering)
    SearchResultList tempResults;
    DYNARRAY_INIT(tempResults);

    for (int d = 0; d < engine->drivesCount; d++) {
        NtfsIndex* pIndex = engine->drives[d].Index;
        wchar_t letter = pIndex->driveLetter;

        if (driveFilter != L'\0' && towlower(driveFilter) != towlower(letter)) {
            continue;
        }

        NtfsIndex_LockShared(pIndex);

        size_t count = pIndex->activeCount;
        FileRecord** records = pIndex->activeRecords;

        if (records) {
            for (size_t i = 0; i < count; i++) {
                // Cooperative cancel check every 4096 records to keep preemption extremely responsive
                if ((i & 4095) == 0 && pCancelFlag && InterlockedOr(pCancelFlag, 0)) {
                    DYNARRAY_FREE(tempResults);
                    NtfsIndex_UnlockShared(pIndex);
                    ReleaseSRWLockShared(&engine->drivesLock);
                    return false;
                }
                const FileRecord* item = records[i];

                if (!item || !item->Name || item->Name[0] == L'\0' || item->ParentFrs == 0xFFFFFFFF) {
                    continue;
                }

                bool isDir = (item->Attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;

                if (!FilePassesFilter(item->Name, isDir, filter)) {
                    continue;
                }

                bool matched = false;
                if (engine->matchPath) {
                    // Match Path: Resolve full canonical path in zero-allocations stack buffer in O(1)
                    wchar_t fullPath[4096];
                    NtfsIndex_ResolveFullPathToBuf(pIndex, item, fullPath, 4096);
                    matched = StringMatcher_Matches(matcher, fullPath);
                } else {
                    matched = StringMatcher_Matches(matcher, item->Name);
                }

                if (matched) {
                    SearchResult res;
                    res.Record = item;
                    res.Drive = letter;
                    DYNARRAY_ADD(tempResults, res);
                }
            }
        }

        NtfsIndex_UnlockShared(pIndex);
    }

    // Success! Swap tempResults into outResults
    DYNARRAY_FREE(*outResults);
    outResults->data = tempResults.data;
    outResults->count = tempResults.count;
    outResults->capacity = tempResults.capacity;

    ReleaseSRWLockShared(&engine->drivesLock);
    return true;
}

size_t SearchEngine_GetResultFullPath(const SearchEngine* engine, const SearchResult* result, wchar_t* outBuf, size_t maxChars) {
    size_t written = 0;
    
    // Acquire SRWLock shared for drive list safety
    AcquireSRWLockShared((PSRWLOCK)&engine->drivesLock);
    for (int i = 0; i < engine->drivesCount; i++) {
        if (engine->drives[i].Index->driveLetter == result->Drive) {
            NtfsIndex_LockShared(engine->drives[i].Index);
            written = NtfsIndex_ResolveFullPathFromParent(engine->drives[i].Index, result->Record->ParentFrs, result->Record->Name, outBuf, maxChars);
            NtfsIndex_UnlockShared(engine->drives[i].Index);
            break;
        }
    }
    ReleaseSRWLockShared((PSRWLOCK)&engine->drivesLock);

    return written;
}

void SearchEngine_LockDrivesShared(const SearchEngine* engine) {
    AcquireSRWLockShared((PSRWLOCK)&engine->drivesLock);
    for (int i = 0; i < engine->drivesCount; i++) {
        NtfsIndex_LockShared(engine->drives[i].Index);
    }
    ReleaseSRWLockShared((PSRWLOCK)&engine->drivesLock);
}

void SearchEngine_UnlockDrivesShared(const SearchEngine* engine) {
    AcquireSRWLockShared((PSRWLOCK)&engine->drivesLock);
    for (int i = 0; i < engine->drivesCount; i++) {
        NtfsIndex_UnlockShared(engine->drives[i].Index);
    }
    ReleaseSRWLockShared((PSRWLOCK)&engine->drivesLock);
}

const FileRecord* SearchEngine_GetRecordUnsafe(const SearchEngine* engine, wchar_t driveLetter, unsigned int recordIndex) {
    for (int i = 0; i < engine->drivesCount; i++) {
        if (engine->drives[i].Index->driveLetter == driveLetter) {
            if (recordIndex < engine->drives[i].Index->recordsCount) {
                return NtfsIndex_GetRecord(engine->drives[i].Index, recordIndex);
            }
            break;
        }
    }
    return NULL;
}

void SearchEngine_RemoveDrive(SearchEngine* engine, wchar_t driveLetter) {
    AcquireSRWLockExclusive(&engine->drivesLock);
    for (int i = 0; i < engine->drivesCount; i++) {
        if (engine->drives[i].Index->driveLetter == driveLetter) {
            UsnJournalMonitor_Stop(engine->drives[i].Monitor);
            UsnJournalMonitor_Destroy(engine->drives[i].Monitor);
            NtfsIndex_Destroy(engine->drives[i].Index);

            for (int k = i; k < engine->drivesCount - 1; k++) {
                engine->drives[k] = engine->drives[k + 1];
            }
            engine->drivesCount--;
            break;
        }
    }
    ReleaseSRWLockExclusive(&engine->drivesLock);
}

size_t SearchEngine_GetTotalIndexedFiles(const SearchEngine* engine) {
    AcquireSRWLockShared((PSRWLOCK)&engine->drivesLock);
    size_t total = 0;
    for (int i = 0; i < engine->drivesCount; i++) {
        total += engine->drives[i].Index->totalFiles;
        total += engine->drives[i].Index->totalFolders;
    }
    ReleaseSRWLockShared((PSRWLOCK)&engine->drivesLock);
    return total;
}

int SearchEngine_GetIndexedDrives(const SearchEngine* engine, wchar_t* outDrives, int maxDrives) {
    AcquireSRWLockShared((PSRWLOCK)&engine->drivesLock);
    int count = engine->drivesCount < maxDrives ? engine->drivesCount : maxDrives;
    for (int i = 0; i < count; i++) {
        outDrives[i] = engine->drives[i].Index->driveLetter;
    }
    ReleaseSRWLockShared((PSRWLOCK)&engine->drivesLock);
    return count;
}
