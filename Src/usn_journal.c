#include "usn_journal.h"
#include "ntfs_index.h"
#include <winioctl.h>

#define WM_NTFS_INDEX_CHANGED (WM_USER + 102)

UsnJournalMonitor* UsnJournalMonitor_Create(NtfsIndex* index) {
    UsnJournalMonitor* monitor = (UsnJournalMonitor*)malloc(sizeof(UsnJournalMonitor));
    if (!monitor) return NULL;

    monitor->m_index = index;
    monitor->m_running = 0;
    monitor->m_monitorThread = NULL;
    monitor->m_volumeHandle = INVALID_HANDLE_VALUE;
    monitor->m_nextUsn = 0;
    monitor->m_journalId = 0;
    monitor->m_notifyWindow = NULL;

    return monitor;
}

void UsnJournalMonitor_Destroy(UsnJournalMonitor* monitor) {
    if (!monitor) return;
    UsnJournalMonitor_Stop(monitor);
    free(monitor);
}

void UsnJournalMonitor_RegisterNotifyWindow(UsnJournalMonitor* monitor, HWND hwnd) {
    monitor->m_notifyWindow = hwnd;
}

// Windows USN Record structure definition
#pragma pack(push, 1)
typedef struct {
    unsigned long RecordLength;
    unsigned short MajorVersion;
    unsigned short MinorVersion;
    unsigned long long FileReferenceNumber;
    unsigned long long ParentFileReferenceNumber;
    USN Usn;
    long long TimeStamp;
    unsigned long Reason;
    unsigned long SourceInfo;
    unsigned long SecurityId;
    unsigned long FileAttributes;
    unsigned short FileNameLength;
    unsigned short FileNameOffset;
    WCHAR FileName[1];
} USN_REC_V2;
#pragma pack(pop)

static unsigned int __stdcall MonitorThreadProc(void* arg) {
    UsnJournalMonitor* pThis = (UsnJournalMonitor*)arg;

    wchar_t volPath[64];
    swprintf_s(volPath, 64, L"\\\\.\\%c:", pThis->m_index->driveLetter);

    // 1. Open volume handle with write access to query/create journal if needed
    HANDLE hInitVol = CreateFileW(
        volPath,
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL,
        OPEN_EXISTING,
        0,
        NULL
    );

    // Fallback to read-only access if write access is denied
    if (hInitVol == INVALID_HANDLE_VALUE) {
        hInitVol = CreateFileW(
            volPath,
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            NULL,
            OPEN_EXISTING,
            0,
            NULL
        );
    }

    if (hInitVol == INVALID_HANDLE_VALUE) {
        return 0;
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
        cujd.MaximumSize = 0;      // System default max size
        cujd.AllocationDelta = 0;  // System default allocation delta
        
        DeviceIoControl(
            hInitVol,
            FSCTL_CREATE_USN_JOURNAL,
            &cujd, sizeof(cujd),
            NULL, 0,
            &cbReturned,
            NULL
        );

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
        return 0;
    }

    pThis->m_nextUsn = ujd.NextUsn;
    pThis->m_journalId = ujd.UsnJournalID;

    // Close initial handle immediately so drive is not locked
    CloseHandle(hInitVol);

    // 3. Monitor Loop
    READ_USN_JOURNAL_DATA_V0 readData = { 0 };
    readData.StartUsn = pThis->m_nextUsn;
    readData.ReasonMask = 0xFFFFFFFF;
    readData.ReturnOnlyOnClose = FALSE;
    readData.Timeout = 0;
    readData.BytesToWaitFor = 0;
    readData.UsnJournalID = pThis->m_journalId;

    unsigned char* usnBuffer = (unsigned char*)malloc(8192);
    if (!usnBuffer) return 0;

    while (InterlockedOr(&pThis->m_running, 0)) {
        // Open volume handle on-demand for this poll
        HANDLE hVol = CreateFileW(
            volPath,
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            NULL,
            OPEN_EXISTING,
            0,
            NULL
        );

        if (hVol == INVALID_HANDLE_VALUE) {
            Sleep(1000);
            continue;
        }

        DWORD cb = 0;
        BOOL success = DeviceIoControl(
            hVol,
            FSCTL_READ_USN_JOURNAL,
            &readData, sizeof(readData),
            usnBuffer, 8192,
            &cb,
            NULL
        );

        CloseHandle(hVol);

        if (!success) {
            DWORD err = GetLastError();
            if (err == ERROR_JOURNAL_ENTRY_DELETED || err == ERROR_INVALID_PARAMETER) {
                // Recover from journal roll-over or clearance by re-querying the current position
                HANDLE hRecov = CreateFileW(volPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
                if (hRecov != INVALID_HANDLE_VALUE) {
                    USN_JOURNAL_DATA_V0 recovUjd = { 0 };
                    DWORD recovCb = 0;
                    if (DeviceIoControl(hRecov, FSCTL_QUERY_USN_JOURNAL, NULL, 0, &recovUjd, sizeof(recovUjd), &recovCb, NULL)) {
                        pThis->m_nextUsn = recovUjd.NextUsn;
                        pThis->m_journalId = recovUjd.UsnJournalID;
                        readData.StartUsn = pThis->m_nextUsn;
                        readData.UsnJournalID = pThis->m_journalId;
                    }
                    CloseHandle(hRecov);
                }
            }
            Sleep(1000);
            continue;
        }

        if (cb < sizeof(USN)) {
            Sleep(1000);
            continue;
        }

        // Store NextUsn starting pointer for subsequent query
        pThis->m_nextUsn = *(USN*)usnBuffer;
        readData.StartUsn = pThis->m_nextUsn;

        // Parse returned record list starting after the 8-byte NextUsn header
        unsigned char* pRecord = usnBuffer + sizeof(USN);
        unsigned char* pEnd = usnBuffer + cb;
        bool processedAny = false;

        while (pRecord < pEnd) {
            USN_REC_V2* record = (USN_REC_V2*)pRecord;

            if (record->RecordLength == 0 || pRecord + record->RecordLength > pEnd) {
                break;
            }

            processedAny = true;

            unsigned int frs = (unsigned int)(record->FileReferenceNumber & 0x0000FFFFFFFFFFFFLL);
            unsigned int parentFrs = (unsigned int)(record->ParentFileReferenceNumber & 0x0000FFFFFFFFFFFFLL);
            
            wchar_t name[260];
            int nameLen = record->FileNameLength / sizeof(wchar_t);
            if (nameLen >= 260) nameLen = 259;
            wcsncpy_s(name, 260, (wchar_t*)((char*)record + record->FileNameOffset), nameLen);
            name[nameLen] = L'\0';

            bool isDir = (record->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;

            if (record->Reason & USN_REASON_FILE_DELETE) {
                NtfsIndex_DeleteRecord(pThis->m_index, frs);
            }
            else if (record->Reason & (USN_REASON_FILE_CREATE | USN_REASON_RENAME_NEW_NAME | 
                                       USN_REASON_DATA_EXTEND | USN_REASON_DATA_OVERWRITE | 
                                       USN_REASON_BASIC_INFO_CHANGE)) {
                unsigned long long size = 0;
                unsigned long long sizeOnDisk = 0;
                unsigned long long dateModified = 0;
                unsigned long long dateCreated = 0;
                unsigned long long dateAccessed = 0;

                // Zero-allocation parent path query
                wchar_t parentPath[MAX_PATH];
                NtfsIndex_ResolveFullPathToBufByFrs(pThis->m_index, parentFrs, parentPath, MAX_PATH);

                wchar_t fullPath[MAX_PATH];
                if (wcslen(parentPath) > 0 && parentPath[wcslen(parentPath) - 1] == L'\\') {
                    swprintf_s(fullPath, MAX_PATH, L"%s%s", parentPath, name);
                } else {
                    swprintf_s(fullPath, MAX_PATH, L"%s\\%s", parentPath, name);
                }

                WIN32_FILE_ATTRIBUTE_DATA fad;
                if (GetFileAttributesExW(fullPath, GetFileExInfoStandard, &fad)) {
                    size = ((unsigned long long)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
                    sizeOnDisk = size;

                    dateCreated = ((unsigned long long)fad.ftCreationTime.dwHighDateTime << 32) | fad.ftCreationTime.dwLowDateTime;
                    dateModified = ((unsigned long long)fad.ftLastWriteTime.dwHighDateTime << 32) | fad.ftLastWriteTime.dwLowDateTime;
                    dateAccessed = ((unsigned long long)fad.ftLastAccessTime.dwHighDateTime << 32) | fad.ftLastAccessTime.dwLowDateTime;
                }

                NtfsIndex_AddOrUpdateRecord(
                    pThis->m_index, frs, name, parentFrs, size, sizeOnDisk,
                    dateCreated, dateModified, dateAccessed,
                    record->FileAttributes, isDir
                );
            }

            pRecord += record->RecordLength;
        }

        if (processedAny) {
            // Post notification to target window if index changed
            if (pThis->m_notifyWindow && IsWindow(pThis->m_notifyWindow)) {
                PostMessageW(pThis->m_notifyWindow, WM_NTFS_INDEX_CHANGED, 0, 0);
            }
        } else {
            Sleep(1000);
        }
    }

    free(usnBuffer);
    return 0;
}

bool UsnJournalMonitor_Start(UsnJournalMonitor* monitor) {
    UsnJournalMonitor_Stop(monitor);

    InterlockedExchange(&monitor->m_running, 1);
    monitor->m_monitorThread = (HANDLE)_beginthreadex(
        NULL,
        0,
        MonitorThreadProc,
        monitor,
        0,
        NULL
    );

    return monitor->m_monitorThread != NULL;
}

void UsnJournalMonitor_Stop(UsnJournalMonitor* monitor) {
    InterlockedExchange(&monitor->m_running, 0);
    if (monitor->m_monitorThread) {
        WaitForSingleObject(monitor->m_monitorThread, INFINITE);
        CloseHandle(monitor->m_monitorThread);
        monitor->m_monitorThread = NULL;
    }
    if (monitor->m_volumeHandle != INVALID_HANDLE_VALUE) {
        CloseHandle(monitor->m_volumeHandle);
        monitor->m_volumeHandle = INVALID_HANDLE_VALUE;
    }
}
