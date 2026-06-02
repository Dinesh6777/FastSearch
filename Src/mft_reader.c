#include "mft_reader.h"
#include "ntfs_structs.h"

// Lifecycle management
MftReader* MftReader_Create(void) {
    MftReader* reader = (MftReader*)malloc(sizeof(MftReader));
    if (!reader) return NULL;
    reader->m_cancelled = 0;
    reader->m_volumeHandle = INVALID_HANDLE_VALUE;
    reader->m_workerThread = NULL;
    return reader;
}

void MftReader_Destroy(MftReader* reader) {
    if (!reader) return;
    MftReader_Cancel(reader);
    free(reader);
}

void MftReader_Cancel(MftReader* reader) {
    InterlockedExchange(&reader->m_cancelled, 1);
    if (reader->m_workerThread) {
        WaitForSingleObject(reader->m_workerThread, INFINITE);
        CloseHandle(reader->m_workerThread);
        reader->m_workerThread = NULL;
    }
    if (reader->m_volumeHandle != INVALID_HANDLE_VALUE) {
        CloseHandle(reader->m_volumeHandle);
        reader->m_volumeHandle = INVALID_HANDLE_VALUE;
    }
}

// Parses NTFS non-resident runlist mapping pairs into cluster runs
bool MftReader_ParseDataRuns(const unsigned char* runlist, size_t maxLen, MftRun* outRuns, size_t maxRunsCount, size_t* outRunsCount) {
    size_t idx = 0;
    long long currentLCN = 0;
    size_t count = 0;

    while (idx < maxLen && runlist[idx] != 0x00 && count < maxRunsCount) {
        unsigned char header = runlist[idx++];
        
        int lenBytes = header & 0x0F;
        int offsetBytes = (header >> 4) & 0x0F;

        if (idx + lenBytes + offsetBytes > maxLen) {
            return false; // Buffer overflow safety check
        }

        // Extract run length (unsigned)
        long long runLength = 0;
        for (int b = 0; b < lenBytes; ++b) {
            runLength |= ((long long)runlist[idx++] << (8 * b));
        }

        // Extract LCN delta (signed)
        long long lcnDelta = 0;
        for (int b = 0; b < offsetBytes; ++b) {
            lcnDelta |= ((long long)runlist[idx++] << (8 * b));
        }

        // Perform sign extension for the signed LCN delta
        if (offsetBytes > 0 && (lcnDelta & (1LL << (8 * offsetBytes - 1)))) {
            lcnDelta |= (~0LL << (8 * offsetBytes));
        }

        currentLCN += lcnDelta;

        outRuns[count].StartLcn = currentLCN;
        outRuns[count].Length = runLength;
        count++;
    }

    *outRunsCount = count;
    return count > 0;
}

// Arguments struct passed to the asynchronous reader thread
typedef struct {
    MftReader* reader;
    wchar_t driveLetter;
    IMftProgressCallback callback;
} ReaderThreadArgs;

static unsigned int __stdcall ReaderThreadProc(void* arg) {
    ReaderThreadArgs* args = (ReaderThreadArgs*)arg;
    MftReader* pThis = args->reader;
    wchar_t driveLetter = args->driveLetter;
    IMftProgressCallback callback = args->callback;
    free(args); // Free the thread argument structure

    wchar_t volumePath[64];
    swprintf_s(volumePath, 64, L"\\\\.\\%c:", driveLetter);

    // Open raw volume handle with FILE_FLAG_NO_BUFFERING (requires sector aligned I/O)
    pThis->m_volumeHandle = CreateFileW(
        volumePath,
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL,
        OPEN_EXISTING,
        FILE_FLAG_NO_BUFFERING,
        NULL
    );

    if (pThis->m_volumeHandle == INVALID_HANDLE_VALUE) {
        callback.OnComplete(callback.Context, false, 0);
        return 0;
    }

    // 1. Read Boot Sector (Sector 0)
    // Allocate sector-aligned buffer for sector 0 (usually 512 bytes)
    unsigned char* sectorBuffer = (unsigned char*)_aligned_malloc(512, 512);
    if (!sectorBuffer) {
        CloseHandle(pThis->m_volumeHandle);
        pThis->m_volumeHandle = INVALID_HANDLE_VALUE;
        callback.OnComplete(callback.Context, false, 0);
        return 0;
    }

    DWORD bytesRead = 0;
    if (!ReadFile(pThis->m_volumeHandle, sectorBuffer, 512, &bytesRead, NULL) || bytesRead != 512) {
        _aligned_free(sectorBuffer);
        CloseHandle(pThis->m_volumeHandle);
        pThis->m_volumeHandle = INVALID_HANDLE_VALUE;
        callback.OnComplete(callback.Context, false, 0);
        return 0;
    }

    NTFS_BOOT_SECTOR* bootSector = (NTFS_BOOT_SECTOR*)sectorBuffer;
    if (memcmp(bootSector->Oem, "NTFS    ", 8) != 0) {
        _aligned_free(sectorBuffer);
        CloseHandle(pThis->m_volumeHandle);
        pThis->m_volumeHandle = INVALID_HANDLE_VALUE;
        callback.OnComplete(callback.Context, false, 0);
        return 0;
    }

    unsigned int sectorSize = bootSector->BytesPerSector;
    unsigned int clusterSize = Ntfs_ClusterSize(bootSector);
    unsigned int recordSize = Ntfs_FileRecordSize(bootSector);
    long long mftStartLcn = bootSector->MftStartLcn;

    _aligned_free(sectorBuffer);

    // 2. Read FRS 0 ($MFT itself)
    // Spans 'recordSize' bytes at physical disk offset (MftStartLcn * clusterSize).
    LARGE_INTEGER mftOffset;
    mftOffset.QuadPart = mftStartLcn * clusterSize;

    unsigned char* frs0Buffer = (unsigned char*)_aligned_malloc(recordSize, sectorSize);
    if (!frs0Buffer) {
        CloseHandle(pThis->m_volumeHandle);
        pThis->m_volumeHandle = INVALID_HANDLE_VALUE;
        callback.OnComplete(callback.Context, false, 0);
        return 0;
    }

    OVERLAPPED ol = { 0 };
    ol.Offset = mftOffset.LowPart;
    ol.OffsetHigh = mftOffset.HighPart;

    if (!ReadFile(pThis->m_volumeHandle, frs0Buffer, recordSize, &bytesRead, &ol) || bytesRead != recordSize) {
        _aligned_free(frs0Buffer);
        CloseHandle(pThis->m_volumeHandle);
        pThis->m_volumeHandle = INVALID_HANDLE_VALUE;
        callback.OnComplete(callback.Context, false, 0);
        return 0;
    }

    FILE_RECORD_SEGMENT_HEADER* frs0 = (FILE_RECORD_SEGMENT_HEADER*)frs0Buffer;
    if (frs0->MultiSectorHeader.Magic != 0x454C4946) { // 'FILE' in little-endian
        _aligned_free(frs0Buffer);
        CloseHandle(pThis->m_volumeHandle);
        pThis->m_volumeHandle = INVALID_HANDLE_VALUE;
        callback.OnComplete(callback.Context, false, 0);
        return 0;
    }

    // Apply Update Sequence Array safety unfixups
    if (!Ntfs_Unfixup(&frs0->MultiSectorHeader, recordSize)) {
        _aligned_free(frs0Buffer);
        CloseHandle(pThis->m_volumeHandle);
        pThis->m_volumeHandle = INVALID_HANDLE_VALUE;
        callback.OnComplete(callback.Context, false, 0);
        return 0;
    }

    // 3. Find non-resident $DATA attributes of FRS 0
    ATTRIBUTE_RECORD_HEADER* attr = Ntfs_FirstAttribute(frs0);
    ATTRIBUTE_RECORD_HEADER* endMarker = (ATTRIBUTE_RECORD_HEADER*)(frs0Buffer + recordSize);

    ATTRIBUTE_RECORD_HEADER* dataAttrs[16];
    int dataAttrsCount = 0;

    while (attr < endMarker && attr->Type != AttributeEnd) {
        if (attr->Length == 0 || (unsigned char*)attr + attr->Length > frs0Buffer + recordSize) {
            break;
        }
        if (attr->Type == AttributeData) {
            if (dataAttrsCount < 16) {
                dataAttrs[dataAttrsCount++] = attr;
            }
        }
        attr = Ntfs_NextAttribute(attr);
    }

    if (dataAttrsCount == 0) {
        _aligned_free(frs0Buffer);
        CloseHandle(pThis->m_volumeHandle);
        pThis->m_volumeHandle = INVALID_HANDLE_VALUE;
        callback.OnComplete(callback.Context, false, 0);
        return 0;
    }

    long long mftLogicalSize = 0;
    for (int k = 0; k < dataAttrsCount; k++) {
        ATTRIBUTE_RECORD_HEADER* dataAttr = dataAttrs[k];
        if (dataAttr->IsNonResident && dataAttr->u.NonResident.LowestVCN == 0) {
            mftLogicalSize = dataAttr->u.NonResident.DataSize;
            break;
        }
    }

    if (mftLogicalSize == 0) {
        if (dataAttrs[0]->IsNonResident) {
            mftLogicalSize = dataAttrs[0]->u.NonResident.DataSize;
        } else {
            mftLogicalSize = dataAttrs[0]->Length;
        }
    }

    unsigned int totalExpectedRecords = (unsigned int)(mftLogicalSize / recordSize);
    callback.OnStart(callback.Context, totalExpectedRecords);

    // 4. Extract data runs of all MFT fragments
    MftRun* mftRuns = (MftRun*)malloc(1024 * sizeof(MftRun));
    size_t mftRunsCount = 0;

    if (mftRuns) {
        for (int k = 0; k < dataAttrsCount; k++) {
            ATTRIBUTE_RECORD_HEADER* dataAttr = dataAttrs[k];
            if (dataAttr->IsNonResident) {
                const unsigned char* runlist = (const unsigned char*)dataAttr + dataAttr->u.NonResident.MappingPairsOffset;
                size_t runlistMaxLen = dataAttr->Length - dataAttr->u.NonResident.MappingPairsOffset;
                size_t parsedCount = 0;
                
                if (MftReader_ParseDataRuns(runlist, runlistMaxLen, mftRuns + mftRunsCount, 1024 - mftRunsCount, &parsedCount)) {
                    mftRunsCount += parsedCount;
                }
            }
        }
    }

    _aligned_free(frs0Buffer);

    if (mftRunsCount == 0 || !mftRuns) {
        if (mftRuns) free(mftRuns);
        CloseHandle(pThis->m_volumeHandle);
        pThis->m_volumeHandle = INVALID_HANDLE_VALUE;
        callback.OnComplete(callback.Context, false, 0);
        return 0;
    }

    // 5. Read MFT clusters in chunks and process them immediately
    const DWORD maxChunkSize = 4 * 1024 * 1024; // 4MB chunks
    unsigned char* chunkBuffer = (unsigned char*)_aligned_malloc(maxChunkSize, sectorSize);
    if (!chunkBuffer) {
        free(mftRuns);
        CloseHandle(pThis->m_volumeHandle);
        pThis->m_volumeHandle = INVALID_HANDLE_VALUE;
        callback.OnComplete(callback.Context, false, 0);
        return 0;
    }

    unsigned int recordsProcessed = 0;
    DWORD lastUpdateTick = 0;

    for (size_t r = 0; r < mftRunsCount; r++) {
        if (InterlockedOr(&pThis->m_cancelled, 0)) {
            break;
        }

        MftRun run = mftRuns[r];
        long long runByteSize = run.Length * clusterSize;
        long long runStartByteOffset = run.StartLcn * clusterSize;

        long long remainingBytes = runByteSize;
        long long currentReadOffset = runStartByteOffset;

        while (remainingBytes > 0) {
            if (InterlockedOr(&pThis->m_cancelled, 0)) {
                break;
            }

            DWORD chunkToRead = (DWORD)(remainingBytes < (long long)maxChunkSize ? remainingBytes : (long long)maxChunkSize);
            // Align chunk size to sector boundary
            chunkToRead = (chunkToRead + sectorSize - 1) & ~(sectorSize - 1);

            OVERLAPPED runOl = { 0 };
            LARGE_INTEGER liOffset;
            liOffset.QuadPart = currentReadOffset;
            runOl.Offset = liOffset.LowPart;
            runOl.OffsetHigh = liOffset.HighPart;

            if (!ReadFile(pThis->m_volumeHandle, chunkBuffer, chunkToRead, &bytesRead, &runOl) || bytesRead == 0) {
                // If read failed, trigger complete failure
                InterlockedExchange(&pThis->m_cancelled, 1);
                break;
            }

            unsigned int startFrs = recordsProcessed;
            callback.OnChunk(callback.Context, chunkBuffer, bytesRead, startFrs, recordSize);

            remainingBytes -= bytesRead;
            currentReadOffset += bytesRead;
            recordsProcessed += (unsigned int)(bytesRead / recordSize);

            // Throttle progress updates (every 100ms) to avoid GUI locks
            DWORD currentTick = GetTickCount();
            if (currentTick - lastUpdateTick >= 100 || recordsProcessed >= totalExpectedRecords || remainingBytes <= 0) {
                callback.OnProgress(callback.Context, recordsProcessed, totalExpectedRecords);
                lastUpdateTick = currentTick;
            }
        }
    }

    free(mftRuns);
    _aligned_free(chunkBuffer);
    CloseHandle(pThis->m_volumeHandle);
    pThis->m_volumeHandle = INVALID_HANDLE_VALUE;

    bool wasSuccess = !InterlockedOr(&pThis->m_cancelled, 0);
    callback.OnComplete(callback.Context, wasSuccess, recordSize);

    return 0;
}

bool MftReader_ReadMftAsync(MftReader* reader, wchar_t driveLetter, IMftProgressCallback callback) {
    MftReader_Cancel(reader); // Clean any existing workers
    InterlockedExchange(&reader->m_cancelled, 0);

    ReaderThreadArgs* args = (ReaderThreadArgs*)malloc(sizeof(ReaderThreadArgs));
    if (!args) return false;

    args->reader = reader;
    args->driveLetter = driveLetter;
    args->callback = callback;

    reader->m_workerThread = (HANDLE)_beginthreadex(
        NULL,
        0,
        ReaderThreadProc,
        args,
        0,
        NULL
    );

    return reader->m_workerThread != NULL;
}
