#pragma once
#include "stdafx.h"
#include "NtfsIndex.h"
#include "UsnJournal.h"
#include "StringMatcher.h"

namespace Search {

    // Compact representational search result structure.
    // Stores lightweight references and indices to avoid deep copying of string names.
    struct SearchResult {
        unsigned int RecordIndex;
        wchar_t Drive;
    };

    // Filter index matches for quick selections (Everything-like filters)
    enum class FilterType {
        All = 0,
        Folders,
        Files,
        Documents,
        Executables,
        Pictures,
        Audio,
        Video
    };

    // Callback for index loading progress updates
    class IIndexProgressCallback {
    public:
        // drive: drive letter being indexed
        // current: FRS read count
        // total: expected MFT record count
        virtual void OnIndexProgress(wchar_t drive, unsigned int current, unsigned int total) = 0;
        virtual void OnIndexComplete(wchar_t drive, bool success, unsigned int fileCount, unsigned int folderCount) = 0;
    };

    // class SearchEngine
    // Coordinates MFT indexes and real-time USN monitors across all active NTFS drives.
    // Handles multi-drive query execution and integrates the Everything-style category filters.
    class SearchEngine {
    public:
        SearchEngine();
        ~SearchEngine();

        // Detects all local fixed NTFS drives and initializes indexes
        void InitializeDrives(IIndexProgressCallback* callback);

        // Runs a query across all indexed volumes.
        // query: text search string
        // mode: plain, wildcard, or regex
        // driveFilter: L'\0' for all, or specific drive letter (e.g. L'C')
        // filter: Everything quick filter index
        // outResults: output search result vector
        void ExecuteSearch(const std::wstring& query, MatchMode mode, wchar_t driveFilter, 
                           FilterType filter, std::vector<SearchResult>& outResults);

        // Resolves full path for a search result
        std::wstring GetResultFullPath(const SearchResult& result) const;

        // Thread-safe drive lock helpers
        void LockDrivesShared() const;
        void UnlockDrivesShared() const;

        // Unsafe raw record retrieval (MUST call LockDrivesShared first!)
        const Ntfs::FileRecord* GetRecordUnsafe(wchar_t driveLetter, unsigned int recordIndex) const;

        // Safely stops and removes a drive index and monitor (used during ejection/removal)
        void RemoveDrive(wchar_t driveLetter);
        void RemoveDriveByHandle(HANDLE hVolume);

        // Retrieves the active query matcher for custom drawing highlights
        const StringMatcher& GetLastMatcher() const { return m_lastMatcher; }

        // Drive information query
        size_t GetTotalIndexedFiles() const;
        std::vector<wchar_t> GetIndexedDrives() const;

        // UI notification support
        void RegisterNotifyWindow(HWND hwnd);

    private:
        struct DriveContext {
            std::unique_ptr<Ntfs::NtfsIndex> Index;
            std::unique_ptr<Ntfs::UsnJournalMonitor> Monitor;
        };

        std::vector<DriveContext> m_drives;
        mutable std::mutex m_searchMutex; // protects drive context lists
        StringMatcher m_lastMatcher; // caches active search pattern matcher
        HWND m_notifyWindow = nullptr;

        // Helper to check if file record matches the selected category filter
        static bool FilePassesFilter(const std::wstring& filename, bool isDirectory, FilterType filter);
        static bool HasExtension(const std::wstring& filename, const std::vector<std::wstring>& extensions);
    };

} // namespace Search
