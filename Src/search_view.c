#include "search_view.h"
#include "settings_dialog.h"
#include "shell_integration.h"
#include "drag_drop.h"
#include "resource.h"
#include <commctrl.h>
#include <shlobj.h>
#include <commoncontrols.h>
#include <math.h>
#include <process.h>

#define WM_SEARCH_COMPLETE (WM_USER + 110)

// Forward declarations for background thread result sorting
static SearchViewData* g_SortData;
static int CompareResults(const void* a, const void* b);

#define WM_SEARCH_RESULTS_CHANGED (WM_USER + 101)
#define WM_NTFS_INDEX_CHANGED (WM_USER + 102)

// Search Edit Control subclass WndProc
static LRESULT CALLBACK EditSubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    SearchViewData* data = (SearchViewData*)GetWindowLongPtr(GetParent(hWnd), GWLP_USERDATA);
    WNDPROC oldProc = data ? data->oldEditWndProc : NULL;
    if (!oldProc) return DefWindowProcW(hWnd, uMsg, wParam, lParam);

    if (uMsg == WM_LBUTTONDOWN) {
        if (GetFocus() != hWnd) {
            SetFocus(hWnd);
            SendMessageW(hWnd, EM_SETSEL, 0, -1);
            return 0; // Prevent default mouse down from clearing selection
        }
    }
    else if (uMsg == WM_SETFOCUS) {
        LRESULT res = CallWindowProc(oldProc, hWnd, uMsg, wParam, lParam);
        SendMessageW(hWnd, EM_SETSEL, 0, -1);
        return res;
    }

    if (uMsg == WM_CHAR) {
        // Suppress DEL block char when pressing Ctrl+Backspace (ASCII 127)
        if (wParam == 127 || wParam == 0x7F) {
            return 0;
        }
    }
    else if (uMsg == WM_KEYDOWN) {
        int key = (int)wParam;
        bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;

        if (key == VK_BACK && ctrl) {
            // Delete previous word on Ctrl+Backspace
            DWORD startSel = 0, endSel = 0;
            SendMessageW(hWnd, EM_GETSEL, (WPARAM)&startSel, (LPARAM)&endSel);

            if (startSel != endSel) {
                SendMessageW(hWnd, EM_REPLACESEL, TRUE, (LPARAM)L"");
            } else if (startSel > 0) {
                int len = GetWindowTextLengthW(hWnd);
                wchar_t* text = (wchar_t*)malloc((len + 1) * sizeof(wchar_t));
                if (text) {
                    GetWindowTextW(hWnd, text, len + 1);
                    int pos = (int)startSel;

                    // 1. Skip spaces backwards
                    while (pos > 0 && iswspace(text[pos - 1])) {
                        pos--;
                    }
                    // 2. Skip alphanumeric characters backwards
                    if (pos > 0) {
                        if (iswalnum(text[pos - 1])) {
                            while (pos > 0 && iswalnum(text[pos - 1])) {
                                pos--;
                            }
                        } else {
                            pos--; // Skip single punctuation
                        }
                    }

                    SendMessageW(hWnd, EM_SETSEL, pos, startSel);
                    SendMessageW(hWnd, EM_REPLACESEL, TRUE, (LPARAM)L"");
                    free(text);
                }
            }
            return 0; // Handled
        }
    }
    return CallWindowProc(oldProc, hWnd, uMsg, wParam, lParam);
}

// ListView Control subclass WndProc
static LRESULT CALLBACK ListSubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    SearchViewData* data = (SearchViewData*)GetWindowLongPtr(GetParent(hWnd), GWLP_USERDATA);
    WNDPROC oldProc = data ? data->oldListWndProc : NULL;
    if (!oldProc) return DefWindowProcW(hWnd, uMsg, wParam, lParam);

    if (uMsg == WM_KEYDOWN) {
        int key = (int)wParam;
        bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;

        if (ctrl) {
            if (key == 'C' || key == 'c') {
                SearchView_ExportResults(GetParent(hWnd), NULL); // Trigger standard clipboard copy
                return 0;
            }
            else if (key == 'X' || key == 'x') {
                // Cut (Export selection with isCut = true)
                int selIdx = -1;
                size_t count = 0;
                while ((selIdx = ListView_GetNextItem(hWnd, selIdx, LVNI_SELECTED)) != -1) {
                    count++;
                }
                if (count > 0) {
                    const wchar_t** files = (const wchar_t**)malloc(count * sizeof(wchar_t*));
                    if (files) {
                        int idx = -1;
                        size_t k = 0;
                        while ((idx = ListView_GetNextItem(hWnd, idx, LVNI_SELECTED)) != -1) {
                            wchar_t* path = (wchar_t*)malloc(MAX_PATH * sizeof(wchar_t));
                            if (path) {
                                SearchEngine_GetResultFullPath(data->searchEngine, &data->results.data[idx], path, MAX_PATH);
                                files[k++] = path;
                            }
                        }
                        ShellIntegration_CopyFilesToClipboard(data->hWnd, files, k, true);
                        for (size_t i = 0; i < k; i++) free((void*)files[i]);
                        free(files);
                    }
                }
                return 0;
            }
            else if (key == 'A' || key == 'a') {
                // Select all items
                int itemsCount = ListView_GetItemCount(hWnd);
                SendMessageW(hWnd, WM_SETREDRAW, FALSE, 0);
                for (int i = 0; i < itemsCount; i++) {
                    ListView_SetItemState(hWnd, i, LVIS_SELECTED, LVIS_SELECTED);
                }
                SendMessageW(hWnd, WM_SETREDRAW, TRUE, 0);
                InvalidateRect(hWnd, NULL, FALSE);
                return 0;
            }
        }
        else if (key == VK_RETURN) {
            // Run/execute active search result
            int selIdx = ListView_GetNextItem(hWnd, -1, LVNI_SELECTED);
            if (selIdx != -1 && selIdx < (int)data->results.count) {
                wchar_t path[MAX_PATH];
                SearchEngine_GetResultFullPath(data->searchEngine, &data->results.data[selIdx], path, MAX_PATH);
                ShellExecuteW(NULL, L"open", path, NULL, NULL, SW_SHOWNORMAL);
            }
            return 0;
        }
        else if (key == VK_DELETE) {
            bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            int count = 0;
            int idx = -1;
            while ((idx = ListView_GetNextItem(hWnd, idx, LVNI_SELECTED)) != -1) {
                count++;
            }
            if (count > 0) {
                wchar_t** paths = (wchar_t**)malloc(count * sizeof(wchar_t*));
                size_t totalLen = 0;
                if (paths) {
                    int sIdx = -1;
                    int k = 0;
                    while ((sIdx = ListView_GetNextItem(hWnd, sIdx, LVNI_SELECTED)) != -1) {
                        wchar_t* path = (wchar_t*)malloc(MAX_PATH * sizeof(wchar_t));
                        if (path) {
                            SearchEngine_GetResultFullPath(data->searchEngine, &data->results.data[sIdx], path, MAX_PATH);
                            paths[k++] = path;
                            totalLen += wcslen(path) + 1;
                        }
                    }
                    totalLen += 1;
                    wchar_t* doubleNullBuf = (wchar_t*)calloc(totalLen, sizeof(wchar_t));
                    if (doubleNullBuf) {
                        size_t offset = 0;
                        for (int i = 0; i < k; i++) {
                            wcscpy_s(doubleNullBuf + offset, totalLen - offset, paths[i]);
                            offset += wcslen(paths[i]) + 1;
                        }
                        doubleNullBuf[offset] = L'\0';
                        SHFILEOPSTRUCTW fileOp = { 0 };
                        fileOp.hwnd = GetParent(hWnd);
                        fileOp.wFunc = FO_DELETE;
                        fileOp.pFrom = doubleNullBuf;
                        fileOp.pTo = NULL;
                        fileOp.fFlags = FOF_NOCONFIRMMKDIR;
                        if (!shift) {
                            fileOp.fFlags |= FOF_ALLOWUNDO;
                        }
                        SHFileOperationW(&fileOp);
                        free(doubleNullBuf);
                    }
                    for (int i = 0; i < k; i++) free(paths[i]);
                    free(paths);
                }
            }
            return 0;
        }
    }
    return CallWindowProc(oldProc, hWnd, uMsg, wParam, lParam);
}

static unsigned int __stdcall SearchThreadProc(void* arg) {
    SearchViewData* data = (SearchViewData*)arg;
    
    while (!data->isTerminating) {
        // Wait for search signal or termination
        DWORD waitRes = WaitForSingleObject(data->hSearchEvent, INFINITE);
        if (waitRes != WAIT_OBJECT_0 || data->isTerminating) {
            break;
        }

        // Reset event immediately
        ResetEvent(data->hSearchEvent);

        // Copy inputs under lock
        EnterCriticalSection(&data->searchInputMutex);
        wchar_t* query = data->pendingQuery ? _wcsdup(data->pendingQuery) : _wcsdup(L"");
        MatchMode mode = data->pendingMode;
        wchar_t driveLetter = data->pendingDrive;
        FilterType filter = data->pendingFilter;
        LeaveCriticalSection(&data->searchInputMutex);

        InterlockedExchange(&data->cancelSearch, 0);
        InterlockedExchange(&data->isSearching, 1);

        LARGE_INTEGER freq, start, end;
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&start);

        // Execute the search with cancel support
        SearchResultList tempResults;
        DYNARRAY_INIT(tempResults);

        StringMatcher threadMatcher;
        StringMatcher_Init(&threadMatcher);
        StringMatcher_SetPattern(&threadMatcher, query, mode, false);

        bool success = SearchEngine_ExecuteSearch(
            data->searchEngine,
            &threadMatcher,
            driveLetter,
            filter,
            &tempResults,
            &data->cancelSearch
        );

        free(query);

        if (success && !InterlockedOr(&data->cancelSearch, 0)) {
            // Apply sorting if enabled on the background thread!
            if (data->sortColumn != -1 && tempResults.count > 0) {
                SearchEngine_LockDrivesShared(data->searchEngine);
                g_SortData = data;
                qsort(tempResults.data, tempResults.count, sizeof(SearchResult), CompareResults);
                g_SortData = NULL;
                SearchEngine_UnlockDrivesShared(data->searchEngine);
            }

            QueryPerformanceCounter(&end);
            double elapsedMs = (double)(end.QuadPart - start.QuadPart) * 1000.0 / (double)freq.QuadPart;

            // Lock view results and swap atomically
            EnterCriticalSection(&data->searchInputMutex);
            DYNARRAY_FREE(data->results);
            data->results.data = tempResults.data;
            data->results.count = tempResults.count;
            data->results.capacity = tempResults.capacity;
            data->lastSearchTimeMs = elapsedMs;

            // Swap highlightMatcher
            StringMatcher_Free(&data->highlightMatcher);
            data->highlightMatcher = threadMatcher;
            LeaveCriticalSection(&data->searchInputMutex);

            // Post completion back to UI thread
            PostMessageW(data->hWnd, WM_SEARCH_COMPLETE, 0, 0);
        } else {
            // Aborted: Clean up temp results and threadMatcher
            DYNARRAY_FREE(tempResults);
            StringMatcher_Free(&threadMatcher);
        }

        InterlockedExchange(&data->isSearching, 0);
    }
    return 0;
}

// Global search result sorting comparator
// (g_SortData is declared as static at the top of the file)

static int CompareResults(const void* a, const void* b) {
    const SearchResult* resA = (const SearchResult*)a;
    const SearchResult* resB = (const SearchResult*)b;
    SearchViewData* data = g_SortData;
    if (!data) return 0;

    // Always keep standard system '$' metadata files at the end
    bool aStartsWithDollar = (resA->Name[0] == L'$');
    bool bStartsWithDollar = (resB->Name[0] == L'$');
    if (aStartsWithDollar != bStartsWithDollar) {
        return aStartsWithDollar ? 1 : -1;
    }

    int flag = data->visibleColumns[data->sortColumn].subitemIndex;
    int res = 0;

    switch (flag) {
        case COL_NAME:
            res = _wcsicmp(resA->Name, resB->Name);
            break;
        case COL_PATH: {
            wchar_t pathA[MAX_PATH];
            wchar_t pathB[MAX_PATH];
            SearchEngine_GetResultFullPath(data->searchEngine, resA, pathA, MAX_PATH);
            SearchEngine_GetResultFullPath(data->searchEngine, resB, pathB, MAX_PATH);
            res = _wcsicmp(pathA, pathB);
            break;
        }
        case COL_SIZE:
            res = resA->Size < resB->Size ? -1 : (resA->Size > resB->Size ? 1 : 0);
            break;
        case COL_SIZE_DISK:
            res = resA->SizeOnDisk < resB->SizeOnDisk ? -1 : (resA->SizeOnDisk > resB->SizeOnDisk ? 1 : 0);
            break;
        case COL_MODIFIED:
            res = resA->DateModified < resB->DateModified ? -1 : (resA->DateModified > resB->DateModified ? 1 : 0);
            break;
        case COL_CREATED:
            res = resA->DateCreated < resB->DateCreated ? -1 : (resA->DateCreated > resB->DateCreated ? 1 : 0);
            break;
        case COL_ACCESSED:
            res = resA->DateAccessed < resB->DateAccessed ? -1 : (resA->DateAccessed > resB->DateAccessed ? 1 : 0);
            break;
        case COL_ATTRIBUTES:
            res = resA->Attributes < resB->Attributes ? -1 : (resA->Attributes > resB->Attributes ? 1 : 0);
            break;
    }

    return data->sortAscending ? res : -res;
}

// Run search query and display results in Virtual ListView
static void RunSearchInternal(SearchViewData* data) {
    // Get search edit box text
    int len = GetWindowTextLengthW(data->searchEdit);
    wchar_t* query = (wchar_t*)malloc((len + 1) * sizeof(wchar_t));
    if (query) {
        GetWindowTextW(data->searchEdit, query, len + 1);
    } else {
        query = _wcsdup(L"");
    }

    FilterType filter = (FilterType)SendMessageW(data->filterCombo, CB_GETCURSEL, 0, 0);

    // Resolve active drive letter filter selection
    wchar_t driveLetter = L'\0';
    int volSel = (int)SendMessageW(data->volumeCombo, CB_GETCURSEL, 0, 0);
    if (volSel > 0) {
        wchar_t indexedDrives[26];
        int count = SearchEngine_GetIndexedDrives(data->searchEngine, indexedDrives, 26);
        if (volSel - 1 < count) {
            driveLetter = indexedDrives[volSel - 1];
        }
    }

    MatchMode mode = MatchMode_PlainText;
    if (SendMessageW(data->regexCheck, BM_GETCHECK, 0, 0) == BST_CHECKED) {
        mode = MatchMode_Regex;
    } else if (wcspbrk(query, L"*?") != NULL) {
        mode = MatchMode_Wildcard;
    }

    // Signal cancellation of any running background search immediately
    InterlockedExchange(&data->cancelSearch, 1);

    // Update pending inputs under lock
    EnterCriticalSection(&data->searchInputMutex);
    if (data->pendingQuery) free(data->pendingQuery);
    data->pendingQuery = query;
    data->pendingMode = mode;
    data->pendingDrive = driveLetter;
    data->pendingFilter = filter;
    LeaveCriticalSection(&data->searchInputMutex);

    // Wake up the search worker thread
    SetEvent(data->hSearchEvent);
}

// Helpers to format files attributes, sizes, and times
static void FormatFileSize(unsigned long long size, wchar_t* outBuf, size_t maxChars) {
    if (size == 0) {
        wcscpy_s(outBuf, maxChars, L"0 KB");
        return;
    }
    double sizeKb = ceil((double)size / 1024.0);
    
    // Windows API helper to group numbers with comma separators
    NUMBERFMTW fmt;
    fmt.NumDigits = 0;
    fmt.LeadingZero = 0;
    fmt.Grouping = 3;
    fmt.lpDecimalSep = L".";
    fmt.lpThousandSep = L",";
    fmt.NegativeOrder = 1;

    wchar_t rawNum[64];
    swprintf_s(rawNum, 64, L"%.0f", sizeKb);

    wchar_t groupedNum[64];
    if (GetNumberFormatW(LOCALE_USER_DEFAULT, 0, rawNum, &fmt, groupedNum, 64) > 0) {
        swprintf_s(outBuf, maxChars, L"%s KB", groupedNum);
    } else {
        swprintf_s(outBuf, maxChars, L"%.0f KB", sizeKb);
    }
}

static void FormatFileTime(unsigned long long time, wchar_t* outBuf, size_t maxChars) {
    if (time == 0) {
        outBuf[0] = L'\0';
        return;
    }
    FILETIME ft;
    ft.dwHighDateTime = (DWORD)(time >> 32);
    ft.dwLowDateTime = (DWORD)(time & 0xFFFFFFFF);

    SYSTEMTIME st, localSt;
    if (FileTimeToSystemTime(&ft, &st)) {
        if (SystemTimeToTzSpecificLocalTime(NULL, &st, &localSt)) {
            swprintf_s(outBuf, maxChars, L"%04d-%02d-%02d %02d:%02d:%02d", 
                       localSt.wYear, localSt.wMonth, localSt.wDay, 
                       localSt.wHour, localSt.wMinute, localSt.wSecond);
            return;
        }
    }
    outBuf[0] = L'\0';
}

static void FormatAttributes(unsigned int attribs, wchar_t* outBuf, size_t maxChars) {
    size_t idx = 0;
    if (attribs & FILE_ATTRIBUTE_DIRECTORY) outBuf[idx++] = L'D';
    if (attribs & FILE_ATTRIBUTE_READONLY)  outBuf[idx++] = L'R';
    if (attribs & FILE_ATTRIBUTE_HIDDEN)    outBuf[idx++] = L'H';
    if (attribs & FILE_ATTRIBUTE_SYSTEM)    outBuf[idx++] = L'S';
    if (attribs & FILE_ATTRIBUTE_ARCHIVE)   outBuf[idx++] = L'A';
    if (attribs & FILE_ATTRIBUTE_COMPRESSED) outBuf[idx++] = L'C';
    if (attribs & FILE_ATTRIBUTE_ENCRYPTED)  outBuf[idx++] = L'E';
    if (attribs & FILE_ATTRIBUTE_SPARSE_FILE) outBuf[idx++] = L'S';
    if (attribs & FILE_ATTRIBUTE_REPARSE_POINT) outBuf[idx++] = L'L';
    outBuf[idx] = L'\0';
    (void)maxChars;
}

// SearchView custom window class procedure
static LRESULT CALLBACK SearchViewWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    SearchViewData* data = (SearchViewData*)GetWindowLongPtr(hWnd, GWLP_USERDATA);

    switch (uMsg) {
        case WM_SEARCH_COMPLETE: {
            if (!data) return 0;
            EnterCriticalSection(&data->searchInputMutex);
            int count = (int)data->results.count;
            LeaveCriticalSection(&data->searchInputMutex);

            ListView_SetItemCount(data->listView, count);
            if (count > 0) {
                ListView_RedrawItems(data->listView, 0, count - 1);
            }
            InvalidateRect(data->listView, NULL, FALSE);
            UpdateWindow(data->listView);

            if (IsWindow(GetParent(data->hWnd))) {
                PostMessageW(GetParent(data->hWnd), WM_SEARCH_RESULTS_CHANGED, 0, 0);
            }
            return 0;
        }
        case WM_CREATE: {
            data = (SearchViewData*)malloc(sizeof(SearchViewData));
            if (!data) return -1;
            memset(data, 0, sizeof(SearchViewData));
            data->hWnd = hWnd;
            data->sortColumn = -1;
            data->sortAscending = true;
            data->viewMode = ID_VIEW_DETAILS;
            data->columnMask = COL_DEFAULT;
            DYNARRAY_INIT(data->results);

            InitializeCriticalSection(&data->searchInputMutex);
            data->hSearchEvent = CreateEventW(NULL, TRUE, FALSE, NULL); // manual reset
            data->pendingQuery = NULL;
            data->cancelSearch = 0;
            data->isSearching = 0;
            data->isTerminating = false;
            StringMatcher_Init(&data->highlightMatcher);

            // Spawn background search thread
            data->hSearchThread = (HANDLE)_beginthreadex(NULL, 0, SearchThreadProc, data, 0, NULL);

            CREATESTRUCTW* pcs = (CREATESTRUCTW*)lParam;
            data->searchEngine = (SearchEngine*)pcs->lpCreateParams;

            SetWindowLongPtr(hWnd, GWLP_USERDATA, (LONG_PTR)data);

            // 1. Create Control bar inputs
            data->volumeCombo = CreateWindowExW(
                0, L"COMBOBOX", NULL,
                WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_TABSTOP,
                0, 0, 90, 150,
                hWnd, (HMENU)IDC_VOLUME_COMBO, GetModuleHandle(NULL), NULL
            );
            SendMessageW(data->volumeCombo, CB_ADDSTRING, 0, (LPARAM)L"All Drives");
            SendMessageW(data->volumeCombo, CB_SETCURSEL, 0, 0);

            data->searchEdit = CreateWindowExW(
                WS_EX_CLIENTEDGE, L"EDIT", NULL,
                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | WS_TABSTOP,
                0, 0, 200, 22,
                hWnd, (HMENU)IDC_SEARCH_EDIT, GetModuleHandle(NULL), NULL
            );
            // Setting visual Cue banner
            SendMessageW(data->searchEdit, EM_SETCUEBANNER, TRUE, (LPARAM)L"Type to search...");

            data->filterCombo = CreateWindowExW(
                0, L"COMBOBOX", NULL,
                WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_TABSTOP,
                0, 0, 145, 150,
                hWnd, (HMENU)IDC_FILTER_COMBO, GetModuleHandle(NULL), NULL
            );
            SendMessageW(data->filterCombo, CB_ADDSTRING, 0, (LPARAM)L"All Files and Folders");
            SendMessageW(data->filterCombo, CB_ADDSTRING, 0, (LPARAM)L"Folders Only");
            SendMessageW(data->filterCombo, CB_ADDSTRING, 0, (LPARAM)L"Files Only");
            SendMessageW(data->filterCombo, CB_ADDSTRING, 0, (LPARAM)L"Documents");
            SendMessageW(data->filterCombo, CB_ADDSTRING, 0, (LPARAM)L"Executables");
            SendMessageW(data->filterCombo, CB_ADDSTRING, 0, (LPARAM)L"Pictures");
            SendMessageW(data->filterCombo, CB_ADDSTRING, 0, (LPARAM)L"Audio");
            SendMessageW(data->filterCombo, CB_ADDSTRING, 0, (LPARAM)L"Video");
            SendMessageW(data->filterCombo, CB_SETCURSEL, 0, 0);

            data->caseCheck = CreateWindowExW(
                0, L"BUTTON", L"Match Case",
                WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
                0, 0, 85, 22,
                hWnd, (HMENU)IDC_CASE_CHECK, GetModuleHandle(NULL), NULL
            );

            data->regexCheck = CreateWindowExW(
                0, L"BUTTON", L"RegEx",
                WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
                0, 0, 55, 22,
                hWnd, (HMENU)IDC_REGEX_CHECK, GetModuleHandle(NULL), NULL
            );

            // Apply modern Segoe UI font to inputs
            HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
            SendMessageW(data->volumeCombo, WM_SETFONT, (WPARAM)hFont, TRUE);
            SendMessageW(data->searchEdit, WM_SETFONT, (WPARAM)hFont, TRUE);
            SendMessageW(data->filterCombo, WM_SETFONT, (WPARAM)hFont, TRUE);
            SendMessageW(data->caseCheck, WM_SETFONT, (WPARAM)hFont, TRUE);
            SendMessageW(data->regexCheck, WM_SETFONT, (WPARAM)hFont, TRUE);

            // 2. High-performance Virtual ListView
            data->listView = CreateWindowExW(
                WS_EX_CLIENTEDGE, WC_LISTVIEWW, NULL,
                WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_OWNERDATA | LVS_SHOWSELALWAYS | LVS_SHAREIMAGELISTS | WS_TABSTOP,
                0, 0, 400, 300,
                hWnd, (HMENU)IDC_RESULTS_LIST, GetModuleHandle(NULL), NULL
            );

            // Attach Aero gridlines and double-buffering
            ListView_SetExtendedListViewStyle(data->listView, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);

            // Attach system small and large ImageLists
            SHFILEINFOW sfi;
            HIMAGELIST hSmallImgList = (HIMAGELIST)SHGetFileInfoW(L"C:\\", 0, &sfi, sizeof(sfi), SHGFI_SYSICONINDEX | SHGFI_SMALLICON);
            if (hSmallImgList) {
                ListView_SetImageList(data->listView, hSmallImgList, LVSIL_SMALL);
            }
            HIMAGELIST hLargeImgList = (HIMAGELIST)SHGetFileInfoW(L"C:\\", 0, &sfi, sizeof(sfi), SHGFI_SYSICONINDEX | SHGFI_LARGEICON);
            if (hLargeImgList) {
                ListView_SetImageList(data->listView, hLargeImgList, LVSIL_NORMAL);
            }

            // Subclass Search Edit box and ListView
            data->oldEditWndProc = (WNDPROC)SetWindowLongPtr(data->searchEdit, GWLP_WNDPROC, (LONG_PTR)EditSubclassProc);
            data->oldListWndProc = (WNDPROC)SetWindowLongPtr(data->listView, GWLP_WNDPROC, (LONG_PTR)ListSubclassProc);

            SearchView_UpdateColumns(hWnd, data->columnMask);

            return 0;
        }
        case WM_SIZE: {
            if (!data) return 0;
            int cx = LOWORD(lParam);
            int cy = HIWORD(lParam);
            if (cx == 0 || cy == 0) return 0;

            int padding = 6;
            int topBarHeight = 30;
            int volWidth = 90;
            int filterWidth = 145;
            int checkCaseWidth = 85;
            int checkRegexWidth = 55;

            int editWidth = cx - (volWidth + filterWidth + checkCaseWidth + checkRegexWidth + (padding * 5));
            if (editWidth < 50) editWidth = 50;

            int currentX = padding;
            MoveWindow(data->volumeCombo, currentX, padding, volWidth, 150, TRUE);
            currentX += volWidth + padding;

            MoveWindow(data->searchEdit, currentX, padding, editWidth, 22, TRUE);
            currentX += editWidth + padding;

            MoveWindow(data->filterCombo, currentX, padding, filterWidth, 150, TRUE);
            currentX += filterWidth + padding;

            MoveWindow(data->caseCheck, currentX, padding, checkCaseWidth, 22, TRUE);
            currentX += checkCaseWidth + padding;

            MoveWindow(data->regexCheck, currentX, padding, checkRegexWidth, 22, TRUE);

            int listY = topBarHeight + (padding * 2);
            MoveWindow(data->listView, 0, listY, cx, cy - listY, TRUE);

            return 0;
        }
        case WM_SETFOCUS: {
            if (data) SetFocus(data->searchEdit);
            return 0;
        }
        case WM_COMMAND: {
            if (!data) return 0;
            WORD id = LOWORD(wParam);
            WORD code = HIWORD(wParam);

            if (id == IDC_SEARCH_EDIT && code == EN_CHANGE) {
                // Typings debounce/throttle: 40ms
                KillTimer(hWnd, 1);
                SetTimer(hWnd, 1, 40, NULL);
            }
            else if ((id == IDC_VOLUME_COMBO || id == IDC_FILTER_COMBO) && code == CBN_SELCHANGE) {
                RunSearchInternal(data);
            }
            else if (id == IDC_CASE_CHECK || id == IDC_REGEX_CHECK) {
                RunSearchInternal(data);
            }
            break;
        }
        case WM_TIMER: {
            if (!data) return 0;
            if (wParam == 1) {
                KillTimer(hWnd, 1);
                RunSearchInternal(data);
            }
            else if (wParam == 2) {
                KillTimer(hWnd, 2);
                RunSearchInternal(data);
            }
            break;
        }
        case WM_NTFS_INDEX_CHANGED: {
            if (!data) return 0;
            // Hot-reload throttle: 500ms debounce
            KillTimer(hWnd, 2);
            SetTimer(hWnd, 2, 500, NULL);
            break;
        }
        case WM_NOTIFY: {
            if (!data) return 0;
            LPNMHDR pnmh = (LPNMHDR)lParam;

            if (pnmh->hwndFrom == data->listView) {
                switch (pnmh->code) {
                    case LVN_GETDISPINFOW: {
                        NMLVDISPINFO* pDispInfo = (NMLVDISPINFO*)lParam;
                        int idx = pDispInfo->item.iItem;
                        if (idx < 0 || idx >= (int)data->results.count) return 0;

                        const SearchResult* res = &data->results.data[idx];

                        if (pDispInfo->item.mask & LVIF_TEXT) {
                            int subItem = pDispInfo->item.iSubItem;
                            if (subItem >= 0 && subItem < data->visibleColumnsCount) {
                                int flag = data->visibleColumns[subItem].subitemIndex;
                                wchar_t temp[MAX_PATH] = { 0 };

                                switch (flag) {
                                    case COL_NAME:
                                        wcsncpy_s(temp, MAX_PATH, res->Name, _TRUNCATE);
                                        break;
                                    case COL_PATH:
                                        SearchEngine_GetResultFullPath(data->searchEngine, res, temp, MAX_PATH);
                                        // Strip filename to show parent directory folder only
                                        wchar_t* lastSlash = wcsrchr(temp, L'\\');
                                        if (lastSlash) *lastSlash = L'\0';
                                        break;
                                    case COL_SIZE:
                                        if (!res->IsDirectory) FormatFileSize(res->Size, temp, MAX_PATH);
                                        break;
                                    case COL_SIZE_DISK:
                                        if (!res->IsDirectory) FormatFileSize(res->SizeOnDisk, temp, MAX_PATH);
                                        break;
                                    case COL_MODIFIED:
                                        FormatFileTime(res->DateModified, temp, MAX_PATH);
                                        break;
                                    case COL_CREATED:
                                        FormatFileTime(res->DateCreated, temp, MAX_PATH);
                                        break;
                                    case COL_ACCESSED:
                                        FormatFileTime(res->DateAccessed, temp, MAX_PATH);
                                        break;
                                    case COL_ATTRIBUTES:
                                        FormatAttributes(res->Attributes, temp, MAX_PATH);
                                        break;
                                }
                                wcsncpy_s(pDispInfo->item.pszText, pDispInfo->item.cchTextMax, temp, _TRUNCATE);
                            }
                        }

                        if (pDispInfo->item.mask & LVIF_IMAGE) {
                            SHFILEINFOW sfiLocal;
                            DWORD attribs = res->IsDirectory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
                            
                            DWORD dwStyle = GetWindowLong(data->listView, GWL_STYLE);
                            DWORD dwView = dwStyle & LVS_TYPEMASK;
                            DWORD iconFlags = SHGFI_SYSICONINDEX;
                            if (dwView == LVS_ICON) iconFlags |= SHGFI_LARGEICON;
                            else iconFlags |= SHGFI_SMALLICON;

                            wchar_t fullPath[MAX_PATH];
                            SearchEngine_GetResultFullPath(data->searchEngine, res, fullPath, MAX_PATH);

                            if (GetFileAttributesW(fullPath) == INVALID_FILE_ATTRIBUTES) {
                                iconFlags |= SHGFI_USEFILEATTRIBUTES;
                                SHGetFileInfoW(res->Name, attribs, &sfiLocal, sizeof(sfiLocal), iconFlags);
                            } else {
                                SHGetFileInfoW(fullPath, attribs, &sfiLocal, sizeof(sfiLocal), iconFlags);
                            }
                            pDispInfo->item.iImage = sfiLocal.iIcon;
                        }

                        return 0;
                    }
                    case LVN_COLUMNCLICK: {
                        LPNMLISTVIEW pnmv = (LPNMLISTVIEW)lParam;
                        int clickedCol = pnmv->iSubItem;

                        if (clickedCol == data->sortColumn) {
                            data->sortAscending = !data->sortAscending;
                        } else {
                            data->sortColumn = clickedCol;
                            data->sortAscending = true;
                        }

                        RunSearchInternal(data);
                        return 0;
                    }
                    case NM_DBLCLK: {
                        int selIdx = ListView_GetNextItem(data->listView, -1, LVNI_SELECTED);
                        if (selIdx == -1) return 0;

                        const SearchResult* res = &data->results.data[selIdx];
                        wchar_t fullPath[MAX_PATH];
                        SearchEngine_GetResultFullPath(data->searchEngine, res, fullPath, MAX_PATH);

                        // Subitem hit-testing to see what column was double-clicked
                        DWORD pos = GetMessagePos();
                        POINT pt;
                        pt.x = GET_X_LPARAM(pos);
                        pt.y = GET_Y_LPARAM(pos);
                        ScreenToClient(data->listView, &pt);

                        LVHITTESTINFO hti;
                        hti.pt = pt;
                        ListView_SubItemHitTest(data->listView, &hti);

                        if (hti.iSubItem >= 0 && hti.iSubItem < data->visibleColumnsCount && 
                            data->visibleColumns[hti.iSubItem].subitemIndex == COL_PATH) {
                            // Double-clicked path: open containing folder and select the item
                            wchar_t arg[512];
                            swprintf_s(arg, 512, L"/select,\"%s\"", fullPath);
                            ShellExecuteW(NULL, L"open", L"explorer.exe", arg, NULL, SW_SHOWNORMAL);
                        } else {
                            // Double-clicked filename: launch it standard
                            ShellExecuteW(NULL, L"open", fullPath, NULL, NULL, SW_SHOWNORMAL);
                        }
                        return 0;
                    }
                    case LVN_BEGINDRAG: {
                        // Collect all selected files
                        int count = 0;
                        int idx = -1;
                        while ((idx = ListView_GetNextItem(data->listView, idx, LVNI_SELECTED)) != -1) {
                            count++;
                        }
                        if (count > 0) {
                            const wchar_t** files = (const wchar_t**)malloc(count * sizeof(wchar_t*));
                            if (files) {
                                int sIdx = -1;
                                size_t k = 0;
                                while ((sIdx = ListView_GetNextItem(data->listView, sIdx, LVNI_SELECTED)) != -1) {
                                    wchar_t* path = (wchar_t*)malloc(MAX_PATH * sizeof(wchar_t));
                                    if (path) {
                                        SearchEngine_GetResultFullPath(data->searchEngine, &data->results.data[sIdx], path, MAX_PATH);
                                        files[k++] = path;
                                    }
                                }
                                DragDrop_CopyFilesToClipboardOrDrag(data->hWnd, files, k, true);
                                for (size_t i = 0; i < k; i++) free((void*)files[i]);
                                free(files);
                            }
                        }
                        return 0;
                    }
                    case NM_CUSTOMDRAW: {
                        LPNMLVCUSTOMDRAW pLVCD = (LPNMLVCUSTOMDRAW)lParam;
                        
                        DWORD dwStyle = GetWindowLong(data->listView, GWL_STYLE);
                        if ((dwStyle & LVS_TYPEMASK) != LVS_REPORT) {
                            return CDRF_DODEFAULT;
                        }

                        switch (pLVCD->nmcd.dwDrawStage) {
                            case CDDS_PREPAINT:
                                return CDRF_NOTIFYITEMDRAW;

                            case CDDS_ITEMPREPAINT:
                                return CDRF_NOTIFYSUBITEMDRAW;

                            case CDDS_SUBITEM | CDDS_ITEMPREPAINT: {
                                int itemIdx = (int)pLVCD->nmcd.dwItemSpec;
                                int subItemIdx = pLVCD->iSubItem;

                                if (itemIdx < 0 || itemIdx >= (int)data->results.count ||
                                    subItemIdx < 0 || subItemIdx >= data->visibleColumnsCount) {
                                    return CDRF_DODEFAULT;
                                }

                                int flag = data->visibleColumns[subItemIdx].subitemIndex;

                                bool isSelected = (ListView_GetItemState(data->listView, itemIdx, LVIS_SELECTED) & LVIS_SELECTED) != 0;
                                bool isFocused = (GetFocus() == data->listView);
                                bool isHot = (ListView_GetHotItem(data->listView) == itemIdx);

                                COLORREF clrBg = isSelected ? GetSysColor(isFocused ? COLOR_HIGHLIGHT : COLOR_BTNFACE) :
                                                 (isHot ? RGB(229, 243, 255) : GetSysColor(COLOR_WINDOW));
                                COLORREF clrText = isSelected ? GetSysColor(isFocused ? COLOR_HIGHLIGHTTEXT : COLOR_BTNTEXT) :
                                                   GetSysColor(COLOR_WINDOWTEXT);

                                if (flag == COL_NAME) {
                                    HDC hdc = pLVCD->nmcd.hdc;
                                    RECT rc = pLVCD->nmcd.rc;

                                    // Manually fill background
                                    HBRUSH hBgBrush = CreateSolidBrush(clrBg);
                                    FillRect(hdc, &rc, hBgBrush);
                                    DeleteObject(hBgBrush);

                                    const SearchResult* res = &data->results.data[itemIdx];

                                    // Retrieve small icon
                                    SHFILEINFOW sfiLocal;
                                    DWORD attribs = res->IsDirectory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
                                    SHGetFileInfoW(res->Name, attribs, &sfiLocal, sizeof(sfiLocal), 
                                                   SHGFI_SYSICONINDEX | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES);

                                    HIMAGELIST hSmallImgList = ListView_GetImageList(data->listView, LVSIL_SMALL);
                                    if (hSmallImgList && sfiLocal.iIcon >= 0) {
                                        int iconX = rc.left + 2;
                                        int iconY = rc.top + (rc.bottom - rc.top - 16) / 2;
                                        ImageList_Draw(hSmallImgList, sfiLocal.iIcon, hdc, iconX, iconY, ILD_TRANSPARENT);
                                    }

                                    COLORREF clrHighlightText = RGB(0, 0, 0); // black text
                                    COLORREF clrHighlightBg = RGB(255, 235, 156); // soft yellow

                                    HFONT hOldFont = (HFONT)SelectObject(hdc, (HFONT)SendMessageW(data->listView, WM_GETFONT, 0, 0));

                                    rc.left += 24; // padding offset for icon

                                    // Split words for text term highlights
                                    const wchar_t* name = res->Name;
                                    size_t nameLen = wcslen(name);
                                    
                                    wchar_t* nameLower = (wchar_t*)malloc((nameLen + 1) * sizeof(wchar_t));
                                    if (nameLower) {
                                        for (size_t i = 0; i < nameLen; i++) nameLower[i] = towlower(name[i]);
                                        nameLower[nameLen] = L'\0';
                                    }

                                    bool* highlighted = (bool*)calloc(nameLen + 1, sizeof(bool));

                                    if (nameLower && highlighted) {
                                        EnterCriticalSection(&data->searchInputMutex);
                                        const StringMatcher* matcher = &data->highlightMatcher;
                                        for (int w = 0; w < matcher->wordsCount; w++) {
                                            const wchar_t* wL = matcher->wordsLower[w];
                                            size_t wLen = wcslen(wL);
                                            if (wLen == 0) continue;

                                            wchar_t* p = wcsstr(nameLower, wL);
                                            while (p != NULL) {
                                                size_t start = p - nameLower;
                                                for (size_t idx = 0; idx < wLen && start + idx < nameLen; idx++) {
                                                    highlighted[start + idx] = true;
                                                }
                                                p = wcsstr(p + 1, wL);
                                            }
                                        }
                                        LeaveCriticalSection(&data->searchInputMutex);
                                    }

                                    // Print highlighted segments
                                    int x = rc.left;
                                    if (nameLen > 0 && highlighted) {
                                        bool currentState = highlighted[0];
                                        size_t start = 0;
                                        for (size_t i = 1; i < nameLen; i++) {
                                            if (highlighted[i] != currentState) {
                                                size_t segLen = i - start;
                                                SIZE sz;
                                                GetTextExtentPoint32W(hdc, name + start, (int)segLen, &sz);
                                                RECT rcSeg = { x, rc.top, x + sz.cx, rc.bottom };
                                                
                                                if (rcSeg.right > rc.right - 4) rcSeg.right = rc.right - 4;

                                                if (rcSeg.left < rcSeg.right) {
                                                    if (currentState) {
                                                        HBRUSH hHighlightBrush = CreateSolidBrush(clrHighlightBg);
                                                        FillRect(hdc, &rcSeg, hHighlightBrush);
                                                        DeleteObject(hHighlightBrush);
                                                        SetTextColor(hdc, clrHighlightText);
                                                    } else {
                                                        SetTextColor(hdc, clrText);
                                                    }
                                                    SetBkMode(hdc, TRANSPARENT);
                                                    DrawTextW(hdc, name + start, (int)segLen, &rcSeg, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
                                                }
                                                x += sz.cx;
                                                currentState = highlighted[i];
                                                start = i;
                                            }
                                        }
                                        // Print final segment
                                        size_t segLen = nameLen - start;
                                        SIZE sz;
                                        GetTextExtentPoint32W(hdc, name + start, (int)segLen, &sz);
                                        RECT rcSeg = { x, rc.top, x + sz.cx, rc.bottom };
                                        
                                        if (rcSeg.right > rc.right - 4) rcSeg.right = rc.right - 4;

                                        if (rcSeg.left < rcSeg.right) {
                                            if (currentState) {
                                                HBRUSH hHighlightBrush = CreateSolidBrush(clrHighlightBg);
                                                FillRect(hdc, &rcSeg, hHighlightBrush);
                                                DeleteObject(hHighlightBrush);
                                                SetTextColor(hdc, clrHighlightText);
                                            } else {
                                                SetTextColor(hdc, clrText);
                                            }
                                            SetBkMode(hdc, TRANSPARENT);
                                            DrawTextW(hdc, name + start, (int)segLen, &rcSeg, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
                                        }
                                    }

                                    if (nameLower) free(nameLower);
                                    if (highlighted) free(highlighted);

                                    SelectObject(hdc, hOldFont);
                                    return CDRF_SKIPDEFAULT;
                                } else {
                                    pLVCD->clrText = clrText;
                                    pLVCD->clrTextBk = clrBg;
                                    return CDRF_DODEFAULT;
                                }
                                break;
                            }
                        }
                        return CDRF_DODEFAULT;
                    }
                }
            }
            break;
        }
        case WM_CONTEXTMENU: {
            if (!data) return 0;
            HWND hWndSource = (HWND)wParam;
            if (hWndSource == data->listView) {
                POINT ptCursor;
                ptCursor.x = GET_X_LPARAM(lParam);
                ptCursor.y = GET_Y_LPARAM(lParam);

                if (lParam == -1) {
                    // Triggered by Keyboard Shift+F10
                    int selIdx = ListView_GetNextItem(data->listView, -1, LVNI_SELECTED);
                    if (selIdx != -1) {
                        RECT rcItem;
                        ListView_GetItemRect(data->listView, selIdx, &rcItem, LVIR_BOUNDS);
                        ptCursor.x = (rcItem.left + rcItem.right) / 2;
                        ptCursor.y = (rcItem.top + rcItem.bottom) / 2;
                        ClientToScreen(data->listView, &ptCursor);
                    } else {
                        RECT rcClient;
                        GetClientRect(data->listView, &rcClient);
                        ptCursor.x = (rcClient.left + rcClient.right) / 2;
                        ptCursor.y = (rcClient.top + rcClient.bottom) / 2;
                        ClientToScreen(data->listView, &ptCursor);
                    }
                } else {
                    POINT ptClient = ptCursor;
                    ScreenToClient(data->listView, &ptClient);
                    LVHITTESTINFO hti;
                    hti.pt = ptClient;
                    int idx = ListView_SubItemHitTest(data->listView, &hti);
                    if (idx != -1) {
                        if (!(ListView_GetItemState(data->listView, idx, LVIS_SELECTED) & LVIS_SELECTED)) {
                            ListView_SetItemState(data->listView, -1, 0, LVIS_SELECTED); // deselect all
                            ListView_SetItemState(data->listView, idx, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                        }
                    }
                }

                int selIdx = ListView_GetNextItem(data->listView, -1, LVNI_SELECTED);
                if (selIdx == -1) return 0;

                // Gather full paths
                int count = 0;
                int idx = -1;
                while ((idx = ListView_GetNextItem(data->listView, idx, LVNI_SELECTED)) != -1) {
                    count++;
                }
                if (count > 0) {
                    const wchar_t** files = (const wchar_t**)malloc(count * sizeof(wchar_t*));
                    if (files) {
                        int sIdx = -1;
                        size_t k = 0;
                        while ((sIdx = ListView_GetNextItem(data->listView, sIdx, LVNI_SELECTED)) != -1) {
                            wchar_t* path = (wchar_t*)malloc(MAX_PATH * sizeof(wchar_t));
                            if (path) {
                                SearchEngine_GetResultFullPath(data->searchEngine, &data->results.data[sIdx], path, MAX_PATH);
                                files[k++] = path;
                            }
                        }
                        ShellIntegration_ShowContextMenu(data->hWnd, files, k, ptCursor);
                        for (size_t i = 0; i < k; i++) free((void*)files[i]);
                        free(files);
                    }
                }
                return TRUE;
            }
            break;
        }
        case WM_DESTROY: {
            if (data) {
                // Signal search thread to exit safely
                data->isTerminating = true;
                InterlockedExchange(&data->cancelSearch, 1);
                SetEvent(data->hSearchEvent);

                if (data->hSearchThread) {
                    WaitForSingleObject(data->hSearchThread, 2000);
                    CloseHandle(data->hSearchThread);
                }
                if (data->hSearchEvent) {
                    CloseHandle(data->hSearchEvent);
                }

                DeleteCriticalSection(&data->searchInputMutex);
                if (data->pendingQuery) free(data->pendingQuery);

                // Restore subclasses
                SetWindowLongPtr(data->searchEdit, GWLP_WNDPROC, (LONG_PTR)data->oldEditWndProc);
                SetWindowLongPtr(data->listView, GWLP_WNDPROC, (LONG_PTR)data->oldListWndProc);

                DYNARRAY_FREE(data->results);
                StringMatcher_Free(&data->highlightMatcher);
                free(data);
                SetWindowLongPtr(hWnd, GWLP_USERDATA, 0);
            }
            break;
        }
    }
    return DefWindowProc(hWnd, uMsg, wParam, lParam);
}

bool SearchView_RegisterClass(void) {
    WNDCLASSEXW wcex = { 0 };
    wcex.cbSize = sizeof(WNDCLASSEXW);
    wcex.style = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc = SearchViewWndProc;
    wcex.cbClsExtra = 0;
    wcex.cbWndExtra = 0;
    wcex.hInstance = GetModuleHandle(NULL);
    wcex.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wcex.lpszClassName = L"CSearchViewClass";

    return RegisterClassExW(&wcex) != 0;
}

HWND SearchView_Create(HWND hwndParent, SearchEngine* engine) {
    return CreateWindowExW(
        0, L"CSearchViewClass", NULL,
        WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
        0, 0, 0, 0,
        hwndParent, NULL, GetModuleHandle(NULL), engine
    );
}

void SearchView_UpdateColumns(HWND hwndView, unsigned int columnMask) {
    SearchViewData* data = (SearchViewData*)GetWindowLongPtr(hwndView, GWLP_USERDATA);
    if (!data) return;

    data->columnMask = columnMask;

    // Clear existing columns
    HWND hHeader = ListView_GetHeader(data->listView);
    int count = Header_GetItemCount(hHeader);
    for (int i = count - 1; i >= 0; i--) {
        ListView_DeleteColumn(data->listView, i);
    }

    data->visibleColumnsCount = 0;
    
    // Add default Name and Path
    wcscpy_s(data->visibleColumns[data->visibleColumnsCount].text, 128, L"Name");
    data->visibleColumns[data->visibleColumnsCount].width = 220;
    data->visibleColumns[data->visibleColumnsCount].subitemIndex = COL_NAME;
    data->visibleColumnsCount++;

    wcscpy_s(data->visibleColumns[data->visibleColumnsCount].text, 128, L"Path");
    data->visibleColumns[data->visibleColumnsCount].width = 320;
    data->visibleColumns[data->visibleColumnsCount].subitemIndex = COL_PATH;
    data->visibleColumnsCount++;

    if (columnMask & COL_SIZE) {
        wcscpy_s(data->visibleColumns[data->visibleColumnsCount].text, 128, L"Size");
        data->visibleColumns[data->visibleColumnsCount].width = 90;
        data->visibleColumns[data->visibleColumnsCount].subitemIndex = COL_SIZE;
        data->visibleColumnsCount++;
    }
    if (columnMask & COL_SIZE_DISK) {
        wcscpy_s(data->visibleColumns[data->visibleColumnsCount].text, 128, L"Size on Disk");
        data->visibleColumns[data->visibleColumnsCount].width = 100;
        data->visibleColumns[data->visibleColumnsCount].subitemIndex = COL_SIZE_DISK;
        data->visibleColumnsCount++;
    }
    if (columnMask & COL_MODIFIED) {
        wcscpy_s(data->visibleColumns[data->visibleColumnsCount].text, 128, L"Date Modified");
        data->visibleColumns[data->visibleColumnsCount].width = 130;
        data->visibleColumns[data->visibleColumnsCount].subitemIndex = COL_MODIFIED;
        data->visibleColumnsCount++;
    }
    if (columnMask & COL_CREATED) {
        wcscpy_s(data->visibleColumns[data->visibleColumnsCount].text, 128, L"Date Created");
        data->visibleColumns[data->visibleColumnsCount].width = 130;
        data->visibleColumns[data->visibleColumnsCount].subitemIndex = COL_CREATED;
        data->visibleColumnsCount++;
    }
    if (columnMask & COL_ACCESSED) {
        wcscpy_s(data->visibleColumns[data->visibleColumnsCount].text, 128, L"Date Accessed");
        data->visibleColumns[data->visibleColumnsCount].width = 130;
        data->visibleColumns[data->visibleColumnsCount].subitemIndex = COL_ACCESSED;
        data->visibleColumnsCount++;
    }
    if (columnMask & COL_ATTRIBUTES) {
        wcscpy_s(data->visibleColumns[data->visibleColumnsCount].text, 128, L"Attributes");
        data->visibleColumns[data->visibleColumnsCount].width = 70;
        data->visibleColumns[data->visibleColumnsCount].subitemIndex = COL_ATTRIBUTES;
        data->visibleColumnsCount++;
    }

    // Insert columns in UI
    for (int i = 0; i < data->visibleColumnsCount; ++i) {
        int fmt = (data->visibleColumns[i].subitemIndex == COL_SIZE || data->visibleColumns[i].subitemIndex == COL_SIZE_DISK) ? 
                  LVCFMT_RIGHT : LVCFMT_LEFT;
        
        LVCOLUMNW lvc = { 0 };
        lvc.mask = LVCF_FMT | LVCF_WIDTH | LVCF_TEXT | LVCF_SUBITEM;
        lvc.fmt = fmt;
        lvc.cx = data->visibleColumns[i].width;
        lvc.pszText = data->visibleColumns[i].text;
        lvc.iSubItem = i;
        ListView_InsertColumn(data->listView, i, &lvc);
    }
}

void SearchView_TriggerSearch(HWND hwndView) {
    SearchViewData* data = (SearchViewData*)GetWindowLongPtr(hwndView, GWLP_USERDATA);
    if (data) RunSearchInternal(data);
}

size_t SearchView_GetResultCount(HWND hwndView) {
    SearchViewData* data = (SearchViewData*)GetWindowLongPtr(hwndView, GWLP_USERDATA);
    return data ? data->results.count : 0;
}

double SearchView_GetLastSearchTimeMs(HWND hwndView) {
    SearchViewData* data = (SearchViewData*)GetWindowLongPtr(hwndView, GWLP_USERDATA);
    return data ? data->lastSearchTimeMs : 0.0;
}

void SearchView_SetViewMode(HWND hwndView, int mode) {
    SearchViewData* data = (SearchViewData*)GetWindowLongPtr(hwndView, GWLP_USERDATA);
    if (!data) return;

    data->viewMode = mode;
    HIMAGELIST hImgList = NULL;
    
    // Set appropriate view style using Win32 API window styles
    DWORD dwStyle = GetWindowLong(data->listView, GWL_STYLE);
    dwStyle &= ~LVS_TYPEMASK;

    if (mode == ID_VIEW_DETAILS) {
        dwStyle |= LVS_REPORT;
        SetWindowLong(data->listView, GWL_STYLE, dwStyle);
        HRESULT hr = SHGetImageList(SHIL_SMALL, &IID_IImageList, (void**)&hImgList);
        if (SUCCEEDED(hr) && hImgList) {
            ListView_SetImageList(data->listView, hImgList, LVSIL_SMALL);
        }
    }
    else if (mode == ID_VIEW_SMALL_ICONS) {
        dwStyle |= LVS_SMALLICON;
        SetWindowLong(data->listView, GWL_STYLE, dwStyle);
        HRESULT hr = SHGetImageList(SHIL_SMALL, &IID_IImageList, (void**)&hImgList);
        if (SUCCEEDED(hr) && hImgList) {
            ListView_SetImageList(data->listView, hImgList, LVSIL_SMALL);
        }
    }
    else if (mode == ID_VIEW_MEDIUM_ICONS) {
        dwStyle |= LVS_ICON;
        SetWindowLong(data->listView, GWL_STYLE, dwStyle);
        HRESULT hr = SHGetImageList(SHIL_LARGE, &IID_IImageList, (void**)&hImgList);
        if (SUCCEEDED(hr) && hImgList) {
            ListView_SetImageList(data->listView, hImgList, LVSIL_NORMAL);
            ListView_SetIconSpacing(data->listView, 76, 76);
        }
    }
    else if (mode == ID_VIEW_LARGE_ICONS) {
        dwStyle |= LVS_ICON;
        SetWindowLong(data->listView, GWL_STYLE, dwStyle);
        HRESULT hr = SHGetImageList(SHIL_EXTRALARGE, &IID_IImageList, (void**)&hImgList);
        if (FAILED(hr)) hr = SHGetImageList(SHIL_LARGE, &IID_IImageList, (void**)&hImgList);
        if (SUCCEEDED(hr) && hImgList) {
            ListView_SetImageList(data->listView, hImgList, LVSIL_NORMAL);
            ListView_SetIconSpacing(data->listView, 100, 100);
        }
    }
    else if (mode == ID_VIEW_EXTRA_LARGE_ICONS) {
        dwStyle |= LVS_ICON;
        SetWindowLong(data->listView, GWL_STYLE, dwStyle);
        HRESULT hr = SHGetImageList(SHIL_JUMBO, &IID_IImageList, (void**)&hImgList);
        if (FAILED(hr)) hr = SHGetImageList(SHIL_EXTRALARGE, &IID_IImageList, (void**)&hImgList);
        if (SUCCEEDED(hr) && hImgList) {
            ListView_SetImageList(data->listView, hImgList, LVSIL_NORMAL);
            ListView_SetIconSpacing(data->listView, 270, 290);
        }
    }

    if ((dwStyle & LVS_TYPEMASK) != LVS_REPORT) {
        ListView_Arrange(data->listView, LVA_DEFAULT);
    }

    InvalidateRect(data->listView, NULL, FALSE);
}

int SearchView_GetViewMode(HWND hwndView) {
    SearchViewData* data = (SearchViewData*)GetWindowLongPtr(hwndView, GWLP_USERDATA);
    return data ? data->viewMode : ID_VIEW_DETAILS;
}

static void EscapeVal(bool isCsv, const wchar_t* val, wchar_t* outBuf, size_t maxChars) {
    if (!isCsv) {
        wcscpy_s(outBuf, maxChars, val);
        return;
    }
    if (wcschr(val, L',') || wcschr(val, L'"') || wcschr(val, L'\n') || wcschr(val, L'\r')) {
        outBuf[0] = L'"';
        size_t idx = 1;
        for (size_t i = 0; val[i] != L'\0' && idx < maxChars - 3; i++) {
            if (val[i] == L'"') {
                outBuf[idx++] = L'"';
                outBuf[idx++] = L'"';
            } else {
                outBuf[idx++] = val[i];
            }
        }
        outBuf[idx++] = L'"';
        outBuf[idx] = L'\0';
    } else {
        wcscpy_s(outBuf, maxChars, val);
    }
}

bool SearchView_ExportResults(HWND hwndView, const wchar_t* filePath) {
    SearchViewData* data = (SearchViewData*)GetWindowLongPtr(hwndView, GWLP_USERDATA);
    if (!data) return false;

    // If filePath is NULL, fall back to copy selection CF_HDROP clipboard packer!
    if (!filePath) {
        int count = 0;
        int idx = -1;
        while ((idx = ListView_GetNextItem(data->listView, idx, LVNI_SELECTED)) != -1) {
            count++;
        }
        if (count > 0) {
            const wchar_t** files = (const wchar_t**)malloc(count * sizeof(wchar_t*));
            if (files) {
                int sIdx = -1;
                size_t k = 0;
                while ((sIdx = ListView_GetNextItem(data->listView, sIdx, LVNI_SELECTED)) != -1) {
                    wchar_t* path = (wchar_t*)malloc(MAX_PATH * sizeof(wchar_t));
                    if (path) {
                        SearchEngine_GetResultFullPath(data->searchEngine, &data->results.data[sIdx], path, MAX_PATH);
                        files[k++] = path;
                    }
                }
                ShellIntegration_CopyFilesToClipboard(data->hWnd, files, k, false);
                for (size_t i = 0; i < k; i++) free((void*)files[i]);
                free(files);
            }
        }
        return true;
    }

    FILE* file = NULL;
    if (_wfopen_s(&file, filePath, L"w, ccs=UTF-8") != 0 || !file) {
        return false;
    }

    bool isCsv = false;
    size_t fpLen = wcslen(filePath);
    if (fpLen >= 4) {
        const wchar_t* ext = filePath + fpLen - 4;
        if (_wcsicmp(ext, L".csv") == 0) {
            isCsv = true;
        }
    }

    wchar_t sep = isCsv ? L',' : L'\t';

    wchar_t escName[260], escPath[MAX_PATH], escSize[64], escSizeDisk[64], escMod[64];
    EscapeVal(isCsv, L"Name", escName, 260);
    EscapeVal(isCsv, L"Path", escPath, MAX_PATH);
    EscapeVal(isCsv, L"Size", escSize, 64);
    EscapeVal(isCsv, L"Size on Disk", escSizeDisk, 64);
    EscapeVal(isCsv, L"Date Modified", escMod, 64);

    fwprintf(file, L"%s%c%s%c%s%c%s%c%s\n", escName, sep, escPath, sep, escSize, sep, escSizeDisk, sep, escMod);

    for (size_t i = 0; i < data->results.count; i++) {
        const SearchResult* res = &data->results.data[i];

        wchar_t fullPath[MAX_PATH];
        SearchEngine_GetResultFullPath(data->searchEngine, res, fullPath, MAX_PATH);

        wchar_t sizeStr[64] = { 0 };
        wchar_t sizeDiskStr[64] = { 0 };
        wchar_t modStr[64] = { 0 };

        if (!res->IsDirectory) {
            FormatFileSize(res->Size, sizeStr, 64);
            FormatFileSize(res->SizeOnDisk, sizeDiskStr, 64);
        }
        FormatFileTime(res->DateModified, modStr, 64);

        EscapeVal(isCsv, res->Name, escName, 260);
        EscapeVal(isCsv, fullPath, escPath, MAX_PATH);
        EscapeVal(isCsv, sizeStr, escSize, 64);
        EscapeVal(isCsv, sizeDiskStr, escSizeDisk, 64);
        EscapeVal(isCsv, modStr, escMod, 64);

        fwprintf(file, L"%s%c%s%c%s%c%s%c%s\n", escName, sep, escPath, sep, escSize, sep, escSizeDisk, sep, escMod);
    }

    fclose(file);
    return true;
}
