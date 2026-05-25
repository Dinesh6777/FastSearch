#include "stdafx.h"
#include "NtfsIndex.h"

namespace Ntfs {

    NtfsIndex::NtfsIndex(wchar_t driveLetter) 
        : m_driveLetter(driveLetter), m_isIndexed(false), m_totalFiles(0), m_totalFolders(0) {
    }

    NtfsIndex::~NtfsIndex() {
    }

    std::wstring NtfsIndex::GetVolumePath() const {
        std::wstring path = L"\\\\.\\";
        path += m_driveLetter;
        path += L":";
        return path;
    }

    // Parses the raw buffer of Master File Table (MFT) records.
    // Each record is processed:
    // 1. Sector boundaries are verified and repaired (unfixup) using Update Sequence Array (USA).
    // 2. Attributes are enumerated in-order inside the record space.
    // 3. Metadata (timestamps, sizes, parent folder pointers) is cached in the flat O(1) vector.
    bool NtfsIndex::BuildIndex(const std::vector<unsigned char>& rawBuffer, unsigned int recordSize) {
        InitializeIndexSize(static_cast<unsigned int>(rawBuffer.size() / recordSize));
        ProcessMftChunk(const_cast<unsigned char*>(rawBuffer.data()), rawBuffer.size(), 0, recordSize);
        FinalizeIndex();
        return true;
    }

    void NtfsIndex::InitializeIndexSize(unsigned int totalRecords) {
        LockExclusive();
        m_records.clear();
        m_records.resize(totalRecords);
        m_totalFiles = 0;
        m_totalFolders = 0;
        m_isIndexed = false;

        // Hardcode standard root directory (FRS 5) representation
        if (5 < m_records.size()) {
            m_records[5] = std::make_unique<FileRecord>();
            m_records[5]->Name = L"";
            m_records[5]->ParentFrs = 5;
            m_records[5]->IsDirectory = true;
        }

        UnlockExclusive();
    }

    void NtfsIndex::ProcessMftChunk(unsigned char* chunkBuffer, size_t chunkSize, unsigned int startFrs, unsigned int recordSize) {
        LockExclusive();

        unsigned int numRecords = static_cast<unsigned int>(chunkSize / recordSize);
        
        // Ensure capacity
        if (startFrs + numRecords > m_records.size()) {
            m_records.resize(startFrs + numRecords + 1024);
        }

        // Pre-allocate std::wstring objects outside the loop to reuse their underlying buffers
        std::wstring primaryName;
        std::wstring dosName;
        primaryName.reserve(260);
        dosName.reserve(260);

        for (unsigned int i = 0; i < numRecords; ++i) {
            unsigned int frsIndex = startFrs + i;
            unsigned char* pRecordBytes = chunkBuffer + (i * recordSize);
            FILE_RECORD_SEGMENT_HEADER* frsh = reinterpret_cast<FILE_RECORD_SEGMENT_HEADER*>(pRecordBytes);

            // Skip unallocated or deleted records (magic must be "FILE", flags must show in-use)
            if (frsh->MultiSectorHeader.Magic != 0x454C4946 || !(frsh->Flags & FRH_IN_USE)) {
                continue;
            }

            // Verify and repair record sector ends using NTFS USA fixup
            if (!frsh->MultiSectorHeader.unfixup(recordSize)) {
                continue; // Skip record if fixup check fails (corrupt or partial record)
            }

            bool isDirectory = (frsh->Flags & FRH_DIRECTORY) != 0;

            // Iterate attributes inside this FRS
            ATTRIBUTE_RECORD_HEADER* attr = frsh->begin();
            unsigned char* recordEnd = pRecordBytes + recordSize;

            primaryName.clear();
            dosName.clear();
            unsigned int primaryParent = 0xFFFFFFFF;
            unsigned int dosParent = 0xFFFFFFFF;

            // Temporary stack metadata variables
            unsigned long long dateCreated = 0;
            unsigned long long dateModified = 0;
            unsigned long long dateAccessed = 0;
            unsigned int attributes = 0;
            unsigned long long size = 0;
            unsigned long long sizeOnDisk = 0;

            // Timestamps, sizes, and attribute structures
            while (reinterpret_cast<unsigned char*>(attr) + sizeof(ATTRIBUTE_RECORD_HEADER) <= recordEnd && 
                   attr->Type != AttributeEnd) {
                
                // Ensure length is safe to prevent infinite loops
                if (attr->Length == 0 || reinterpret_cast<unsigned char*>(attr) + attr->Length > recordEnd) {
                    break;
                }

                if (attr->Type == AttributeStandardInformation) {
                    // resident standard information
                    if (!attr->IsNonResident) {
                        const STANDARD_INFORMATION* stdInfo = 
                            reinterpret_cast<const STANDARD_INFORMATION*>(attr->GetValue());
                        dateCreated = stdInfo->CreationTime;
                        dateModified = stdInfo->LastModificationTime;
                        dateAccessed = stdInfo->LastAccessTime;
                        attributes = stdInfo->FileAttributes;
                    }
                }
                else if (attr->Type == AttributeFileName) {
                    // resident filename information
                    if (!attr->IsNonResident) {
                        const FILENAME_INFORMATION* fnInfo = 
                            reinterpret_cast<const FILENAME_INFORMATION*>(attr->GetValue());
                        
                        // Extract parent folder FRS index (low 48 bits of parent reference)
                        unsigned int pFrs = static_cast<unsigned int>(fnInfo->ParentDirectory & 0x0000FFFFFFFFFFFFLL);

                        // File names inside MFT are stored in UTF-16.
                        // Namespace flags: 0=POSIX, 1=Win32, 2=DOS, 3=Win32&DOS.
                        // We prefer Win32/POSIX names and fallback to DOS.
                        if (fnInfo->Flags == 2) {
                            if (dosName.empty()) {
                                dosName.assign(fnInfo->FileName, fnInfo->FileNameLength);
                                dosParent = pFrs;
                            }
                        } else {
                            if (primaryName.empty()) {
                                primaryName.assign(fnInfo->FileName, fnInfo->FileNameLength);
                                primaryParent = pFrs;
                            }
                        }
                    }
                }
                else if (attr->Type == AttributeData) {
                    // data stream attribute (file size)
                    if (attr->IsNonResident) {
                        size = attr->NonResident.DataSize;
                        sizeOnDisk = attr->NonResident.AllocatedSize;
                    } else {
                        size = attr->Resident.ValueLength;
                        sizeOnDisk = attr->Length; // Size of attribute record itself
                    }
                }

                attr = attr->next();
            }

            const std::wstring* pFinalName = nullptr;
            unsigned int finalParent = 0xFFFFFFFF;

            if (!primaryName.empty()) {
                pFinalName = &primaryName;
                finalParent = primaryParent;
            } else if (!dosName.empty()) {
                pFinalName = &dosName;
                finalParent = dosParent;
            }

            // Lazy Allocation: Only allocate and populate if the record has a valid name!
            if (pFinalName && !pFinalName->empty() && finalParent != 0xFFFFFFFF) {
                m_records[frsIndex] = std::make_unique<FileRecord>();
                FileRecord& item = *m_records[frsIndex];
                item.IsDirectory = isDirectory;
                item.Name = *pFinalName;
                item.ParentFrs = finalParent;
                item.DateCreated = dateCreated;
                item.DateModified = dateModified;
                item.DateAccessed = dateAccessed;
                item.Attributes = attributes;
                item.Size = size;
                item.SizeOnDisk = sizeOnDisk;

                if (item.IsDirectory) {
                    m_totalFolders++;
                } else {
                    m_totalFiles++;
                }
            }
        }

        UnlockExclusive();
    }

    void NtfsIndex::FinalizeIndex() {
        LockExclusive();
        m_isIndexed = true;

        // Log first 50 non-empty indexed files to check for corruption
        FILE* fBuildLog = nullptr;
        if (_wfopen_s(&fBuildLog, L"C:\\Users\\Sri\\Documents\\FastSearch\\build_index_debug.txt", L"w") == 0 && fBuildLog) {
            fwprintf(fBuildLog, L"First 50 Indexed Names:\n");
            int loggedCount = 0;
            for (size_t k = 0; k < m_records.size() && loggedCount < 50; ++k) {
                if (m_records[k] && !m_records[k]->Name.empty()) {
                    fwprintf(fBuildLog, L"  FRS [%zu]: IsDir=%d, PrimaryName='%s'\n", 
                             k, m_records[k]->IsDirectory, m_records[k]->Name.c_str());
                    loggedCount++;
                }
            }
            fclose(fBuildLog);
        }

        UnlockExclusive();
    }


    // Safely resolves the full canonical path for a given MFT file record
    // by walking the parent chain upward until the partition root (FRS 5 or 0) is hit.
    // Time complexity: O(depth) where depth is the directory level (usually < 10), making it extremely fast.
    std::wstring NtfsIndex::ResolveFullPath(unsigned int recordIndex) const {
        std::wstring path;
        unsigned int current = recordIndex;

        std::shared_lock<std::shared_mutex> lck(m_lock);

        int depth = 0;
        // Keep tracing parent references upwards
        while (current < m_records.size() && current != 5 && current != 0 && depth < 256) {
            const auto& pRec = m_records[current];
            if (!pRec || pRec->Name.empty()) {
                break;
            }
            if (pRec->ParentFrs == 0xFFFFFFFF || pRec->ParentFrs == current) {
                break;
            }
            if (path.empty()) {
                path = pRec->Name;
            } else {
                path = pRec->Name + L"\\" + path;
            }
            current = pRec->ParentFrs;
            depth++;
        }

        std::wstring drive;
        drive += m_driveLetter;
        drive += L":\\";
        return drive + path;
    }

    // Resolves the full canonical path starting from a specific parent FRS and file name.
    // Extremely fast and supports multiple hard links correctly by starting path resolution from the link's specific parent.
    std::wstring NtfsIndex::ResolveFullPath(unsigned int parentFrs, const std::wstring& name) const {
        std::wstring path = name;
        unsigned int current = parentFrs;

        std::shared_lock<std::shared_mutex> lck(m_lock);

        int depth = 0;
        // Keep tracing parent references upwards
        while (current < m_records.size() && current != 5 && current != 0 && depth < 256) {
            const auto& pRec = m_records[current];
            if (!pRec || pRec->Name.empty()) {
                break;
            }
            if (pRec->ParentFrs == 0xFFFFFFFF || pRec->ParentFrs == current) {
                break;
            }
            if (path.empty()) {
                path = pRec->Name;
            } else {
                path = pRec->Name + L"\\" + path;
            }
            current = pRec->ParentFrs;
            depth++;
        }

        std::wstring drive;
        drive += m_driveLetter;
        drive += L":\\";
        return drive + path;
    }

    // Adds a new file/directory record or updates an existing one inside the flat index.
    // Invoked by real-time monitors (USN Journal) to maintain synchronization.
    void NtfsIndex::AddOrUpdateRecord(unsigned int recordIndex, const std::wstring& name, unsigned int parentFrs,
                                      unsigned long long size, unsigned long long sizeOnDisk,
                                      unsigned long long dateModified, unsigned long long dateCreated,
                                      unsigned long long dateAccessed, unsigned int attributes, bool isDirectory) {
        LockExclusive();
        
        if (recordIndex >= m_records.size()) {
            m_records.resize(recordIndex + 1024); // grow buffer in batches
        }

        if (!m_records[recordIndex]) {
            m_records[recordIndex] = std::make_unique<FileRecord>();
        }
        FileRecord& item = *m_records[recordIndex];
        
        // Update stats
        if (!item.Name.empty()) {
            if (item.IsDirectory) m_totalFolders--;
            else m_totalFiles--;
        }

        item.Name = name;
        item.ParentFrs = parentFrs;
        item.Size = size;
        item.SizeOnDisk = sizeOnDisk;
        item.DateModified = dateModified;
        item.DateCreated = dateCreated;
        item.DateAccessed = dateAccessed;
        item.Attributes = attributes;
        item.IsDirectory = isDirectory;

        if (item.IsDirectory) m_totalFolders++;
        else m_totalFiles++;

        UnlockExclusive();
    }

    // Removes a record from the active search index by marking its Parent FRS index as unallocated.
    void NtfsIndex::DeleteRecord(unsigned int recordIndex) {
        LockExclusive();

        if (recordIndex < m_records.size() && m_records[recordIndex]) {
            FileRecord& item = *m_records[recordIndex];
            if (!item.Name.empty()) {
                if (item.IsDirectory) m_totalFolders--;
                else m_totalFiles--;
            }
            m_records[recordIndex].reset(); // Free the memory instantly
        }

        UnlockExclusive();
    }

    void NtfsIndex::NotifyIndexChanged() {
        std::shared_lock<std::shared_mutex> lck(m_lock);
        if (m_notifyWindow && ::IsWindow(m_notifyWindow)) {
            ::PostMessageW(m_notifyWindow, WM_NTFS_INDEX_CHANGED, 0, 0);
        }
    }

} // namespace Ntfs
