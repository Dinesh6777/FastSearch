#ifndef SEARCH_ENGINE_H
#define SEARCH_ENGINE_H

#include "fs_common.h"
#include "ntfs_index.h"
#include "usn_journal.h"
#include "string_matcher.h"

// Search result structure
typedef struct {
    wchar_t Name[260];
    unsigned int Frs;
    unsigned int ParentFrs;
    unsigned long long Size;
    unsigned long long SizeOnDisk;
    unsigned long long DateCreated;
    unsigned long long DateModified;
    unsigned long long DateAccessed;
    unsigned int Attributes;
    bool IsDirectory;
    wchar_t Drive;
} SearchResult;

// Dynamic array type definition for SearchResult using fs_dynarray
#include "fs_dynarray.h"
DYNARRAY_DECLARE(SearchResult, SearchResultList)

// Everything-style quick filters
typedef enum {
    FilterType_All = 0,
    FilterType_Folders,
    FilterType_Files,
    FilterType_Documents,
    FilterType_Executables,
    FilterType_Pictures,
    FilterType_Audio,
    FilterType_Video
} FilterType;

// Callback interface for reporting loading progress
typedef struct {
    void* Context;
    void (*OnIndexProgress)(void* context, wchar_t drive, unsigned int current, unsigned int total);
    void (*OnIndexComplete)(void* context, wchar_t drive, bool success, unsigned int fileCount, unsigned int folderCount);
} IIndexProgressCallback;

typedef struct {
    NtfsIndex* Index;
    UsnJournalMonitor* Monitor;
} DriveContext;

#define MAX_INDEXED_DRIVES 26

typedef struct {
    DriveContext drives[MAX_INDEXED_DRIVES];
    int drivesCount;
    SRWLOCK drivesLock;                    // Slim Reader/Writer Lock protecting the context lists
    StringMatcher lastMatcher;             // Caches active search patterns
    HWND notifyWindow;                     // Notification target window
    bool matchPath;                        // Toggles full path matching
} SearchEngine;

SearchEngine* SearchEngine_Create(void);
void SearchEngine_Destroy(SearchEngine* engine);

// Auto-discovery and async loader
void SearchEngine_InitializeDrives(SearchEngine* engine, IIndexProgressCallback callback);

// Search executor
bool SearchEngine_ExecuteSearch(SearchEngine* engine, const StringMatcher* matcher, wchar_t driveFilter, 
                                 FilterType filter, SearchResultList* outResults, volatile LONG* pCancelFlag);

// Zero-allocation full path resolver
size_t SearchEngine_GetResultFullPath(const SearchEngine* engine, const SearchResult* result, wchar_t* outBuf, size_t maxChars);

// Thread safety locking
void SearchEngine_LockDrivesShared(const SearchEngine* engine);
void SearchEngine_UnlockDrivesShared(const SearchEngine* engine);

// Unsafe raw record access (Caller MUST lock drives shared first!)
const FileRecord* SearchEngine_GetRecordUnsafe(const SearchEngine* engine, wchar_t driveLetter, unsigned int recordIndex);

void SearchEngine_RemoveDrive(SearchEngine* engine, wchar_t driveLetter);

size_t SearchEngine_GetTotalIndexedFiles(const SearchEngine* engine);
int SearchEngine_GetIndexedDrives(const SearchEngine* engine, wchar_t* outDrives, int maxDrives);

void SearchEngine_RegisterNotifyWindow(SearchEngine* engine, HWND hwnd);

#endif // SEARCH_ENGINE_H
