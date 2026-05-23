#include "stdafx.h"
#include "ShellIntegration.h"
#include "InputDialog.h"
#include <shlobj.h>

namespace AppShell {

    // Helper to split a path into directory and file name components
    static void SplitPath(const std::wstring& fullPath, std::wstring& directory, std::wstring& fileName) {
        size_t lastSlash = fullPath.find_last_of(L"\\/");
        if (lastSlash == std::wstring::npos) {
            directory = L"";
            fileName = fullPath;
        } else {
            directory = fullPath.substr(0, lastSlash);
            fileName = fullPath.substr(lastSlash + 1);
            if (directory.length() == 2 && directory[1] == L':') {
                directory += L"\\"; // Map C: to C:\ for valid Shell parsing
            }
        }
    }

    // Helper to clean and normalize menu item text to safely identify duplicate standard actions
    static std::wstring CleanMenuText(const wchar_t* text) {
        std::wstring s = text;
        // Strip everything after tab (\t) which is the keyboard shortcut
        size_t tabPos = s.find(L'\t');
        if (tabPos != std::wstring::npos) {
            s = s.substr(0, tabPos);
        }
        // Strip ampersands
        s.erase(std::remove(s.begin(), s.end(), L'&'), s.end());
        // Trim spaces
        s.erase(0, s.find_first_not_of(L" "));
        s.erase(s.find_last_not_of(L" ") + 1);
        // Convert to lowercase
        std::transform(s.begin(), s.end(), s.begin(), ::towlower);
        return s;
    }

    // Resolves and executes the native Windows Explorer shell context menu.
    // Binds directly to the items' parent IShellFolder and requests the IContextMenu UI object.
    // This traditional method is 100% reliable and fixes the bug where copying from the context menu copied the entire drive.
    bool ShellIntegration::ShowContextMenu(HWND hwndParent, const std::vector<std::wstring>& filePaths, POINT ptScreen) {
        if (filePaths.empty()) return false;

        // Group files by parent directory. Shell menus are bound per-IShellFolder.
        std::wstring parentDir, fileName0;
        SplitPath(filePaths[0], parentDir, fileName0);

        // Parse parent directory into a PIDL
        PIDLIST_ABSOLUTE parentPidl = nullptr;
        HRESULT hr = ::SHParseDisplayName(parentDir.c_str(), NULL, &parentPidl, 0, NULL);
        if (FAILED(hr)) return false;

        // Bind parent PIDL to its desktop folder interface
        IShellFolder* desktopFolder = nullptr;
        hr = ::SHGetDesktopFolder(&desktopFolder);
        if (FAILED(hr)) {
            ::CoTaskMemFree(parentPidl);
            return false;
        }

        IShellFolder* parentFolder = nullptr;
        hr = desktopFolder->BindToObject(parentPidl, NULL, IID_IShellFolder, (void**)&parentFolder);
        desktopFolder->Release();
        ::CoTaskMemFree(parentPidl);

        if (FAILED(hr)) return false;

        // Get relative PIDLs for all files in this parent folder
        std::vector<PITEMID_CHILD> relativePidls;
        for (const auto& path : filePaths) {
            std::wstring dir, file;
            SplitPath(path, dir, file);

            // Only include files that reside in the same parent directory to prevent UI mismatches
            if (_wcsicmp(dir.c_str(), parentDir.c_str()) == 0) {
                PITEMID_CHILD childPidl = nullptr;
                hr = parentFolder->ParseDisplayName(hwndParent, NULL, const_cast<wchar_t*>(file.c_str()), NULL, &childPidl, NULL);
                if (SUCCEEDED(hr) && childPidl) {
                    relativePidls.push_back(childPidl);
                }
            }
        }

        if (relativePidls.empty()) {
            parentFolder->Release();
            return false;
        }

        // Store relative PIDLs in a 100% type-safe LPCITEMIDLIST vector to avoid cast errors
        std::vector<LPCITEMIDLIST> pidlPointers;
        pidlPointers.reserve(relativePidls.size());
        for (auto p : relativePidls) {
            pidlPointers.push_back(p);
        }

        // Get IContextMenu interface for this selection
        IContextMenu* contextMenu = nullptr;
        hr = parentFolder->GetUIObjectOf(
            hwndParent,
            static_cast<UINT>(pidlPointers.size()),
            pidlPointers.data(),
            IID_IContextMenu,
            NULL,
            (void**)&contextMenu
        );

        if (FAILED(hr) || !contextMenu) {
            for (auto p : relativePidls) {
                ::CoTaskMemFree(p);
            }
            parentFolder->Release();
            return false;
        }

        // Create standard popup menu
        HMENU hMenu = ::CreatePopupMenu();
        if (!hMenu) {
            contextMenu->Release();
            for (auto p : relativePidls) {
                ::CoTaskMemFree(p);
            }
            parentFolder->Release();
            return false;
        }

        // Prepend our standard custom actions at the top of the menu with clean separators
        ::AppendMenuW(hMenu, MF_STRING, 0x6001, L"&Open");
        ::AppendMenuW(hMenu, MF_STRING, 0x6002, L"Open &Path");
        ::AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
        ::AppendMenuW(hMenu, MF_STRING, 0x6003, L"Copy &full name to clipboard");
        ::AppendMenuW(hMenu, MF_STRING, 0x6004, L"Copy &path");
        ::AppendMenuW(hMenu, MF_STRING, 0x6005, L"Copy as p&ath");
        ::AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
        ::AppendMenuW(hMenu, MF_STRING, 0x6006, L"Cu&t");
        ::AppendMenuW(hMenu, MF_STRING, 0x6007, L"&Copy");
        ::AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
        ::AppendMenuW(hMenu, MF_STRING, 0x6008, L"&Delete");
        ::AppendMenuW(hMenu, MF_STRING, 0x6009, L"&Rename");
        ::AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);

        // Populate popup menu with native context items starting at ID = 1 after our 12 custom items
        hr = contextMenu->QueryContextMenu(hMenu, 12, 1, 0x5FFF, CMF_NORMAL);
        if (FAILED(hr)) {
            ::DestroyMenu(hMenu);
            contextMenu->Release();
            for (auto p : relativePidls) {
                ::CoTaskMemFree(p);
            }
            parentFolder->Release();
            return false;
        }

        // Remove duplicate native Cut, Copy, and Delete entries at the bottom of the menu
        int itemCount = ::GetMenuItemCount(hMenu);
        for (int i = itemCount - 1; i >= 12; --i) {
            wchar_t buf[256] = { 0 };
            if (::GetMenuStringW(hMenu, i, buf, 256, MF_BYPOSITION) > 0) {
                std::wstring clean = CleanMenuText(buf);
                if (clean == L"cut" || clean == L"copy" || clean == L"delete") {
                    ::DeleteMenu(hMenu, i, MF_BYPOSITION);
                }
            }
        }

        // SetForegroundWindow is critical before TrackPopupMenu for standard Win32 focus safety
        ::SetForegroundWindow(hwndParent);
        
        int cmdId = ::TrackPopupMenu(
            hMenu,
            TPM_RETURNCMD | TPM_LEFTALIGN | TPM_RIGHTBUTTON,
            ptScreen.x,
            ptScreen.y,
            0,
            hwndParent,
            NULL
        );

        ::DestroyMenu(hMenu);

        // If a custom command was selected, execute it here
        if (cmdId >= 0x6001 && cmdId <= 0x6009) {
            if (cmdId == 0x6001) { // Open
                for (const auto& path : filePaths) {
                    ::ShellExecuteW(NULL, L"open", path.c_str(), NULL, NULL, SW_SHOWNORMAL);
                }
            }
            else if (cmdId == 0x6002) { // Open Path
                std::wstring arg = L"/select,\"" + filePaths[0] + L"\"";
                ::ShellExecuteW(NULL, L"open", L"explorer.exe", arg.c_str(), NULL, SW_SHOWNORMAL);
            }
            else if (cmdId == 0x6003) { // Copy full name to clipboard
                std::wstring text;
                for (size_t i = 0; i < filePaths.size(); ++i) {
                    std::wstring dir, file;
                    SplitPath(filePaths[i], dir, file);
                    if (i > 0) text += L"\r\n";
                    text += file;
                }
                if (::OpenClipboard(hwndParent)) {
                    ::EmptyClipboard();
                    size_t sizeBytes = (text.length() + 1) * sizeof(wchar_t);
                    HGLOBAL hGlobal = ::GlobalAlloc(GMEM_MOVEABLE, sizeBytes);
                    if (hGlobal) {
                        wchar_t* pBuf = reinterpret_cast<wchar_t*>(::GlobalLock(hGlobal));
                        wcscpy_s(pBuf, text.length() + 1, text.c_str());
                        ::GlobalUnlock(hGlobal);
                        ::SetClipboardData(CF_UNICODETEXT, hGlobal);
                    }
                    ::CloseClipboard();
                }
            }
            else if (cmdId == 0x6004) { // Copy path
                std::wstring text;
                for (size_t i = 0; i < filePaths.size(); ++i) {
                    std::wstring dir, file;
                    SplitPath(filePaths[i], dir, file);
                    if (i > 0) text += L"\r\n";
                    text += dir;
                }
                if (::OpenClipboard(hwndParent)) {
                    ::EmptyClipboard();
                    size_t sizeBytes = (text.length() + 1) * sizeof(wchar_t);
                    HGLOBAL hGlobal = ::GlobalAlloc(GMEM_MOVEABLE, sizeBytes);
                    if (hGlobal) {
                        wchar_t* pBuf = reinterpret_cast<wchar_t*>(::GlobalLock(hGlobal));
                        wcscpy_s(pBuf, text.length() + 1, text.c_str());
                        ::GlobalUnlock(hGlobal);
                        ::SetClipboardData(CF_UNICODETEXT, hGlobal);
                    }
                    ::CloseClipboard();
                }
            }
            else if (cmdId == 0x6005) { // Copy as path
                std::wstring text;
                for (size_t i = 0; i < filePaths.size(); ++i) {
                    if (i > 0) text += L"\r\n";
                    text += L"\"" + filePaths[i] + L"\"";
                }
                if (::OpenClipboard(hwndParent)) {
                    ::EmptyClipboard();
                    size_t sizeBytes = (text.length() + 1) * sizeof(wchar_t);
                    HGLOBAL hGlobal = ::GlobalAlloc(GMEM_MOVEABLE, sizeBytes);
                    if (hGlobal) {
                        wchar_t* pBuf = reinterpret_cast<wchar_t*>(::GlobalLock(hGlobal));
                        wcscpy_s(pBuf, text.length() + 1, text.c_str());
                        ::GlobalUnlock(hGlobal);
                        ::SetClipboardData(CF_UNICODETEXT, hGlobal);
                    }
                    ::CloseClipboard();
                }
            }
            else if (cmdId == 0x6006) { // Cut
                CopyFilesToClipboard(hwndParent, filePaths, true);
            }
            else if (cmdId == 0x6007) { // Copy
                CopyFilesToClipboard(hwndParent, filePaths, false);
            }
            else if (cmdId == 0x6008) { // Delete
                SHFILEOPSTRUCTW fileOp = { 0 };
                fileOp.hwnd = hwndParent;
                fileOp.wFunc = FO_DELETE;
                std::vector<wchar_t> fromBuffer;
                for (const auto& path : filePaths) {
                    fromBuffer.insert(fromBuffer.end(), path.begin(), path.end());
                    fromBuffer.push_back(L'\0');
                }
                fromBuffer.push_back(L'\0'); // double null
                fileOp.pFrom = fromBuffer.data();
                fileOp.fFlags = FOF_ALLOWUNDO; // Send to Recycle Bin!
                ::SHFileOperationW(&fileOp);
            }
            else if (cmdId == 0x6009) { // Rename
                std::wstring dir, file;
                SplitPath(filePaths[0], dir, file);
                CInputDialog dlg(L"Rename File", L"Enter new name:", file);
                if (dlg.DoModal(hwndParent) == IDOK) {
                    std::wstring newName = dlg.GetValue();
                    if (!newName.empty() && newName != file) {
                        std::wstring newFullPath = dir + L"\\" + newName;
                        if (dir.back() == L'\\') newFullPath = dir + newName;
                        if (!::MoveFileW(filePaths[0].c_str(), newFullPath.c_str())) {
                            ::MessageBoxW(hwndParent, L"Failed to rename file.", L"Error", MB_OK | MB_ICONERROR);
                        }
                    }
                }
            }
        }
        // Else if a native command was selected, execute it via IContextMenu Invoke
        else if (cmdId > 0) {
            CMINVOKECOMMANDINFO cmi = { 0 };
            cmi.cbSize = sizeof(cmi);
            cmi.hwnd = hwndParent;
            cmi.lpVerb = MAKEINTRESOURCEA(cmdId - 1);
            cmi.nShow = SW_SHOWNORMAL;
            contextMenu->InvokeCommand(&cmi);
        }

        contextMenu->Release();

        // Free relative PIDLs and parent folder after command execution
        for (auto p : relativePidls) {
            ::CoTaskMemFree(p);
        }
        parentFolder->Release();

        return true;
    }

    // Encodes selected file paths in a standard CF_HDROP structure for Clipboard copy/cut.
    bool ShellIntegration::CopyFilesToClipboard(HWND hwndOwner, const std::vector<std::wstring>& filePaths, bool isCut) {
        if (filePaths.empty()) return false;

        if (!OpenClipboard(hwndOwner)) return false;
        EmptyClipboard();

        // Calculate double-null-terminated buffer size
        size_t totalChars = 1; // for terminating double null
        for (const auto& path : filePaths) {
            totalChars += path.length() + 1;
        }

        size_t size = sizeof(DROPFILES) + (totalChars * sizeof(wchar_t));
        HGLOBAL hGlobal = GlobalAlloc(GHND | GMEM_SHARE, size);
        if (!hGlobal) {
            CloseClipboard();
            return false;
        }

        DROPFILES* df = reinterpret_cast<DROPFILES*>(GlobalLock(hGlobal));
        df->pFiles = sizeof(DROPFILES);
        df->fWide = TRUE; // specify wide string format

        wchar_t* pStr = reinterpret_cast<wchar_t*>(reinterpret_cast<char*>(df) + sizeof(DROPFILES));
        for (const auto& path : filePaths) {
            wcscpy_s(pStr, path.length() + 1, path.c_str());
            pStr += path.length() + 1;
        }
        *pStr = L'\0'; // double null terminator

        GlobalUnlock(hGlobal);

        // Set standard file drop descriptor on clipboard
        if (!SetClipboardData(CF_HDROP, hGlobal)) {
            GlobalFree(hGlobal);
            CloseClipboard();
            return false;
        }

        // Set preferred drop effect structure if the operation is a 'Cut' (Move action)
        if (isCut) {
            UINT moveFormat = RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT);
            HGLOBAL hEffect = GlobalAlloc(GHND | GMEM_SHARE, sizeof(DWORD));
            if (hEffect) {
                DWORD* effect = reinterpret_cast<DWORD*>(GlobalLock(hEffect));
                *effect = DROPEFFECT_MOVE;
                GlobalUnlock(hEffect);
                SetClipboardData(moveFormat, hEffect);
            }
        }

        CloseClipboard();
        return true;
    }

} // namespace AppShell
