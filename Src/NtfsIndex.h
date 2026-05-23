#pragma once
#include "stdafx.h"
#include "NtfsStructs.h"

namespace Ntfs {

    // Represents a single file or directory record stored in the memory index.
    // Specifying fields with compact alignment.
    struct FileRecordName {
        std::wstring Name;
        unsigned int ParentFrs = 0xFFFFFFFF;
    };

    // Represents a single file or directory record stored in the memory index.
    // Specifying fields with compact alignment.
    struct FileRecord {
        std::vector<FileRecordName> Names;
        unsigned long long Size = 0;
        unsigned long long SizeOnDisk = 0;
        unsigned long long DateModified = 0;
        unsigned long long DateCreated = 0;
        unsigned long long DateAccessed = 0;
        unsigned int Attributes = 0;
        bool IsDirectory = false;
    };

    // class NtfsIndex
    // A thread-safe, high-performance in-memory index representing all active files
    // and folders on an NTFS partition.
    class NtfsIndex {
    public:
        NtfsIndex(wchar_t driveLetter);
        ~NtfsIndex();

        // Accessors
        wchar_t GetDriveLetter() const { return m_driveLetter; }
        std::wstring GetVolumePath() const;
        bool IsIndexed() const { return m_isIndexed; }
        unsigned int GetTotalFileCount() const { return m_totalFiles; }
        unsigned int GetTotalFolderCount() const { return m_totalFolders; }

        // Core Indexer methods
        // Parses the raw MFT buffer in a single thread or multithreaded pass
        bool BuildIndex(const std::vector<unsigned char>& rawBuffer, unsigned int recordSize);

        // Path resolution: traces parent chain upwards in O(depth) (extremely fast, usually depth < 10)
        std::wstring ResolveFullPath(unsigned int recordIndex) const;
        std::wstring ResolveFullPath(unsigned int parentFrs, const std::wstring& name) const;

        // Incremental modifications (used by the USN Journal Monitor for real-time tracking)
        void AddOrUpdateRecord(unsigned int recordIndex, const std::wstring& name, unsigned int parentFrs, 
                              unsigned long long size, unsigned long long sizeOnDisk, 
                              unsigned long long dateModified, unsigned long long dateCreated, 
                              unsigned long long dateAccessed, unsigned int attributes, bool isDirectory);
        void DeleteRecord(unsigned int recordIndex);

        // UI change notification support
        void RegisterNotifyWindow(HWND hwnd) { m_notifyWindow = hwnd; }
        void NotifyIndexChanged();

        // Thread-safe access locking
        void LockShared() const { m_lock.lock_shared(); }
        void UnlockShared() const { m_lock.unlock_shared(); }
        void LockExclusive() { m_lock.lock(); }
        void UnlockExclusive() { m_lock.unlock(); }

        // Reference direct accessor (must lock index shared before iterating!)
        const std::vector<FileRecord>& GetRecordsInternal() const { return m_records; }

    private:
        wchar_t m_driveLetter;
        std::vector<FileRecord> m_records; // flat contiguous array indexed by FRS number for O(1) accesses

        std::atomic<bool> m_isIndexed;
        std::atomic<unsigned int> m_totalFiles;
        std::atomic<unsigned int> m_totalFolders;

        mutable std::shared_mutex m_lock; // Shared/Exclusive lock for concurrent search vs real-time USN modifications
        HWND m_notifyWindow = nullptr;
    };

} // namespace Ntfs
