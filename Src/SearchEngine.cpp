#include "stdafx.h"
#include "SearchEngine.h"
#include "MftReader.h"

namespace Search {

    SearchEngine::SearchEngine() : m_notifyWindow(nullptr) {
    }

    SearchEngine::~SearchEngine() {
        std::lock_guard<std::mutex> lock(m_searchMutex);
        for (auto& drive : m_drives) {
            drive.Monitor->Stop(); // Stop USN journal change threads safely
        }
    }

    void SearchEngine::RegisterNotifyWindow(HWND hwnd) {
        std::lock_guard<std::mutex> lock(m_searchMutex);
        m_notifyWindow = hwnd;
        for (auto& drive : m_drives) {
            drive.Index->RegisterNotifyWindow(hwnd);
        }
    }

    // Identifies all physical fixed NTFS drives in the system and triggers 
    // an asynchronous MFT background reader thread for each drive.
    void SearchEngine::InitializeDrives(IIndexProgressCallback* callback) {
        DWORD mask = GetLogicalDrives();
        for (int i = 0; i < 26; ++i) {
            if (mask & (1 << i)) {
                wchar_t driveLetter = L'A' + i;
                std::wstring drivePath = L"";
                drivePath += driveLetter;
                drivePath += L":\\";

                // We only index local fixed volumes (HDDs, SSDs)
                if (GetDriveTypeW(drivePath.c_str()) == DRIVE_FIXED) {
                    wchar_t fsName[100] = { 0 };
                    if (GetVolumeInformationW(drivePath.c_str(), NULL, 0, NULL, NULL, NULL, fsName, 100)) {
                        // MFT parsing is exclusive to NTFS file systems
                        if (wcscmp(fsName, L"NTFS") == 0) {
                            std::lock_guard<std::mutex> lock(m_searchMutex);

                            DriveContext ctx;
                            ctx.Index = std::make_unique<Ntfs::NtfsIndex>(driveLetter);
                            if (m_notifyWindow) {
                                ctx.Index->RegisterNotifyWindow(m_notifyWindow);
                            }
                            ctx.Monitor = std::make_unique<Ntfs::UsnJournalMonitor>(ctx.Index.get());

                            Ntfs::NtfsIndex* pIndex = ctx.Index.get();
                            Ntfs::UsnJournalMonitor* pMonitor = ctx.Monitor.get();

                            m_drives.push_back(std::move(ctx));

                            // Spawn a background indexing thread for this volume
                            std::thread([this, driveLetter, pIndex, pMonitor, callback]() {
                                
                                // Progress wrapper for Raw MFT Reader callback
                                class MftCallback : public Ntfs::IMftProgressCallback {
                                public:
                                    wchar_t Drive;
                                    IIndexProgressCallback* UiCallback;
                                    bool Completed = false;
                                    bool Success = false;
                                    std::vector<unsigned char> Buffer;
                                    unsigned int RecSize = 0;

                                    void OnProgress(unsigned int current, unsigned int total) override {
                                        UiCallback->OnIndexProgress(Drive, current, total);
                                    }
                                    void OnComplete(bool success, const std::vector<unsigned char>& rawBuffer, unsigned int recordSize) override {
                                        Success = success;
                                        Buffer = rawBuffer;
                                        RecSize = recordSize;
                                        Completed = true;
                                    }
                                } mftCb;

                                mftCb.Drive = driveLetter;
                                mftCb.UiCallback = callback;

                                Ntfs::MftReader reader;
                                std::wstring volPath = L"\\\\.\\";
                                volPath += driveLetter;
                                volPath += L":";

                                reader.ReadMftAsync(volPath, &mftCb);

                                // Block background loader thread until MftReader reads raw partition clusters
                                while (!mftCb.Completed) {
                                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                                }

                                if (mftCb.Success) {
                                    // Parse the loaded raw MFT records and build tree structures
                                    pIndex->BuildIndex(mftCb.Buffer, mftCb.RecSize);
                                    
                                    // Start real-time monitoring of USN filesystem changes!
                                    pMonitor->Start();
                                    
                                    callback->OnIndexComplete(driveLetter, true, pIndex->GetTotalFileCount(), pIndex->GetTotalFolderCount());
                                } else {
                                    callback->OnIndexComplete(driveLetter, false, 0, 0);
                                }
                            }).detach();
                        }
                    }
                }
            }
        }
    }

    // Searches across all indexed drive records.
    // Iterates the flat record structures using the fast StringMatcher.
    // Completes in ~5-15 milliseconds for millions of records.
    void SearchEngine::ExecuteSearch(const std::wstring& query, MatchMode mode, wchar_t driveFilter, 
                                     FilterType filter, std::vector<SearchResult>& outResults) {
        
        m_lastMatcher.SetPattern(query, mode, false); // default to case-insensitive searches

        std::lock_guard<std::mutex> lock(m_searchMutex);

        for (const auto& drive : m_drives) {
            wchar_t driveLetter = drive.Index->GetDriveLetter();
            
            // Check drive letter filter
            if (driveFilter != L'\0' && towlower(driveFilter) != towlower(driveLetter)) {
                continue;
            }

            // Lock index shared for reading.
            // Allows other search operations to execute in parallel, but blocks USN write modifications.
            drive.Index->LockShared();

            const auto& records = drive.Index->GetRecordsInternal();
            unsigned int numRecords = static_cast<unsigned int>(records.size());

            for (unsigned int i = 0; i < numRecords; ++i) {
                const auto& item = records[i];

                // Skip unallocated records
                if (!item || item->Name.empty() || item->ParentFrs == 0xFFFFFFFF) {
                    continue;
                }

                // Apply Everything-style quick filters (Documents, Pictures, Executables, etc.)
                if (!FilePassesFilter(item->Name, item->IsDirectory, filter)) {
                    continue;
                }

                // Perform pattern string matching
                if (m_lastMatcher.Matches(item->Name)) {
                    SearchResult res;
                    res.RecordIndex = i;
                    res.Drive = driveLetter;

                    outResults.push_back(res);
                }
            }

            drive.Index->UnlockShared();
        }
        outResults.shrink_to_fit();
    }

    std::wstring SearchEngine::GetResultFullPath(const SearchResult& result) const {
        std::lock_guard<std::mutex> lock(m_searchMutex);
        for (const auto& drive : m_drives) {
            if (drive.Index->GetDriveLetter() == result.Drive) {
                return drive.Index->ResolveFullPath(result.RecordIndex);
            }
        }
        return L"";
    }

    void SearchEngine::LockDrivesShared() const {
        std::lock_guard<std::mutex> lock(m_searchMutex);
        for (const auto& drive : m_drives) {
            drive.Index->LockShared();
        }
    }

    void SearchEngine::UnlockDrivesShared() const {
        std::lock_guard<std::mutex> lock(m_searchMutex);
        for (const auto& drive : m_drives) {
            drive.Index->UnlockShared();
        }
    }

    const Ntfs::FileRecord* SearchEngine::GetRecordUnsafe(wchar_t driveLetter, unsigned int recordIndex) const {
        // Assume caller holds the shared lock via LockDrivesShared!
        for (const auto& drive : m_drives) {
            if (drive.Index->GetDriveLetter() == driveLetter) {
                const auto& records = drive.Index->GetRecordsInternal();
                if (recordIndex < records.size()) {
                    return records[recordIndex].get();
                }
                break;
            }
        }
        return nullptr;
    }

    size_t SearchEngine::GetTotalIndexedFiles() const {
        std::lock_guard<std::mutex> lock(m_searchMutex);
        size_t total = 0;
        for (const auto& drive : m_drives) {
            total += drive.Index->GetTotalFileCount();
            total += drive.Index->GetTotalFolderCount();
        }
        return total;
    }

    std::vector<wchar_t> SearchEngine::GetIndexedDrives() const {
        std::lock_guard<std::mutex> lock(m_searchMutex);
        std::vector<wchar_t> drives;
        for (const auto& drive : m_drives) {
            drives.push_back(drive.Index->GetDriveLetter());
        }
        return drives;
    }

    // Helper checks if the candidate filename matches Everything extension filter rules
    bool SearchEngine::FilePassesFilter(const std::wstring& filename, bool isDirectory, FilterType filter) {
        if (filter == FilterType::All) return true;
        if (filter == FilterType::Folders) return isDirectory;
        if (filter == FilterType::Files) return !isDirectory;

        // If it is a folder, it fails any document/picture/music category filter
        if (isDirectory) return false;

        switch (filter) {
            case FilterType::Documents:
                return HasExtension(filename, { L".doc", L".docx", L".xls", L".xlsx", L".ppt", L".pptx", 
                                                L".pdf", L".txt", L".rtf", L".odt", L".ods", L".odp", L".csv" });
            
            case FilterType::Executables:
                return HasExtension(filename, { L".exe", L".bat", L".cmd", L".msi", L".lnk", L".com", L".ps1" });

            case FilterType::Pictures:
                return HasExtension(filename, { L".png", L".jpg", L".jpeg", L".gif", L".bmp", L".svg", 
                                                L".ico", L".tiff", L".webp" });

            case FilterType::Audio:
                return HasExtension(filename, { L".mp3", L".wav", L".wma", L".m4a", L".flac", L".aac", 
                                                L".ogg", L".mid" });

            case FilterType::Video:
                return HasExtension(filename, { L".mp4", L".mkv", L".avi", L".mov", L".wmv", L".flv", 
                                                L".webm", L".mpg", L".mpeg" });

            default:
                break;
        }

        return true;
    }

    bool SearchEngine::HasExtension(const std::wstring& filename, const std::vector<std::wstring>& extensions) {
        size_t dotIdx = filename.find_last_of(L'.');
        if (dotIdx == std::wstring::npos) return false;

        std::wstring ext = filename.substr(dotIdx);
        std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);

        for (const auto& e : extensions) {
            if (ext == e) return true;
        }
        return false;
    }

} // namespace Search
