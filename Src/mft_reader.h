#ifndef MFT_READER_H
#define MFT_READER_H

#include "fs_common.h"

// Represents a continuous run of clusters on disk
typedef struct {
    long long StartLcn;                    // Starting Logical Cluster Number
    long long Length;                      // Length of the run in clusters
} MftRun;

// Progress report callbacks
typedef struct {
    void* Context;
    void (*OnStart)(void* context, unsigned int totalExpected);
    void (*OnProgress)(void* context, unsigned int recordsRead, unsigned int totalExpected);
    void (*OnChunk)(void* context, unsigned char* chunkBuffer, size_t chunkSize, unsigned int startFrs, unsigned int recordSize);
    void (*OnComplete)(void* context, bool success, unsigned int recordSize);
} IMftProgressCallback;

typedef struct {
    volatile LONG m_cancelled;             // Thread cancellation atomic flag
    HANDLE m_volumeHandle;                 // Handle to raw partition
    HANDLE m_workerThread;                 // Background worker thread handle
} MftReader;

MftReader* MftReader_Create(void);
void MftReader_Destroy(MftReader* reader);

bool MftReader_ReadMftAsync(MftReader* reader, wchar_t driveLetter, IMftProgressCallback callback);
void MftReader_Cancel(MftReader* reader);

// Helper to decode NTFS runlist mapping pairs into cluster runs
bool MftReader_ParseDataRuns(const unsigned char* runlist, size_t maxLen, MftRun* outRuns, size_t maxRunsCount, size_t* outRunsCount);

#endif // MFT_READER_H
