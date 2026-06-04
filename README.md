# 🚀 FastSearch

**FastSearch** is an instant filename search engine that quickly locates files and folders by name on Windows. Searches 1 million files in milliseconds after indexing. 

### How is FastSearch different from other File search engines
* Small Portable installation file (<1MB).
* Clean and simple user interface.
* Quick file indexing.
* Quick searching.
* Quick startup.
* Minimal resource usage.
* No database on disk.
* Real-time updating.

#### [Download FastSearch from Releases](https://github.com/Dinesh6777/FastSearch/releases)
<img width="923" height="545" alt="image" src="https://github.com/user-attachments/assets/f436600a-5a6f-4263-8eac-5000d71c962e" />

---

## ✨ Features

FastSearch is built in pure **C** utilizing raw **Win32 SDK APIs** (with no C++ runtime overhead, WTL, or ATL dependencies). It parses raw NTFS Master File Tables (MFT) directly to index millions of files in seconds, delivering high-performance, real-time search results with an incredibly compact memory footprint.

*   **⚡ Sub-Second MFT Indexing**: Directly parses raw NTFS Master File Table (MFT) partitions using sector-aligned raw disk I/O, indexing over 1,000,000 files in seconds.
*   **🧠 Ultra-Low Memory Footprint**: Consolidated RAM consumption down to under **50-80 MB** using a contiguous Page-Pool Arena Allocator, a 2-level paged sparse array for FRS mappings, and lightweight 16-byte `SearchResult` references.
*   **🔄 Real-Time USN Synchronization**: Tracks all filesystem additions, deletions, renames, and attributes in real-time by reading the Windows Update Sequence Number (USN) Journal.
*   **🔌 Stateless Hot-Plugging & Safe Ejection**: Implements a stateless on-demand polling mechanism and robust PnP device interface translations (GUID volume resolution). Instantly indexes newly attached USB drives and allows safe ejection without locking the drive.
*   **💻 Native Windows UI**: Built with a custom native Win32 window frame using Segoe UI typography, smooth micro-animations, customizable views (Details, Small/Medium/Large/Extra Large Icons), instant character highlighting, and seamless system tray minimization.
*   **📋 Full Explorer Bindings**: Supports standard Windows Clipboard operations (Copy, Cut, Paste) and full OLE Drag-and-Drop compatibility directly into Windows Explorer.
*   **🔍 Multiple Match Modes**: Supports Plain Text, Wildcard matching (`*`, `?`), and regular expressions (Regex) in a thread-safe search interface.
*   **⌨️ Global Win+F Hotkey**: Supports system-wide `Win+F` (utilizing a low-level keyboard hook with dummy control key injection to bypass the Windows Feedback Hub and suppress the Start Menu popup) and `Ctrl+Shift+F` shortcuts to instantly restore and focus the application.
*   **🚀 Auto-Run on Startup**: Checkable menu option to run FastSearch on Windows logon via `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`, running fully in user space without requiring UAC elevation.

---

## 🏗️ Architecture Design

FastSearch achieves high performance and low resource usage through advanced memory and systems-engineering strategies:

```mermaid
graph TD
    A[Physical NTFS Volume] -->|Raw Sector I/O| B[MFT Reader]
    B -->|Build contiguous index| C[NtfsIndex]
    D[OS File Changes] -->|USN Journal Log| E[Stateless USN Monitor]
    E -->|Incremental Updates| C
    C -->|On-demand dynamically lookup| F[Win32 Virtual ListView]
    G[Search Queries] -->|Thread-Safe Query| H[SearchEngine]
    H -->|Lock-Free Search Matching| C
```

1.  **Page-Pool Arena Allocator**: Requesting large 4MB chunks from the OS using `malloc` and sequential-packing `FileRecord` structures contiguously. This eliminates individual heap headers, saving 30MB+ of RAM, and speeds up index destruction to a simple O(1) page-freeing loop.
2.  **2-Level Paged Sparse Array**: Maps File Reference Segment (FRS) indices to `FileRecord` pointers using `recordsByFrsPages`. This prevents reserving massive flat pointer arrays, reducing memory overhead by hundreds of megabytes.
3.  **On-Demand View Rendering**: The Virtual List View (`LVS_OWNERDATA`) retrieves visible row properties on-the-fly from the index via thread-safe shared locks (`SRWLOCK`), keeping search results at a lightweight 16 bytes per item.
4.  **Dual-Array Pre-Sorted Indexing (Contiguous Active Scan Array)**: Instead of scanning a sparse lookup array which is slow, cache-unfriendly, and requires sorting matching items at query-time, FastSearch maintains a packed, contiguous pointer array (`activeRecords`) containing only active records. Upon indexing finalization, this array is sorted alphabetically by name. Searching becomes a cache-friendly sequential iteration over `activeRecords` that generates pre-sorted results instantly (completing in ~2-8ms), completely eliminating real-time sorting overhead.

---

## 🛠️ Getting Started

### System Requirements
*   **OS**: Windows 10 or Windows 11 (64-bit or 32-bit)
*   **Permissions**: Administrator elevation (required to perform raw sector read access on NTFS physical disks and query the USN Journal).
*   **File System**: NTFS formatted partitions (FAT32/exFAT are not supported for raw MFT parsing).

### Build Prerequisites
*   **Build Tools**: Visual Studio Build Tools / Visual Studio (with "Desktop development with C++" workload installed which includes the MSVC compiler).
*   **SDK**: Windows 10 or Windows 11 SDK.

### Building the Project
We provide a batch file to automatically initialize the Visual Studio environment and build both 64-bit (`x64`) and 32-bit (`x86`) native binaries:

1. Open a standard command prompt (`cmd.exe`).
2. Navigate to the build script directory:
   ```cmd
   cd Src\bin
   ```
3. Run the compiler script:
   ```cmd
   .\build_vs.bat
   ```

The script will produce the following optimized binaries and deploy them to the deployment directory:
*   `PortableApp\FastSearch.exe` (x64 Version)
*   `PortableApp\FastSearch_x86.exe` (x86 Version)

---

## 📝 License

This project is released under the terms of the **MIT License**.

```text
MIT License

Copyright (c) 2026

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

---

## 🤝 Contributing

Contributions are welcome! Please feel free to open pull requests, submit bugs, or suggest new optimization layers to the FastSearch index engine.
