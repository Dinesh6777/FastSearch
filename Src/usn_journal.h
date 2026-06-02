#ifndef USN_JOURNAL_H
#define USN_JOURNAL_H

#include "fs_common.h"
#include "ntfs_index.h"

// Listens to filesystem change events using Windows USN Journal.
typedef struct {
    NtfsIndex* m_index;
    volatile LONG m_running;               // Monitor active flag
    HANDLE m_monitorThread;                // Background monitoring thread
    HANDLE m_volumeHandle;                 // Volume partition handle
    USN m_nextUsn;                         // USN journal position
    DWORDLONG m_journalId;                 // USN journal unique ID
    HWND m_notifyWindow;                   // Notification target window
} UsnJournalMonitor;

UsnJournalMonitor* UsnJournalMonitor_Create(NtfsIndex* index);
void UsnJournalMonitor_Destroy(UsnJournalMonitor* monitor);

bool UsnJournalMonitor_Start(UsnJournalMonitor* monitor);
void UsnJournalMonitor_Stop(UsnJournalMonitor* monitor);
void UsnJournalMonitor_RegisterNotifyWindow(UsnJournalMonitor* monitor, HWND hwnd);

#endif // USN_JOURNAL_H
