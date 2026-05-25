#pragma once
#include "stdafx.h"
#include "NtfsStructs.h"

namespace Ntfs {

    // Represents a continuous run of clusters on disk
    struct MftRun {
        long long StartLcn;  // Starting Logical Cluster Number
        long long Length;    // Length of the run in clusters
    };

    // Callback interface for reporting MFT read progress
    class IMftProgressCallback {
    public:
        virtual void OnStart(unsigned int totalExpected) = 0;
        virtual void OnProgress(unsigned int recordsRead, unsigned int totalExpected) = 0;
        virtual void OnChunk(unsigned char* chunkBuffer, size_t chunkSize, unsigned int startFrs, unsigned int recordSize) = 0;
        virtual void OnComplete(bool success, unsigned int recordSize) = 0;
    };

    // class MftReader
    // Responsible for opening a raw volume handle, reading the boot sector,
    // locating the MFT, parsing its data runs, and reading the entire MFT
    // asynchronously using high-performance Win32 Overlapped I/O.
    class MftReader {
    public:
        MftReader();
        ~MftReader();

        // Starts the asynchronous MFT reading process for a specific volume
        // volumePath: e.g. "C:" or "\\\\.\\C:"
        bool ReadMftAsync(const std::wstring& volumePath, IMftProgressCallback* callback);

        // Cancel the current read operation
        void Cancel();

        // Helper to extract runs from non-resident MFT `$DATA` attribute
        static bool ParseDataRuns(const unsigned char* runlist, size_t maxLen, std::vector<MftRun>& outRuns);

    private:
        static void ReaderThreadProc(MftReader* pThis, std::wstring volumePath, IMftProgressCallback* callback);

        std::atomic<bool> m_cancelled;
        std::thread m_workerThread;
        HANDLE m_volumeHandle;
    };

} // namespace Ntfs
