#include "stdafx.h"
#include "UsnJournal.h"
#include <dbt.h>

namespace Ntfs {

    UsnJournalMonitor::UsnJournalMonitor(NtfsIndex* index)
        : m_index(index), m_running(false), m_volumeHandle(INVALID_HANDLE_VALUE), m_nextUsn(0), m_journalId(0) {
    }

    UsnJournalMonitor::~UsnJournalMonitor() {
        Stop();
    }

    bool UsnJournalMonitor::Start() {
        Stop(); // Ensure any existing thread is cleaned up

        m_running = true;
        m_monitorThread = std::thread(MonitorThreadProc, this);
        return true;
    }

    void UsnJournalMonitor::Stop() {
        m_running = false;
        if (m_monitorThread.joinable()) {
            m_monitorThread.join();
        }
        if (m_volumeHandle != INVALID_HANDLE_VALUE) {
            CloseHandle(m_volumeHandle);
            m_volumeHandle = INVALID_HANDLE_VALUE;
        }
    }

    void UsnJournalMonitor::MonitorThreadProc(UsnJournalMonitor* pThis) {
        std::wstring volPath = pThis->m_index->GetVolumePath();

        // 1. Open volume handle with write access to query/create journal if needed.
        // Requires Administrator elevation.
        HANDLE hInitVol = CreateFileW(
            volPath.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            NULL,
            OPEN_EXISTING,
            0,
            NULL
        );

        // Fallback to read-only access if write access is denied (e.g. system locks)
        if (hInitVol == INVALID_HANDLE_VALUE) {
            hInitVol = CreateFileW(
                volPath.c_str(),
                GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                NULL,
                OPEN_EXISTING,
                0,
                NULL
            );
        }

        if (hInitVol == INVALID_HANDLE_VALUE) {
            return; // Exit thread if raw volume open fails
        }

        // 2. Query active USN Journal status
        USN_JOURNAL_DATA_V0 ujd = { 0 };
        DWORD cbReturned = 0;
        BOOL ok = DeviceIoControl(
            hInitVol,
            FSCTL_QUERY_USN_JOURNAL,
            NULL, 0,
            &ujd, sizeof(ujd),
            &cbReturned,
            NULL
        );

        // Try to activate/create USN Journal if it is not active
        if (!ok) {
            CREATE_USN_JOURNAL_DATA cujd = { 0 };
            cujd.MaximumSize = 0;      // System default maximum journal size
            cujd.AllocationDelta = 0;  // System default allocation increment
            
            DeviceIoControl(
                hInitVol,
                FSCTL_CREATE_USN_JOURNAL,
                &cujd, sizeof(cujd),
                NULL, 0,
                &cbReturned,
                NULL
            );

            // Re-query journal status after creation attempt
            ok = DeviceIoControl(
                hInitVol,
                FSCTL_QUERY_USN_JOURNAL,
                NULL, 0,
                &ujd, sizeof(ujd),
                &cbReturned,
                NULL
            );
        }

        if (!ok) {
            CloseHandle(hInitVol);
            return; // Exit if USN Journal query fails
        }

        // Store NextUsn starting pointer.
        // We only want to monitor new filesystem changes occurring *after* index build.
        pThis->m_nextUsn = ujd.NextUsn;
        pThis->m_journalId = ujd.UsnJournalID;

        // Close initial handle immediately so drive is not locked!
        CloseHandle(hInitVol);

        // 3. Monitor Loop
        READ_USN_JOURNAL_DATA_V0 readData = { 0 };
        readData.StartUsn = pThis->m_nextUsn;
        readData.ReasonMask = 0xFFFFFFFF; // Listen to all event categories
        readData.ReturnOnlyOnClose = FALSE;
        readData.Timeout = 0;
        readData.BytesToWaitFor = 0;
        readData.UsnJournalID = pThis->m_journalId;

        // 8KB aligned buffer for holding batches of incoming USN records
        std::vector<unsigned char> usnBuffer(8192);

        while (pThis->m_running) {
            // Open volume handle on-demand for this poll
            HANDLE hVol = CreateFileW(
                volPath.c_str(),
                GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                NULL,
                OPEN_EXISTING,
                0,
                NULL
            );

            if (hVol == INVALID_HANDLE_VALUE) {
                // If the drive is currently locked/ejecting or disconnected, sleep and retry
                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                continue;
            }

            DWORD cb = 0;
            BOOL success = DeviceIoControl(
                hVol,
                FSCTL_READ_USN_JOURNAL,
                &readData, sizeof(readData),
                usnBuffer.data(), static_cast<DWORD>(usnBuffer.size()),
                &cb,
                NULL
            );

            // Close handle immediately after read call!
            CloseHandle(hVol);

            if (!success || cb < sizeof(USN)) {
                // Sleep to avoid thrashing if IOControl encounters transient error or lock
                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                continue;
            }

            // The first 8 bytes of the output buffer contains the NextUsn starting pointer
            // for our subsequent query.
            pThis->m_nextUsn = *reinterpret_cast<USN*>(usnBuffer.data());
            readData.StartUsn = pThis->m_nextUsn;

            // Parse returned record list starting after the 8-byte NextUsn header
            unsigned char* pRecord = usnBuffer.data() + sizeof(USN);
            unsigned char* pEnd = usnBuffer.data() + cb;
            bool processedAny = false;

            while (pRecord < pEnd) {
                USN_RECORD_V2* record = reinterpret_cast<USN_RECORD_V2*>(pRecord);

                if (record->RecordLength == 0 || pRecord + record->RecordLength > pEnd) {
                    break;
                }

                processedAny = true;

                // Extract FRS file reference index (low 48 bits of the reference ID)
                unsigned int frs = static_cast<unsigned int>(record->FileReferenceNumber & 0x0000FFFFFFFFFFFFLL);
                unsigned int parentFrs = static_cast<unsigned int>(record->ParentFileReferenceNumber & 0x0000FFFFFFFFFFFFLL);
                
                // Extract UTF-16 Unicode file name from the USN record buffer
                std::wstring name(
                    reinterpret_cast<wchar_t*>(reinterpret_cast<char*>(record) + record->FileNameOffset),
                    record->FileNameLength / sizeof(wchar_t)
                );

                bool isDir = (record->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;

                // 1. File Deletions
                if (record->Reason & USN_REASON_FILE_DELETE) {
                    pThis->m_index->DeleteRecord(frs);
                }
                // 2. Additions / Rename / Attributes & Sizes Update
                else if (record->Reason & (USN_REASON_FILE_CREATE | USN_REASON_RENAME_NEW_NAME | 
                                           USN_REASON_DATA_EXTEND | USN_REASON_DATA_OVERWRITE | 
                                           USN_REASON_BASIC_INFO_CHANGE)) {
                    unsigned long long size = 0;
                    unsigned long long sizeOnDisk = 0;
                    unsigned long long dateModified = 0;
                    unsigned long long dateCreated = 0;
                    unsigned long long dateAccessed = 0;

                    // Reconstruct full path to query current file sizes/times using standard Win32 APIs
                    std::wstring fullPath = pThis->m_index->ResolveFullPath(parentFrs);
                    if (fullPath.back() != L'\\') {
                        fullPath += L"\\";
                    }
                    fullPath += name;

                    WIN32_FILE_ATTRIBUTE_DATA fad;
                    if (GetFileAttributesExW(fullPath.c_str(), GetFileExInfoStandard, &fad)) {
                        size = ((unsigned long long)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
                        sizeOnDisk = size; // Approximate allocated size as file size

                        dateCreated = ((unsigned long long)fad.ftCreationTime.dwHighDateTime << 32) | fad.ftCreationTime.dwLowDateTime;
                        dateModified = ((unsigned long long)fad.ftLastWriteTime.dwHighDateTime << 32) | fad.ftLastWriteTime.dwLowDateTime;
                        dateAccessed = ((unsigned long long)fad.ftLastAccessTime.dwHighDateTime << 32) | fad.ftLastAccessTime.dwLowDateTime;
                    }

                    pThis->m_index->AddOrUpdateRecord(
                        frs, name, parentFrs, size, sizeOnDisk,
                        dateModified, dateCreated, dateAccessed,
                        record->FileAttributes, isDir
                    );
                }

                // Advance to the next USN record in this batch
                pRecord += record->RecordLength;
            }

            if (processedAny) {
                pThis->m_index->NotifyIndexChanged();
            } else {
                // Idle debounce: if no updates were processed, sleep to keep CPU idle at 0%
                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            }
        }
    }

} // namespace Ntfs
