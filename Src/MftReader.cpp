#include "stdafx.h"
#include "MftReader.h"

namespace Ntfs {

    MftReader::MftReader() : m_cancelled(false), m_volumeHandle(INVALID_HANDLE_VALUE) {
    }

    MftReader::~MftReader() {
        Cancel();
    }

    void MftReader::Cancel() {
        m_cancelled = true;
        if (m_workerThread.joinable()) {
            m_workerThread.join();
        }
        if (m_volumeHandle != INVALID_HANDLE_VALUE) {
            CloseHandle(m_volumeHandle);
            m_volumeHandle = INVALID_HANDLE_VALUE;
        }
    }

    bool MftReader::ReadMftAsync(const std::wstring& volumePath, IMftProgressCallback* callback) {
        Cancel(); // Clear any existing worker thread
        m_cancelled = false;

        // Start worker thread to read MFT asynchronously
        m_workerThread = std::thread(ReaderThreadProc, this, volumePath, callback);
        return true;
    }

    // Decodes the compressed NTFS data run list (mapping pairs).
    // In NTFS, non-resident file data is mapped onto physical disk sectors via compressed 'runs'.
    // Each run specifies a Length (number of clusters) and a Logical Cluster Number (LCN) offset.
    // LCN offsets are signed deltas relative to the starting LCN of the preceding run.
    bool MftReader::ParseDataRuns(const unsigned char* runlist, size_t maxLen, std::vector<MftRun>& outRuns) {
        size_t idx = 0;
        long long currentLCN = 0;

        while (idx < maxLen && runlist[idx] != 0x00) {
            unsigned char header = runlist[idx++];
            
            // Low 4 bits: Number of bytes representing the run length
            int lenBytes = header & 0x0F;
            // High 4 bits: Number of bytes representing the LCN starting offset
            int offsetBytes = (header >> 4) & 0x0F;

            if (idx + lenBytes + offsetBytes > maxLen) {
                return false; // Buffer overflow safety check
            }

            // Extract the run length (unsigned)
            long long runLength = 0;
            for (int b = 0; b < lenBytes; ++b) {
                runLength |= ((long long)runlist[idx++] << (8 * b));
            }

            // Extract the LCN delta (signed!)
            long long lcnDelta = 0;
            for (int b = 0; b < offsetBytes; ++b) {
                lcnDelta |= ((long long)runlist[idx++] << (8 * b));
            }

            // Perform sign extension for the LCN delta (since it can be negative)
            if (offsetBytes > 0 && (lcnDelta & (1LL << (8 * offsetBytes - 1)))) {
                lcnDelta |= (~0LL << (8 * offsetBytes));
            }

            // Calculate the absolute Logical Cluster Number
            currentLCN += lcnDelta;

            MftRun run;
            run.StartLcn = currentLCN;
            run.Length = runLength;
            outRuns.push_back(run);
        }

        return !outRuns.empty();
    }

    void MftReader::ReaderThreadProc(MftReader* pThis, std::wstring volumePath, IMftProgressCallback* callback) {
        // Ensure volume path is formatted correctly as a raw drive path (e.g. "\\.\C:")
        if (volumePath.length() == 1 || (volumePath.length() == 2 && volumePath[1] == L':')) {
            volumePath = L"\\\\.\\" + volumePath.substr(0, 1) + L":";
        }

        // Open raw disk handle.
        // Requires ADMINISTRATOR privileges because we are reading partition sectors directly.
        // FILE_FLAG_NO_BUFFERING requires all reads to be sector-aligned in memory and size.
        pThis->m_volumeHandle = CreateFileW(
            volumePath.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            NULL,
            OPEN_EXISTING,
            FILE_FLAG_NO_BUFFERING,
            NULL
        );

        if (pThis->m_volumeHandle == INVALID_HANDLE_VALUE) {
            callback->OnComplete(false, 0);
            return;
        }

        // 1. Read Boot Sector (Sector 0)
        // Allocate a sector-aligned buffer (typically 512 or 4096 bytes)
        DWORD bytesRead = 0;
        std::vector<unsigned char> sectorBuffer(512);
        if (!ReadFile(pThis->m_volumeHandle, sectorBuffer.data(), 512, &bytesRead, NULL) || bytesRead != 512) {
            callback->OnComplete(false, 0);
            return;
        }

        NTFS_BOOT_SECTOR* bootSector = reinterpret_cast<NTFS_BOOT_SECTOR*>(sectorBuffer.data());
        
        // Verify this is indeed an NTFS partition
        if (memcmp(bootSector->Oem, "NTFS    ", 8) != 0) {
            callback->OnComplete(false, 0);
            return;
        }

        unsigned int sectorSize = bootSector->BytesPerSector;
        unsigned int clusterSize = bootSector->cluster_size();
        unsigned int recordSize = bootSector->file_record_size();
        long long mftStartLcn = bootSector->MftStartLcn;

        // 2. Read FRS 0 ($MFT itself)
        // The first file record in the MFT is the definition of the MFT itself.
        // We find it at physical disk offset (MftStartLcn * clusterSize).
        LARGE_INTEGER mftOffset;
        mftOffset.QuadPart = mftStartLcn * clusterSize;

        std::vector<unsigned char> frs0Buffer(recordSize);
        OVERLAPPED ol = { 0 };
        ol.Offset = mftOffset.LowPart;
        ol.OffsetHigh = mftOffset.HighPart;

        if (!ReadFile(pThis->m_volumeHandle, frs0Buffer.data(), recordSize, &bytesRead, &ol) || bytesRead != recordSize) {
            callback->OnComplete(false, 0);
            return;
        }

        FILE_RECORD_SEGMENT_HEADER* frs0 = reinterpret_cast<FILE_RECORD_SEGMENT_HEADER*>(frs0Buffer.data());
        
        // Validate magic signature "FILE"
        if (frs0->MultiSectorHeader.Magic != 0x454C4946) { // 'FILE' in little-endian
            callback->OnComplete(false, 0);
            return;
        }

        // Apply NTFS Update Sequence Array (USA) unfixup to ensure record is not corrupt
        if (!frs0->MultiSectorHeader.unfixup(recordSize)) {
            callback->OnComplete(false, 0);
            return;
        }

        // 3. Find non-resident $DATA attributes of FRS 0
        // These attributes hold the actual cluster locations (runs) of the rest of the MFT on disk.
        // We collect all of them to support fragmented MFTs (highly common on active drives).
        ATTRIBUTE_RECORD_HEADER* attr = frs0->begin();
        ATTRIBUTE_RECORD_HEADER* endMarker = reinterpret_cast<ATTRIBUTE_RECORD_HEADER*>(frs0Buffer.data() + recordSize);

        std::vector<ATTRIBUTE_RECORD_HEADER*> dataAttrs;
        while (attr < endMarker && attr->Type != AttributeEnd) {
            // Ensure length is safe to prevent infinite loops
            if (attr->Length == 0 || reinterpret_cast<unsigned char*>(attr) + attr->Length > frs0Buffer.data() + recordSize) {
                break;
            }
            if (attr->Type == AttributeData) {
                dataAttrs.push_back(attr);
            }
            attr = attr->next();
        }

        if (dataAttrs.empty()) {
            callback->OnComplete(false, 0);
            return;
        }

        long long mftLogicalSize = 0;
        for (auto dataAttr : dataAttrs) {
            if (dataAttr->IsNonResident && dataAttr->NonResident.LowestVCN == 0) {
                mftLogicalSize = dataAttr->NonResident.DataSize;
                break;
            }
        }

        if (mftLogicalSize == 0) {
            // Fallback to first data attribute if LowestVCN == 0 is not found or is resident
            if (dataAttrs[0]->IsNonResident) {
                mftLogicalSize = dataAttrs[0]->NonResident.DataSize;
            } else {
                mftLogicalSize = dataAttrs[0]->Length;
            }
        }

        unsigned int totalExpectedRecords = static_cast<unsigned int>(mftLogicalSize / recordSize);

        // Pre-allocate index array size dynamically prior to chunked indexing
        callback->OnStart(totalExpectedRecords);

        // 4. Extract data runs of all MFT fragments
        std::vector<MftRun> mftRuns;
        for (auto dataAttr : dataAttrs) {
            if (dataAttr->IsNonResident) {
                const unsigned char* runlist = reinterpret_cast<const unsigned char*>(dataAttr) + dataAttr->NonResident.MappingPairsOffset;
                size_t runlistMaxLen = dataAttr->Length - dataAttr->NonResident.MappingPairsOffset;
                ParseDataRuns(runlist, runlistMaxLen, mftRuns);
            }
        }

        if (mftRuns.empty()) {
            callback->OnComplete(false, 0);
            return;
        }

        // 5. Read MFT clusters in chunks and process them immediately (zero persistent RAM copy!)
        const DWORD maxChunkSize = 4 * 1024 * 1024; // 4MB chunks
        std::vector<unsigned char> chunkBuffer(maxChunkSize);

        unsigned int recordsProcessed = 0;
        DWORD lastUpdateTick = 0;

        for (const auto& run : mftRuns) {
            if (pThis->m_cancelled) {
                callback->OnComplete(false, 0);
                return;
            }

            long long runByteSize = run.Length * clusterSize;
            long long runStartByteOffset = run.StartLcn * clusterSize;

            // Read the run in chunks to provide visual progress updates
            long long remainingBytes = runByteSize;
            long long currentReadOffset = runStartByteOffset;

            while (remainingBytes > 0) {
                if (pThis->m_cancelled) {
                    callback->OnComplete(false, 0);
                    return;
                }

                DWORD chunkToRead = static_cast<DWORD>(std::min(remainingBytes, (long long)maxChunkSize));
                
                // Align to sector boundaries
                chunkToRead = (chunkToRead + sectorSize - 1) & ~(sectorSize - 1);

                OVERLAPPED runOl = { 0 };
                LARGE_INTEGER liOffset;
                liOffset.QuadPart = currentReadOffset;
                runOl.Offset = liOffset.LowPart;
                runOl.OffsetHigh = liOffset.HighPart;

                if (!ReadFile(pThis->m_volumeHandle, chunkBuffer.data(), chunkToRead, &bytesRead, &runOl) || bytesRead == 0) {
                    callback->OnComplete(false, 0);
                    return;
                }

                // Process this chunk immediately!
                unsigned int startFrs = recordsProcessed;
                callback->OnChunk(chunkBuffer.data(), bytesRead, startFrs, recordSize);

                remainingBytes -= bytesRead;
                currentReadOffset += bytesRead;
                recordsProcessed += static_cast<unsigned int>(bytesRead / recordSize);

                // Throttle progress callback to avoid blocking UI updates on small/fragmented chunk runs
                DWORD currentTick = GetTickCount();
                if (currentTick - lastUpdateTick >= 100 || recordsProcessed >= totalExpectedRecords || remainingBytes <= 0) {
                    callback->OnProgress(recordsProcessed, totalExpectedRecords);
                    lastUpdateTick = currentTick;
                }
            }
        }

        // Successfully completed raw reads! Close volume handle and trigger complete callback.
        CloseHandle(pThis->m_volumeHandle);
        pThis->m_volumeHandle = INVALID_HANDLE_VALUE;

        callback->OnComplete(true, recordSize);
    }

} // namespace Ntfs
