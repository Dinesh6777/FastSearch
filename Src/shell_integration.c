#include "shell_integration.h"
#include "resource.h"
#include <shlobj.h>

// Clean and normalize menu item text to safely identify duplicate standard actions
static void CleanMenuText(const wchar_t* src, wchar_t* dest, size_t destMax) {
    size_t idx = 0;
    for (size_t i = 0; src[i] != L'\0' && idx < destMax - 1; i++) {
        if (src[i] == L'\t') break;
        if (src[i] == L'&') continue;
        if (src[i] == L' ') continue;
        dest[idx++] = towlower(src[i]);
    }
    dest[idx] = L'\0';
}

// Split a path into directory and file name
static void SplitPath(const wchar_t* fullPath, wchar_t* directory, size_t dirMax, wchar_t* fileName, size_t fileMax) {
    const wchar_t* lastSlash = wcsrchr(fullPath, L'\\');
    const wchar_t* lastSlashAlt = wcsrchr(fullPath, L'/');
    if (lastSlashAlt > lastSlash) lastSlash = lastSlashAlt;
    
    if (lastSlash == NULL) {
        directory[0] = L'\0';
        wcscpy_s(fileName, fileMax, fullPath);
    } else {
        size_t dirLen = lastSlash - fullPath;
        if (dirLen >= dirMax) dirLen = dirMax - 1;
        wcsncpy_s(directory, dirMax, fullPath, dirLen);
        directory[dirLen] = L'\0';
        
        wcscpy_s(fileName, fileMax, lastSlash + 1);
        
        if (dirLen == 2 && directory[1] == L':') {
            wcscat_s(directory, dirMax, L"\\"); // Map C: to C:\ for valid Shell parsing
        }
    }
}

// Struct context passed to Input Dialog Proc
typedef struct {
    const wchar_t* Title;
    const wchar_t* Label;
    wchar_t* Value;
    size_t MaxValue;
} InputDlgContext;

static INT_PTR CALLBACK InputDlgProc(HWND hwndDlg, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
        case WM_INITDIALOG: {
            InputDlgContext* ctx = (InputDlgContext*)lParam;
            SetWindowLongPtr(hwndDlg, GWLP_USERDATA, (LONG_PTR)ctx);
            SetWindowTextW(hwndDlg, ctx->Title);
            SetDlgItemTextW(hwndDlg, IDC_INPUT_LABEL, ctx->Label);
            SetDlgItemTextW(hwndDlg, IDC_INPUT_EDIT, ctx->Value);

            HWND hEdit = GetDlgItem(hwndDlg, IDC_INPUT_EDIT);
            SetFocus(hEdit);
            SendMessageW(hEdit, EM_SETSEL, 0, -1);
            return FALSE; // Manually set focus
        }
        case WM_COMMAND: {
            WORD id = LOWORD(wParam);
            if (id == IDOK) {
                InputDlgContext* ctx = (InputDlgContext*)GetWindowLongPtr(hwndDlg, GWLP_USERDATA);
                if (ctx) {
                    GetDlgItemTextW(hwndDlg, IDC_INPUT_EDIT, ctx->Value, (int)ctx->MaxValue);
                }
                EndDialog(hwndDlg, IDOK);
                return TRUE;
            }
            else if (id == IDCANCEL) {
                EndDialog(hwndDlg, IDCANCEL);
                return TRUE;
            }
            break;
        }
    }
    return FALSE;
}

bool ShellIntegration_ShowContextMenu(HWND hwndParent, const wchar_t** filePaths, size_t filePathsCount, POINT ptScreen) {
    if (filePathsCount == 0 || !filePaths) return false;

    wchar_t parentDir[MAX_PATH];
    wchar_t fileName0[MAX_PATH];
    SplitPath(filePaths[0], parentDir, MAX_PATH, fileName0, MAX_PATH);

    // Parse parent directory into a PIDL
    PIDLIST_ABSOLUTE parentPidl = NULL;
    HRESULT hr = SHParseDisplayName(parentDir, NULL, &parentPidl, 0, NULL);
    if (FAILED(hr)) return false;

    IShellFolder* desktopFolder = NULL;
    hr = SHGetDesktopFolder(&desktopFolder);
    if (FAILED(hr)) {
        CoTaskMemFree(parentPidl);
        return false;
    }

    IShellFolder* parentFolder = NULL;
    hr = desktopFolder->lpVtbl->BindToObject(desktopFolder, parentPidl, NULL, &IID_IShellFolder, (void**)&parentFolder);
    desktopFolder->lpVtbl->Release(desktopFolder);
    CoTaskMemFree(parentPidl);

    if (FAILED(hr)) return false;

    // Get relative PIDLs for all files in this parent folder to prevent UI mismatches
    PITEMID_CHILD* relativePidls = (PITEMID_CHILD*)malloc(filePathsCount * sizeof(PITEMID_CHILD));
    size_t relativePidlsCount = 0;

    if (relativePidls) {
        for (size_t i = 0; i < filePathsCount; ++i) {
            wchar_t dir[MAX_PATH];
            wchar_t file[MAX_PATH];
            SplitPath(filePaths[i], dir, MAX_PATH, file, MAX_PATH);

            if (_wcsicmp(dir, parentDir) == 0) {
                PITEMID_CHILD childPidl = NULL;
                hr = parentFolder->lpVtbl->ParseDisplayName(parentFolder, hwndParent, NULL, file, NULL, &childPidl, NULL);
                if (SUCCEEDED(hr) && childPidl) {
                    relativePidls[relativePidlsCount++] = childPidl;
                }
            }
        }
    }

    if (relativePidlsCount == 0) {
        if (relativePidls) free(relativePidls);
        parentFolder->lpVtbl->Release(parentFolder);
        return false;
    }

    // Get IContextMenu interface for this selection
    IContextMenu* contextMenu = NULL;
    hr = parentFolder->lpVtbl->GetUIObjectOf(
        parentFolder,
        hwndParent,
        (UINT)relativePidlsCount,
        (LPCITEMIDLIST*)relativePidls,
        &IID_IContextMenu,
        NULL,
        (void**)&contextMenu
    );

    if (FAILED(hr) || !contextMenu) {
        for (size_t i = 0; i < relativePidlsCount; i++) {
            CoTaskMemFree(relativePidls[i]);
        }
        free(relativePidls);
        parentFolder->lpVtbl->Release(parentFolder);
        return false;
    }

    HMENU hMenu = CreatePopupMenu();
    if (!hMenu) {
        contextMenu->lpVtbl->Release(contextMenu);
        for (size_t i = 0; i < relativePidlsCount; i++) {
            CoTaskMemFree(relativePidls[i]);
        }
        free(relativePidls);
        parentFolder->lpVtbl->Release(parentFolder);
        return false;
    }

    // Prepend standard custom options at the top of context menu
    AppendMenuW(hMenu, MF_STRING, 0x6001, L"&Open");
    AppendMenuW(hMenu, MF_STRING, 0x6002, L"Open &Path");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING, 0x6003, L"Copy &full name to clipboard");
    AppendMenuW(hMenu, MF_STRING, 0x6004, L"Copy &path");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING, 0x6005, L"Cu&t");
    AppendMenuW(hMenu, MF_STRING, 0x6006, L"&Copy");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING, 0x6007, L"&Delete");
    AppendMenuW(hMenu, MF_STRING, 0x6008, L"&Rename");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);

    // Populate native context items
    hr = contextMenu->lpVtbl->QueryContextMenu(contextMenu, hMenu, 12, 1, 0x5FFF, CMF_NORMAL);
    if (FAILED(hr)) {
        DestroyMenu(hMenu);
        contextMenu->lpVtbl->Release(contextMenu);
        for (size_t i = 0; i < relativePidlsCount; i++) {
            CoTaskMemFree(relativePidls[i]);
        }
        free(relativePidls);
        parentFolder->lpVtbl->Release(parentFolder);
        return false;
    }

    // Remove duplicate native Cut, Copy, Delete, Rename entries
    int itemCount = GetMenuItemCount(hMenu);
    for (int i = itemCount - 1; i >= 12; --i) {
        wchar_t buf[256] = { 0 };
        if (GetMenuStringW(hMenu, i, buf, 256, MF_BYPOSITION) > 0) {
            wchar_t clean[256];
            CleanMenuText(buf, clean, 256);
            if (wcscmp(clean, L"cut") == 0 || wcscmp(clean, L"copy") == 0 || wcscmp(clean, L"delete") == 0 || 
                wcscmp(clean, L"copyaspath") == 0 || wcscmp(clean, L"copypath") == 0) {
                DeleteMenu(hMenu, i, MF_BYPOSITION);
            }
        }
    }

    SetForegroundWindow(hwndParent);
    
    int cmdId = TrackPopupMenu(
        hMenu,
        TPM_RETURNCMD | TPM_LEFTALIGN | TPM_RIGHTBUTTON,
        ptScreen.x,
        ptScreen.y,
        0,
        hwndParent,
        NULL
    );

    DestroyMenu(hMenu);

    if (cmdId >= 0x6001 && cmdId <= 0x6008) {
        if (cmdId == 0x6001) { // Open
            for (size_t i = 0; i < filePathsCount; ++i) {
                ShellExecuteW(NULL, L"open", filePaths[i], NULL, NULL, SW_SHOWNORMAL);
            }
        }
        else if (cmdId == 0x6002) { // Open Path
            wchar_t arg[512];
            swprintf_s(arg, 512, L"/select,\"%s\"", filePaths[0]);
            ShellExecuteW(NULL, L"open", L"explorer.exe", arg, NULL, SW_SHOWNORMAL);
        }
        else if (cmdId == 0x6003) { // Copy full name (absolute path)
            size_t totalLen = 1;
            for (size_t i = 0; i < filePathsCount; i++) {
                totalLen += wcslen(filePaths[i]) + 2;
            }
            wchar_t* text = (wchar_t*)malloc(totalLen * sizeof(wchar_t));
            if (text) {
                text[0] = L'\0';
                for (size_t i = 0; i < filePathsCount; i++) {
                    if (i > 0) wcscat_s(text, totalLen, L"\r\n");
                    wcscat_s(text, totalLen, filePaths[i]);
                }
                if (OpenClipboard(hwndParent)) {
                    EmptyClipboard();
                    HGLOBAL hGlobal = GlobalAlloc(GMEM_MOVEABLE, (wcslen(text) + 1) * sizeof(wchar_t));
                    if (hGlobal) {
                        wchar_t* pBuf = (wchar_t*)GlobalLock(hGlobal);
                        wcscpy_s(pBuf, wcslen(text) + 1, text);
                        GlobalUnlock(hGlobal);
                        SetClipboardData(CF_UNICODETEXT, hGlobal);
                    }
                    CloseClipboard();
                }
                free(text);
            }
        }
        else if (cmdId == 0x6004) { // Copy path directory
            size_t totalLen = 1;
            for (size_t i = 0; i < filePathsCount; i++) {
                totalLen += wcslen(filePaths[i]) + 2;
            }
            wchar_t* text = (wchar_t*)malloc(totalLen * sizeof(wchar_t));
            if (text) {
                text[0] = L'\0';
                for (size_t i = 0; i < filePathsCount; i++) {
                    wchar_t dir[MAX_PATH];
                    wchar_t file[MAX_PATH];
                    SplitPath(filePaths[i], dir, MAX_PATH, file, MAX_PATH);
                    if (i > 0) wcscat_s(text, totalLen, L"\r\n");
                    wcscat_s(text, totalLen, dir);
                }
                if (OpenClipboard(hwndParent)) {
                    EmptyClipboard();
                    HGLOBAL hGlobal = GlobalAlloc(GMEM_MOVEABLE, (wcslen(text) + 1) * sizeof(wchar_t));
                    if (hGlobal) {
                        wchar_t* pBuf = (wchar_t*)GlobalLock(hGlobal);
                        wcscpy_s(pBuf, wcslen(text) + 1, text);
                        GlobalUnlock(hGlobal);
                        SetClipboardData(CF_UNICODETEXT, hGlobal);
                    }
                    CloseClipboard();
                }
                free(text);
            }
        }
        else if (cmdId == 0x6005) { // Cut
            ShellIntegration_CopyFilesToClipboard(hwndParent, filePaths, filePathsCount, true);
        }
        else if (cmdId == 0x6006) { // Copy
            ShellIntegration_CopyFilesToClipboard(hwndParent, filePaths, filePathsCount, false);
        }
        else if (cmdId == 0x6007) { // Delete (Recycle Bin)
            SHFILEOPSTRUCTW fileOp = { 0 };
            fileOp.hwnd = hwndParent;
            fileOp.wFunc = FO_DELETE;
            
            // Reconstruct double null terminated buffer
            size_t totalChars = 2; // For terminating double null
            for (size_t i = 0; i < filePathsCount; i++) {
                totalChars += wcslen(filePaths[i]) + 1;
            }
            wchar_t* fromBuffer = (wchar_t*)malloc(totalChars * sizeof(wchar_t));
            if (fromBuffer) {
                wchar_t* p = fromBuffer;
                for (size_t i = 0; i < filePathsCount; i++) {
                    wcscpy_s(p, totalChars - (p - fromBuffer), filePaths[i]);
                    p += wcslen(filePaths[i]) + 1;
                }
                *p = L'\0'; // double null
                fileOp.pFrom = fromBuffer;
                fileOp.fFlags = FOF_ALLOWUNDO; // Send to recycle bin
                SHFileOperationW(&fileOp);
                free(fromBuffer);
            }
        }
        else if (cmdId == 0x6008) { // Rename
            wchar_t dir[MAX_PATH];
            wchar_t file[MAX_PATH];
            SplitPath(filePaths[0], dir, MAX_PATH, file, MAX_PATH);

            wchar_t newName[MAX_PATH];
            wcscpy_s(newName, MAX_PATH, file);

            InputDlgContext ctx;
            ctx.Title = L"Rename File";
            ctx.Label = L"Enter new name:";
            ctx.Value = newName;
            ctx.MaxValue = MAX_PATH;

            if (DialogBoxParamW(GetModuleHandle(NULL), MAKEINTRESOURCE(IDD_INPUT), hwndParent, InputDlgProc, (LPARAM)&ctx) == IDOK) {
                if (wcslen(newName) > 0 && wcscmp(newName, file) != 0) {
                    wchar_t newFullPath[MAX_PATH];
                    if (wcslen(dir) > 0 && dir[wcslen(dir) - 1] == L'\\') {
                        swprintf_s(newFullPath, MAX_PATH, L"%s%s", dir, newName);
                    } else {
                        swprintf_s(newFullPath, MAX_PATH, L"%s\\%s", dir, newName);
                    }
                    if (!MoveFileW(filePaths[0], newFullPath)) {
                        MessageBoxW(hwndParent, L"Failed to rename file.", L"Error", MB_OK | MB_ICONERROR);
                    }
                }
            }
        }
    }
    else if (cmdId > 0) {
        CMINVOKECOMMANDINFO cmi = { 0 };
        cmi.cbSize = sizeof(cmi);
        cmi.hwnd = hwndParent;
        cmi.lpVerb = MAKEINTRESOURCEA(cmdId - 1);
        cmi.nShow = SW_SHOWNORMAL;
        contextMenu->lpVtbl->InvokeCommand(contextMenu, &cmi);
    }

    contextMenu->lpVtbl->Release(contextMenu);

    for (size_t i = 0; i < relativePidlsCount; i++) {
        CoTaskMemFree(relativePidls[i]);
    }
    free(relativePidls);
    parentFolder->lpVtbl->Release(parentFolder);

    return true;
}

bool ShellIntegration_CopyFilesToClipboard(HWND hwndOwner, const wchar_t** filePaths, size_t filePathsCount, bool isCut) {
    if (filePathsCount == 0 || !filePaths) return false;

    if (!OpenClipboard(hwndOwner)) return false;
    EmptyClipboard();

    size_t totalChars = 1; // Double null termination
    for (size_t i = 0; i < filePathsCount; i++) {
        totalChars += wcslen(filePaths[i]) + 1;
    }

    size_t size = sizeof(DROPFILES) + (totalChars * sizeof(wchar_t));
    HGLOBAL hGlobal = GlobalAlloc(GHND | GMEM_SHARE, size);
    if (!hGlobal) {
        CloseClipboard();
        return false;
    }

    DROPFILES* df = (DROPFILES*)GlobalLock(hGlobal);
    df->pFiles = sizeof(DROPFILES);
    df->fWide = TRUE;

    wchar_t* pStr = (wchar_t*)((char*)df + sizeof(DROPFILES));
    for (size_t i = 0; i < filePathsCount; i++) {
        wcscpy_s(pStr, totalChars - (pStr - (wchar_t*)((char*)df + sizeof(DROPFILES))), filePaths[i]);
        pStr += wcslen(filePaths[i]) + 1;
    }
    *pStr = L'\0'; // Double null

    GlobalUnlock(hGlobal);

    if (!SetClipboardData(CF_HDROP, hGlobal)) {
        GlobalFree(hGlobal);
        CloseClipboard();
        return false;
    }

    if (isCut) {
        UINT moveFormat = RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT);
        HGLOBAL hEffect = GlobalAlloc(GHND | GMEM_SHARE, sizeof(DWORD));
        if (hEffect) {
            DWORD* effect = (DWORD*)GlobalLock(hEffect);
            *effect = DROPEFFECT_MOVE;
            GlobalUnlock(hEffect);
            SetClipboardData(moveFormat, hEffect);
        }
    }

    CloseClipboard();
    return true;
}
