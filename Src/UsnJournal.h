#pragma once
#include "stdafx.h"
#include "NtfsIndex.h"

namespace Ntfs {

    // class UsnJournalMonitor
    // Listens to filesystem change events using Windows USN (Update Sequence Number) Journal.
    // When files are created, renamed, deleted, or modified, USN records are retrieved 
    // and applied incrementally to the `NtfsIndex` in real-time, keeping the index in-sync
    // without requiring expensive MFT re-scans.
    class UsnJournalMonitor {
    public:
        UsnJournalMonitor(NtfsIndex* index);
        ~UsnJournalMonitor();

        // Starts the background USN monitoring thread
        bool Start();

        // Stops the USN monitoring thread
        void Stop();

        // Register notify window for handle-based notifications
        void RegisterNotifyWindow(HWND hwnd) { m_notifyWindow = hwnd; }

        // Returns active volume handle
        HANDLE GetVolumeHandle() const { return m_volumeHandle; }

    private:
        static void MonitorThreadProc(UsnJournalMonitor* pThis);

        NtfsIndex* m_index;
        std::atomic<bool> m_running;
        std::thread m_monitorThread;
        HANDLE m_volumeHandle;
        USN m_nextUsn;
        DWORDLONG m_journalId;
        HWND m_notifyWindow = nullptr;
    };

} // namespace Ntfs
