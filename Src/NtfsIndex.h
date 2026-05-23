#pragma once
#include "stdafx.h"
#include "NtfsStructs.h"

namespace Ntfs {

    // A lightweight 8-byte string class replacing std::wstring (40 bytes) to save index memory.
    class CompactString {
    public:
        CompactString() : m_str(nullptr) {}
        ~CompactString() { delete[] m_str; }

        CompactString(const CompactString&) = delete;
        CompactString& operator=(const CompactString&) = delete;

        CompactString(CompactString&& other) noexcept : m_str(other.m_str) {
            other.m_str = nullptr;
        }

        CompactString& operator=(CompactString&& other) noexcept {
            if (this != &other) {
                delete[] m_str;
                m_str = other.m_str;
                other.m_str = nullptr;
            }
            return *this;
        }

        CompactString& operator=(const std::wstring& s) {
            delete[] m_str;
            if (!s.empty()) {
                size_t len = s.length();
                m_str = new wchar_t[len + 1];
                wcscpy_s(m_str, len + 1, s.c_str());
            } else {
                m_str = nullptr;
            }
            return *this;
        }

        CompactString& operator=(const wchar_t* s) {
            delete[] m_str;
            if (s && s[0] != L'\0') {
                size_t len = wcslen(s);
                m_str = new wchar_t[len + 1];
                wcscpy_s(m_str, len + 1, s);
            } else {
                m_str = nullptr;
            }
            return *this;
        }

        const wchar_t* c_str() const { return m_str ? m_str : L""; }
        size_t size() const { return m_str ? wcslen(m_str) : 0; }
        size_t length() const { return size(); }
        bool empty() const { return !m_str || m_str[0] == L'\0'; }

        wchar_t operator[](size_t index) const {
            return m_str ? m_str[index] : L'\0';
        }

        // Iterator support
        const wchar_t* begin() const { return c_str(); }
        const wchar_t* end() const { return c_str() + length(); }

        // Conversion to std::wstring
        operator std::wstring() const { return c_str(); }

        // Comparison operators
        bool operator==(const CompactString& other) const { return wcscmp(c_str(), other.c_str()) == 0; }
        bool operator!=(const CompactString& other) const { return wcscmp(c_str(), other.c_str()) != 0; }
        bool operator<(const CompactString& other) const { return wcscmp(c_str(), other.c_str()) < 0; }

        bool operator==(const std::wstring& other) const { return wcscmp(c_str(), other.c_str()) == 0; }
        bool operator!=(const std::wstring& other) const { return wcscmp(c_str(), other.c_str()) != 0; }

        bool operator==(const wchar_t* other) const { return wcscmp(c_str(), other ? other : L"") == 0; }
        bool operator!=(const wchar_t* other) const { return wcscmp(c_str(), other ? other : L"") != 0; }

        std::wstring operator+(const wchar_t* other) const {
            return std::wstring(c_str()) + (other ? other : L"");
        }
        std::wstring operator+(const std::wstring& other) const {
            return std::wstring(c_str()) + other;
        }

    private:
        wchar_t* m_str;
    };

    // Represents a single file or directory record stored in the memory index.
    // Specifying fields with compact alignment.
    struct FileRecord {
        CompactString Name;
        unsigned int ParentFrs = 0xFFFFFFFF;
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
        const std::vector<std::unique_ptr<FileRecord>>& GetRecordsInternal() const { return m_records; }

    private:
        wchar_t m_driveLetter;
        std::vector<std::unique_ptr<FileRecord>> m_records; // flat contiguous array indexed by FRS number for O(1) accesses

        std::atomic<bool> m_isIndexed;
        std::atomic<unsigned int> m_totalFiles;
        std::atomic<unsigned int> m_totalFolders;

        mutable std::shared_mutex m_lock; // Shared/Exclusive lock for concurrent search vs real-time USN modifications
        HWND m_notifyWindow = nullptr;
    };

} // namespace Ntfs
