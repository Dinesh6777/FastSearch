#include "main_frame.h"
#include "search_view.h"
#include "settings_dialog.h"
#include "fs_logger.h"
#include "resource.h"
#include <commctrl.h>
#include <commdlg.h>
#include <dbt.h>
#include <shellapi.h>
#include <shlobj.h>
#include <exdisp.h>
#include <process.h>

#define WM_SEARCH_RESULTS_CHANGED (WM_USER + 101)
#define WM_NTFS_INDEX_CHANGED (WM_USER + 102)
#define WM_TRAYICON (WM_USER + 105)
#define WM_SORTING_STATUS (WM_USER + 106)
#define WM_HOTKEY_TRIGGERED (WM_USER + 200)

// DBT Volume interface GUID
static const GUID GUID_DEVINTERFACE_VOLUME_LOCAL = { 0x53f5630d, 0xb6bf, 0x11d0, {0x94, 0xf2, 0x00, 0xa0, 0xc9, 0x1e, 0xfb, 0x8b} };

static HWND g_hMainFrameWnd = NULL;
static HHOOK hKeyboardHook = NULL;

// Declarations of local functions
static void CreateNewTab(MainFrameData* data, const wchar_t* name);
static void CloseTab(MainFrameData* data, int tabIndex);
static void UpdateStatusText(MainFrameData* data);
static void UpdateDriveComboBoxes(MainFrameData* data);
static void AddTrayIcon(MainFrameData* data);
static void RemoveTrayIcon(MainFrameData* data);

// Safe drive letter resolver on hot-plug arrival
static wchar_t ResolveDriveLetter(const wchar_t* dbcc_name) {
    if (!dbcc_name) return L'\0';

    // 1. Direct path lookup
    wchar_t volName[512];
    wcscpy_s(volName, 512, dbcc_name);
    if (volName[wcslen(volName) - 1] != L'\\') {
        wcscat_s(volName, 512, L"\\");
    }
    wchar_t pathNames[MAX_PATH] = { 0 };
    DWORD charsReturned = 0;
    if (GetVolumePathNamesForVolumeNameW(volName, pathNames, MAX_PATH, &charsReturned)) {
        if (pathNames[0] != L'\0' && pathNames[1] == L':') {
            return towupper(pathNames[0]);
        }
    }

    // 2. Query matches
    DWORD driveMask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (driveMask & (1 << i)) {
            wchar_t driveLetter = L'A' + i;
            wchar_t drivePath[] = { driveLetter, L':', L'\\', L'\0' };
            wchar_t guidName[MAX_PATH] = { 0 };
            if (GetVolumeNameForVolumeMountPointW(drivePath, guidName, MAX_PATH)) {
                size_t gLen = wcslen(guidName);
                size_t dLen = wcslen(dbcc_name);
                if (gLen > 0 && guidName[gLen - 1] == L'\\') guidName[gLen - 1] = L'\0';
                
                wchar_t s2[MAX_PATH];
                wcscpy_s(s2, MAX_PATH, dbcc_name);
                if (dLen > 0 && s2[dLen - 1] == L'\\') s2[dLen - 1] = L'\0';

                if (_wcsicmp(guidName, s2) == 0) {
                    return driveLetter;
                }
            }
        }
    }
    return L'\0';
}

// C progress callbacks triggered by background MFT loader threads
static void FrameOnIndexProgress(void* context, wchar_t drive, unsigned int current, unsigned int total) {
    MainFrameData* data = (MainFrameData*)context;
    wchar_t buf[256];
    unsigned int pct = total > 0 ? (current * 100) / total : 0;
    swprintf_s(buf, 256, L"Indexing Drive %c: %d%% (%u of %u records)...", drive, pct, current, total);
    SendMessageW(data->statusBar, SB_SETTEXTW, 0, (LPARAM)buf);
}

static void FrameOnIndexComplete(void* context, wchar_t drive, bool success, unsigned int fileCount, unsigned int folderCount) {
    MainFrameData* data = (MainFrameData*)context;
    (void)drive; (void)fileCount; (void)folderCount;
    if (success) {
        UpdateStatusText(data);
        if (data->activeTabIndex != -1 && data->activeTabIndex < data->tabsCount) {
            SearchView_TriggerSearch(data->tabs[data->activeTabIndex].View);
        }
        UpdateDriveComboBoxes(data);
    } else {
        SendMessageW(data->statusBar, SB_SETTEXTW, 0, (LPARAM)L"Failed to read raw MFT block. Run as Administrator.");
    }
}



static bool IsStartupEnabled(void) {
    HKEY hKey;
    bool enabled = false;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        DWORD dwType = REG_SZ;
        wchar_t buf[512] = { 0 };
        DWORD dwSize = sizeof(buf);
        if (RegQueryValueExW(hKey, L"FastSearch", NULL, &dwType, (LPBYTE)buf, &dwSize) == ERROR_SUCCESS) {
            enabled = (wcslen(buf) > 0);
        }
        RegCloseKey(hKey);
    }
    return enabled;
}

static void SetStartupEnabled(bool enable) {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_WRITE, &hKey) == ERROR_SUCCESS) {
        if (enable) {
            wchar_t exePath[MAX_PATH];
            GetModuleFileNameW(NULL, exePath, MAX_PATH);
            
            // Format exe path with quotes for safety
            wchar_t quotedPath[MAX_PATH + 2];
            swprintf_s(quotedPath, MAX_PATH + 2, L"\"%s\"", exePath);
            
            RegSetValueExW(hKey, L"FastSearch", 0, REG_SZ, (const BYTE*)quotedPath, (DWORD)((wcslen(quotedPath) + 1) * sizeof(wchar_t)));
        } else {
            RegDeleteValueW(hKey, L"FastSearch");
        }
        RegCloseKey(hKey);
    }
}

// No Explorer integration functions needed

static LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        KBDLLHOOKSTRUCT* pKeyInfo = (KBDLLHOOKSTRUCT*)lParam;
        if (pKeyInfo && (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN)) {
            bool winDown = (GetAsyncKeyState(VK_LWIN) & 0x8000) || (GetAsyncKeyState(VK_RWIN) & 0x8000);
            bool ctrlDown = (GetAsyncKeyState(VK_CONTROL) & 0x8000);
            bool shiftDown = (GetAsyncKeyState(VK_SHIFT) & 0x8000);
            bool altDown = (GetAsyncKeyState(VK_MENU) & 0x8000);

            // 1. Intercept Win + F globally to restore and focus (Option 2)
            if (pKeyInfo->vkCode == 'F' && winDown && !ctrlDown && !shiftDown && !altDown) {
                if (g_hMainFrameWnd && IsWindow(g_hMainFrameWnd)) {
                    // Suppress Start Menu on Win key release by sending dummy Ctrl keystrokes
                    keybd_event(VK_LCONTROL, 0, 0, 0);
                    keybd_event(VK_LCONTROL, 0, KEYEVENTF_KEYUP, 0);

                    // Post trigger asynchronously to main window (wParam=2 signifies Win+F)
                    PostMessageW(g_hMainFrameWnd, WM_HOTKEY_TRIGGERED, 2, 0);
                    return 1; // Swallow key stroke so OS Feedback Hub is bypassed!
                }
            }

        }
    }
    return CallNextHookEx(hKeyboardHook, nCode, wParam, lParam);
}

// Main Frame window procedure
static LRESULT CALLBACK MainFrameWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    MainFrameData* data = (MainFrameData*)GetWindowLongPtr(hWnd, GWLP_USERDATA);

    switch (uMsg) {
        case WM_CREATE: {
            data = (MainFrameData*)malloc(sizeof(MainFrameData));
            if (!data) return -1;
            memset(data, 0, sizeof(MainFrameData));
            data->hWnd = hWnd;
            data->activeTabIndex = -1;
            data->columnMask = COL_DEFAULT;

            CREATESTRUCTW* pcs = (CREATESTRUCTW*)lParam;
            MainFrameCreateParams* params = (MainFrameCreateParams*)pcs->lpCreateParams;
            data->searchEngine = params->engine;
            const wchar_t* initialPath = params->initialPath;

            SetWindowLongPtr(hWnd, GWLP_USERDATA, (LONG_PTR)data);

            // Load large and small icons
            HICON hIcon = (HICON)LoadImageW(GetModuleHandle(NULL), MAKEINTRESOURCE(IDI_APP), IMAGE_ICON, 
                                            GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR);
            if (hIcon) SendMessageW(hWnd, WM_SETICON, ICON_BIG, (LPARAM)hIcon);

            HICON hIconSmall = (HICON)LoadImageW(GetModuleHandle(NULL), MAKEINTRESOURCE(IDI_APP), IMAGE_ICON, 
                                                 GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
            if (hIconSmall) SendMessageW(hWnd, WM_SETICON, ICON_SMALL, (LPARAM)hIconSmall);

            // 1. Create status bar
            data->statusBar = CreateWindowExW(
                0, STATUSCLASSNAMEW, NULL,
                WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
                0, 0, 0, 0,
                hWnd, NULL, GetModuleHandle(NULL), NULL
            );
            int statusWidths[] = { -1 };
            SendMessageW(data->statusBar, SB_SETPARTS, 1, (LPARAM)statusWidths);
            SendMessageW(data->statusBar, SB_SETTEXTW, 0, (LPARAM)L"Scanning local drives...");

            // 2. Create tab control
            data->tabCtrl = CreateWindowExW(
                0, WC_TABCONTROLW, NULL,
                WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN | TCS_TABS | TCS_FOCUSNEVER | WS_TABSTOP,
                0, 0, 0, 0,
                hWnd, (HMENU)IDC_TAB_CTRL, GetModuleHandle(NULL), NULL
            );

            HFONT hFont = CreateFontW(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                      OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                                      DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
            SendMessageW(data->tabCtrl, WM_SETFONT, (WPARAM)hFont, TRUE);

            // Register notifying window with SearchEngine
            SearchEngine_RegisterNotifyWindow(data->searchEngine, hWnd);

            // Trigger NTFS drives indexing discovery
            IIndexProgressCallback cb;
            cb.Context = data;
            cb.OnIndexProgress = FrameOnIndexProgress;
            cb.OnIndexComplete = FrameOnIndexComplete;
            SearchEngine_InitializeDrives(data->searchEngine, cb);

            // Create default initial search pane
            CreateNewTab(data, L"Search 1");
            if (initialPath && initialPath[0] != L'\0') {
                SearchView_SetSearchText(data->tabs[0].View, initialPath);
                data->searchEngine->matchPath = true;
                HMENU hMenu = GetMenu(hWnd);
                if (hMenu) {
                    CheckMenuItem(hMenu, ID_OPTIONS_MATCH_PATH, MF_CHECKED);
                }
                SearchView_TriggerSearch(data->tabs[0].View);
            }

            // Append "+" tab at the end of tab control headers
            TCITEMW plusItem = { 0 };
            plusItem.mask = TCIF_TEXT;
            plusItem.pszText = L"+";
            SendMessageW(data->tabCtrl, TCM_INSERTITEMW, SendMessageW(data->tabCtrl, TCM_GETITEMCOUNT, 0, 0), (LPARAM)&plusItem);

            AddTrayIcon(data);

            // Default View menu style radio check and Logging check
            HMENU hMenu = GetMenu(hWnd);
            if (hMenu) {
                CheckMenuRadioItem(hMenu, ID_VIEW_DETAILS, ID_VIEW_EXTRA_LARGE_ICONS, ID_VIEW_DETAILS, MF_BYCOMMAND);
                CheckMenuItem(hMenu, ID_HELP_ENABLE_LOGGING, Logger_IsEnabled() ? MF_CHECKED : MF_UNCHECKED);
                CheckMenuItem(hMenu, ID_OPTIONS_STARTUP, IsStartupEnabled() ? MF_CHECKED : MF_UNCHECKED);
            }

            // Cleanup any leftover legacy explorer integration registry keys
            RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\Classes\\Folder\\shell\\find\\command");
            RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\Classes\\Folder\\shell\\find");
            RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\Classes\\Directory\\shell\\find\\command");
            RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\Classes\\Directory\\shell\\find");
            RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\Classes\\Drive\\shell\\find\\command");
            RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\Classes\\Drive\\shell\\find");
            SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, NULL, NULL);

            // Register PnP external volume device arrivals/removals
            DEV_BROADCAST_DEVICEINTERFACE notificationFilter;
            ZeroMemory(&notificationFilter, sizeof(notificationFilter));
            notificationFilter.dbcc_size = sizeof(notificationFilter);
            notificationFilter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
            notificationFilter.dbcc_classguid = GUID_DEVINTERFACE_VOLUME_LOCAL;

            data->hDevNotify = RegisterDeviceNotificationW(
                hWnd,
                &notificationFilter,
                DEVICE_NOTIFY_WINDOW_HANDLE
            );



            // Set global window pointer and register low-level keyboard hook
            g_hMainFrameWnd = hWnd;
            hKeyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc, GetModuleHandle(NULL), 0);

            // Register global backup hotkey: Ctrl + Shift + F
            RegisterHotKey(hWnd, 2, MOD_CONTROL | MOD_SHIFT, 'F');

            return 0;
        }
        case WM_SIZE: {
            if (!data) return 0;
            int cx = LOWORD(lParam);
            int cy = HIWORD(lParam);
            if (cx == 0 || cy == 0) return 0;

            SendMessageW(data->statusBar, WM_SIZE, 0, 0);
            RECT rcStatus;
            GetWindowRect(data->statusBar, &rcStatus);
            int statusHeight = rcStatus.bottom - rcStatus.top;

            // Re-flow Tab Control
            MoveWindow(data->tabCtrl, 0, 0, cx, cy - statusHeight, TRUE);

            // Adjust visible tab view coordinates
            if (data->activeTabIndex != -1 && data->activeTabIndex < data->tabsCount) {
                RECT rcDisplay;
                GetClientRect(data->tabCtrl, &rcDisplay);
                SendMessageW(data->tabCtrl, TCM_ADJUSTRECT, FALSE, (LPARAM)&rcDisplay);
                MoveWindow(data->tabs[data->activeTabIndex].View, rcDisplay.left, rcDisplay.top, 
                           rcDisplay.right - rcDisplay.left, rcDisplay.bottom - rcDisplay.top, TRUE);
            }
            return 0;
        }
        case WM_SETFOCUS: {
            if (data && data->activeTabIndex != -1 && data->activeTabIndex < data->tabsCount) {
                if (IsWindow(data->tabs[data->activeTabIndex].View)) {
                    SetFocus(data->tabs[data->activeTabIndex].View);
                }
            }
            break;
        }
        case WM_CLOSE: {
            // Minimize to system tray standard visual behavior
            ShowWindow(hWnd, SW_HIDE);
            return 0; // Handled
        }
        case WM_TRAYICON: {
            if (!data) return 0;
            if (lParam == WM_LBUTTONDBLCLK) {
                ShowWindow(hWnd, SW_RESTORE);
                SetForegroundWindow(hWnd);
            }
            else if (lParam == WM_RBUTTONUP) {
                POINT pt;
                GetCursorPos(&pt);
                HMENU hMenu = LoadMenuW(GetModuleHandle(NULL), MAKEINTRESOURCE(IDR_TRAYMENU));
                if (hMenu) {
                    HMENU hSub = GetSubMenu(hMenu, 0);
                    SetForegroundWindow(hWnd);
                    TrackPopupMenu(hSub, TPM_LEFTALIGN | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hWnd, NULL);
                    DestroyMenu(hMenu);
                }
            }
            return 0;
        }
        case WM_DEVICECHANGE: {
            if (!data) return TRUE;
            if (wParam == DBT_DEVICEARRIVAL || wParam == DBT_DEVICEREMOVECOMPLETE) {
                IIndexProgressCallback cb;
                cb.Context = data;
                cb.OnIndexProgress = FrameOnIndexProgress;
                cb.OnIndexComplete = FrameOnIndexComplete;
                
                SearchEngine_InitializeDrives(data->searchEngine, cb);
                UpdateDriveComboBoxes(data);

                if (data->activeTabIndex != -1 && data->activeTabIndex < data->tabsCount) {
                    SearchView_TriggerSearch(data->tabs[data->activeTabIndex].View);
                }
            }
            else if (wParam == DBT_DEVICEQUERYREMOVE) {
                DEV_BROADCAST_HDR* header = (DEV_BROADCAST_HDR*)lParam;
                if (header) {
                    wchar_t driveLetter = L'\0';
                    if (header->dbch_devicetype == DBT_DEVTYP_VOLUME) {
                        DEV_BROADCAST_VOLUME* vol = (DEV_BROADCAST_VOLUME*)header;
                        for (int i = 0; i < 26; ++i) {
                            if (vol->dbcv_unitmask & (1 << i)) {
                                driveLetter = L'A' + i;
                                break;
                            }
                        }
                    }
                    else if (header->dbch_devicetype == DBT_DEVTYP_DEVICEINTERFACE) {
                        DEV_BROADCAST_DEVICEINTERFACE* devInt = (DEV_BROADCAST_DEVICEINTERFACE*)header;
                        driveLetter = ResolveDriveLetter(devInt->dbcc_name);
                    }

                    if (driveLetter != L'\0') {
                        SearchEngine_RemoveDrive(data->searchEngine, driveLetter);
                        UpdateDriveComboBoxes(data);
                        if (data->activeTabIndex != -1 && data->activeTabIndex < data->tabsCount) {
                            SearchView_TriggerSearch(data->tabs[data->activeTabIndex].View);
                        }
                    }
                }
                return TRUE;
            }
            else if (wParam == DBT_DEVICEQUERYREMOVEFAILED) {
                IIndexProgressCallback cb;
                cb.Context = data;
                cb.OnIndexProgress = FrameOnIndexProgress;
                cb.OnIndexComplete = FrameOnIndexComplete;
                SearchEngine_InitializeDrives(data->searchEngine, cb);
                UpdateDriveComboBoxes(data);
                if (data->activeTabIndex != -1 && data->activeTabIndex < data->tabsCount) {
                    SearchView_TriggerSearch(data->tabs[data->activeTabIndex].View);
                }
                return TRUE;
            }
            return TRUE;
        }
        case WM_NOTIFY: {
            if (!data) return 0;
            LPNMHDR pnmh = (LPNMHDR)lParam;

            if (pnmh->idFrom == IDC_TAB_CTRL) {
                if (pnmh->code == TCN_SELCHANGE) {
                    int sel = (int)SendMessageW(data->tabCtrl, TCM_GETCURSEL, 0, 0);
                    if (sel == -1) return 0;

                    wchar_t text[100];
                    TCITEMW item = { 0 };
                    item.mask = TCIF_TEXT;
                    item.pszText = text;
                    item.cchTextMax = 100;
                    SendMessageW(data->tabCtrl, TCM_GETITEMW, sel, (LPARAM)&item);

                    // Clicked "+" tab: spawn tab and auto-select
                    if (wcscmp(text, L"+") == 0) {
                        wchar_t tabName[50];
                        swprintf_s(tabName, 50, L"Search %d", data->tabsCount + 1);
                        CreateNewTab(data, tabName);
                        SendMessageW(data->tabCtrl, TCM_SETCURSEL, data->tabsCount - 1, 0);
                        sel = data->tabsCount - 1;
                    }

                    // Shift focus/visibility
                    if (data->activeTabIndex != -1 && data->activeTabIndex < data->tabsCount && data->activeTabIndex != sel) {
                        ShowWindow(data->tabs[data->activeTabIndex].View, SW_HIDE);
                    }

                    data->activeTabIndex = sel;
                    if (data->activeTabIndex != -1 && data->activeTabIndex < data->tabsCount) {
                        ShowWindow(data->tabs[data->activeTabIndex].View, SW_SHOW);
                        BringWindowToTop(data->tabs[data->activeTabIndex].View);
                        SetFocus(data->tabs[data->activeTabIndex].View);
                        SearchView_TriggerSearch(data->tabs[data->activeTabIndex].View);

                        HMENU hMenu = GetMenu(hWnd);
                        if (hMenu) {
                            int activeMode = SearchView_GetViewMode(data->tabs[data->activeTabIndex].View);
                            CheckMenuRadioItem(hMenu, ID_VIEW_DETAILS, ID_VIEW_EXTRA_LARGE_ICONS, activeMode, MF_BYCOMMAND);
                        }

                        // Resize layouts
                        RECT rc;
                        GetClientRect(hWnd, &rc);
                        SendMessageW(hWnd, WM_SIZE, SIZE_RESTORED, MAKELPARAM(rc.right - rc.left, rc.bottom - rc.top));
                    }
                    return 0;
                }
                else if (pnmh->code == NM_CLICK) {
                    DWORD pos = GetMessagePos();
                    POINT pt;
                    pt.x = GET_X_LPARAM(pos);
                    pt.y = GET_Y_LPARAM(pos);
                    ScreenToClient(data->tabCtrl, &pt);

                    TCHITTESTINFO hti;
                    hti.pt = pt;
                    int tabIdx = (int)SendMessageW(data->tabCtrl, TCM_HITTEST, 0, (LPARAM)&hti);

                    if (tabIdx != -1 && tabIdx < data->tabsCount) {
                        RECT rcItem;
                        SendMessageW(data->tabCtrl, TCM_GETITEMRECT, tabIdx, (LPARAM)&rcItem);
                        
                        // Clicked the tab close button (rightmost 20px of tab header)
                        if (pt.x >= rcItem.right - 20) {
                            CloseTab(data, tabIdx);
                            return 1; // Suppress further default handling
                        }
                    }
                }
            }
            break;
        }
        case WM_COMMAND: {
            if (!data) return 0;
            WORD id = LOWORD(wParam);

            if (id == ID_FILE_NEW_TAB) {
                wchar_t tabName[50];
                swprintf_s(tabName, 50, L"Search %d", data->tabsCount + 1);
                CreateNewTab(data, tabName);
                SendMessageW(data->tabCtrl, TCM_SETCURSEL, data->tabsCount - 1, 0);
                
                // Trigger change
                NMHDR nm;
                nm.hwndFrom = data->tabCtrl;
                nm.idFrom = IDC_TAB_CTRL;
                nm.code = TCN_SELCHANGE;
                SendMessageW(hWnd, WM_NOTIFY, IDC_TAB_CTRL, (LPARAM)&nm);
            }
            else if (id == ID_FILE_CLOSE_TAB) {
                if (data->tabsCount > 1 && data->activeTabIndex != -1) {
                    CloseTab(data, data->activeTabIndex);
                }
            }
            else if (id == ID_FILE_EXPORT) {
                if (data->activeTabIndex != -1 && data->activeTabIndex < data->tabsCount) {
                    OPENFILENAMEW ofn = { 0 };
                    wchar_t szFile[MAX_PATH] = { 0 };
                    ofn.lStructSize = sizeof(ofn);
                    ofn.hwndOwner = hWnd;
                    ofn.lpstrFile = szFile;
                    ofn.nMaxFile = sizeof(szFile) / sizeof(wchar_t);
                    ofn.lpstrFilter = L"Text Tab-Separated (*.txt)\0*.txt\0CSV Comma-Separated (*.csv)\0*.csv\0";
                    ofn.nFilterIndex = 1;
                    ofn.lpstrDefExt = L"txt";
                    ofn.Flags = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT | OFN_HIDEREADONLY;

                    if (GetSaveFileNameW(&ofn)) {
                        SearchView_ExportResults(data->tabs[data->activeTabIndex].View, ofn.lpstrFile);
                    }
                }
            }
            else if (id == ID_FILE_EXIT) {
                RemoveTrayIcon(data);
                DestroyWindow(hWnd);
            }
            else if (id == ID_VIEW_SETTINGS) {
                unsigned int tempMask = data->columnMask;
                if (SettingsDialog_Show(hWnd, &tempMask)) {
                    data->columnMask = tempMask;
                    for (int i = 0; i < data->tabsCount; i++) {
                        SearchView_UpdateColumns(data->tabs[i].View, data->columnMask);
                        SearchView_TriggerSearch(data->tabs[i].View);
                    }
                }
            }
            else if (id == ID_OPTIONS_MATCH_PATH) {
                data->searchEngine->matchPath = !data->searchEngine->matchPath;
                HMENU hMenu = GetMenu(hWnd);
                if (hMenu) {
                    CheckMenuItem(hMenu, ID_OPTIONS_MATCH_PATH, data->searchEngine->matchPath ? MF_CHECKED : MF_UNCHECKED);
                }
                // Refresh active search view immediately
                if (data->activeTabIndex != -1 && data->activeTabIndex < data->tabsCount) {
                    SearchView_TriggerSearch(data->tabs[data->activeTabIndex].View);
                }
            }
            else if (id == ID_OPTIONS_STARTUP) {
                bool enabled = !IsStartupEnabled();
                SetStartupEnabled(enabled);
                HMENU hMenu = GetMenu(hWnd);
                if (hMenu) {
                    CheckMenuItem(hMenu, ID_OPTIONS_STARTUP, enabled ? MF_CHECKED : MF_UNCHECKED);
                }
            }
            else if (id == ID_HELP_ABOUT) {
                MessageBoxW(hWnd, L"FastSearch File Indexer v1.2.0\n\nBuilt in C utilizing raw Win32 APIs.\nNTFS MFT pre-sorted contiguous indexing completes searches in milliseconds.", 
                            L"About FastSearch", MB_OK | MB_ICONINFORMATION);
            }
            else if (id == ID_HELP_ENABLE_LOGGING) {
                bool enabled = !Logger_IsEnabled();
                Logger_SetEnabled(enabled);
                HMENU hMenu = GetMenu(hWnd);
                if (hMenu) {
                    CheckMenuItem(hMenu, ID_HELP_ENABLE_LOGGING, enabled ? MF_CHECKED : MF_UNCHECKED);
                }
            }
            else if (id == ID_TRAY_RESTORE) {
                ShowWindow(hWnd, SW_RESTORE);
                SetForegroundWindow(hWnd);
            }
            else if (id == ID_TRAY_EXIT) {
                RemoveTrayIcon(data);
                DestroyWindow(hWnd);
            }
            else if (id == ID_TRAY_NEW_WINDOW) {
                wchar_t path[MAX_PATH];
                GetModuleFileNameW(NULL, path, MAX_PATH);
                ShellExecuteW(NULL, L"runas", path, NULL, NULL, SW_SHOWNORMAL); // Elevate
            }
            else if (id >= ID_VIEW_DETAILS && id <= ID_VIEW_EXTRA_LARGE_ICONS) {
                if (data->activeTabIndex != -1 && data->activeTabIndex < data->tabsCount) {
                    SearchView_SetViewMode(data->tabs[data->activeTabIndex].View, id);
                    HMENU hMenu = GetMenu(hWnd);
                    if (hMenu) {
                        CheckMenuRadioItem(hMenu, ID_VIEW_DETAILS, ID_VIEW_EXTRA_LARGE_ICONS, id, MF_BYCOMMAND);
                    }
                }
            }
            break;
        }
        case WM_SEARCH_RESULTS_CHANGED: {
            if (data) UpdateStatusText(data);
            break;
        }
        case WM_NTFS_INDEX_CHANGED: {
            if (data && data->activeTabIndex != -1 && data->activeTabIndex < data->tabsCount) {
                if (IsWindow(data->tabs[data->activeTabIndex].View)) {
                    PostMessageW(data->tabs[data->activeTabIndex].View, WM_NTFS_INDEX_CHANGED, 0, 0);
                }
            }
            break;
        }
        case WM_SORTING_STATUS: {
            if (data) {
                SendMessageW(data->statusBar, SB_SETTEXTW, 0, (LPARAM)L"Sorting the results...");
            }
            return 0;
        }
        case WM_HOTKEY: {
            if (wParam == 2 && data) {
                // Restore window if minimized
                if (IsIconic(hWnd)) {
                    ShowWindow(hWnd, SW_RESTORE);
                } else {
                    ShowWindow(hWnd, SW_SHOW);
                }
                SetForegroundWindow(hWnd);
                
                // Focus active search edit box
                if (data->activeTabIndex != -1 && data->activeTabIndex < data->tabsCount) {
                    SetFocus(data->tabs[data->activeTabIndex].View);
                }
            }
            break;
        }
        case WM_HOTKEY_TRIGGERED: {
            if (data) {
                // Restore window if minimized
                if (IsIconic(hWnd)) {
                    ShowWindow(hWnd, SW_RESTORE);
                } else {
                    ShowWindow(hWnd, SW_SHOW);
                }
                SetForegroundWindow(hWnd);

                if (wParam == 2) {
                    // Focus active search edit box
                    if (data->activeTabIndex != -1 && data->activeTabIndex < data->tabsCount) {
                        SetFocus(data->tabs[data->activeTabIndex].View);
                    }
                }
            }
            return 0;
        }
        case WM_DESTROY: {
            if (hKeyboardHook) {
                UnhookWindowsHookEx(hKeyboardHook);
                hKeyboardHook = NULL;
            }
            g_hMainFrameWnd = NULL;
            UnregisterHotKey(hWnd, 2);
            if (data) {
                if (data->hDevNotify) UnregisterDeviceNotification(data->hDevNotify);
                RemoveTrayIcon(data);
                
                // Cleanly destroy child views
                for (int i = 0; i < data->tabsCount; i++) {
                    DestroyWindow(data->tabs[i].View);
                }
                
                free(data);
                SetWindowLongPtr(hWnd, GWLP_USERDATA, 0);
            }
            PostQuitMessage(0);
            break;
        }
    }
    return DefWindowProc(hWnd, uMsg, wParam, lParam);
}

bool MainFrame_RegisterClass(void) {
    WNDCLASSEXW wcex = { 0 };
    wcex.cbSize = sizeof(WNDCLASSEXW);
    wcex.style = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc = MainFrameWndProc;
    wcex.cbClsExtra = 0;
    wcex.cbWndExtra = 0;
    wcex.hInstance = GetModuleHandle(NULL);
    wcex.hIcon = LoadIconW(GetModuleHandle(NULL), MAKEINTRESOURCE(IDI_APP));
    wcex.hIconSm = LoadIconW(GetModuleHandle(NULL), MAKEINTRESOURCE(IDI_APP));
    wcex.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wcex.lpszMenuName = MAKEINTRESOURCEW(IDR_MAINFRAME);
    wcex.lpszClassName = L"CMainFrameClass";

    return RegisterClassExW(&wcex) != 0;
}

HWND MainFrame_Create(SearchEngine* engine, const wchar_t* initialPath) {
    static MainFrameCreateParams params;
    params.engine = engine;
    params.initialPath = initialPath;

    // 780x550 default size
    return CreateWindowExW(
        0, L"CMainFrameClass", L"FastSearch",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, 780, 550,
        NULL, NULL, GetModuleHandle(NULL), &params
    );
}

// Spawns new Tab pane
static void CreateNewTab(MainFrameData* data, const wchar_t* name) {
    if (data->tabsCount >= 32) return;

    HWND view = SearchView_Create(data->hWnd, data->searchEngine);
    SearchView_UpdateColumns(view, data->columnMask);

    int insertIndex = (int)SendMessageW(data->tabCtrl, TCM_GETITEMCOUNT, 0, 0);
    if (insertIndex > 0) {
        wchar_t text[100];
        TCITEMW item;
        item.mask = TCIF_TEXT;
        item.pszText = text;
        item.cchTextMax = 100;
        SendMessageW(data->tabCtrl, TCM_GETITEMW, insertIndex - 1, (LPARAM)&item);
        if (wcscmp(text, L"+") == 0) {
            insertIndex--; // Insert before the "+" tab
        }
    }

    wchar_t displayName[128];
    swprintf_s(displayName, 128, L"%s  x", name);

    TCITEMW tcItem = { 0 };
    tcItem.mask = TCIF_TEXT;
    tcItem.pszText = displayName;
    SendMessageW(data->tabCtrl, TCM_INSERTITEMW, insertIndex, (LPARAM)&tcItem);

    // Shift memory to insert at index
    for (int i = data->tabsCount; i > insertIndex; i--) {
        data->tabs[i] = data->tabs[i - 1];
    }

    wcscpy_s(data->tabs[insertIndex].Name, 64, name);
    data->tabs[insertIndex].View = view;
    data->tabsCount++;

    if (data->activeTabIndex == -1) {
        data->activeTabIndex = insertIndex;
        ShowWindow(view, SW_SHOW);
        BringWindowToTop(view);
    } else if (insertIndex <= data->activeTabIndex) {
        data->activeTabIndex++;
    }

    // Trigger reflow size resize
    RECT rc;
    GetClientRect(data->hWnd, &rc);
    SendMessageW(data->hWnd, WM_SIZE, SIZE_RESTORED, MAKELPARAM(rc.right - rc.left, rc.bottom - rc.top));
}

static void CloseTab(MainFrameData* data, int tabIndex) {
    if (tabIndex < 0 || tabIndex >= data->tabsCount || data->tabsCount <= 1) return;

    DestroyWindow(data->tabs[tabIndex].View);
    SendMessageW(data->tabCtrl, TCM_DELETEITEM, tabIndex, 0);

    // Shift memory left
    for (int i = tabIndex; i < data->tabsCount - 1; i++) {
        data->tabs[i] = data->tabs[i + 1];
    }
    data->tabsCount--;

    if (data->activeTabIndex == tabIndex) {
        data->activeTabIndex = tabIndex < data->tabsCount ? tabIndex : data->tabsCount - 1;
        if (data->activeTabIndex != -1) {
            ShowWindow(data->tabs[data->activeTabIndex].View, SW_SHOW);
            BringWindowToTop(data->tabs[data->activeTabIndex].View);
            SendMessageW(data->tabCtrl, TCM_SETCURSEL, data->activeTabIndex, 0);
        }
    } else if (tabIndex < data->activeTabIndex) {
        data->activeTabIndex--;
    }

    RECT rc;
    GetClientRect(data->hWnd, &rc);
    SendMessageW(data->hWnd, WM_SIZE, SIZE_RESTORED, MAKELPARAM(rc.right - rc.left, rc.bottom - rc.top));
}

// Update StatusBar status descriptive text (formats total count nicely with commas)
static void UpdateStatusText(MainFrameData* data) {
    size_t total = SearchEngine_GetTotalIndexedFiles(data->searchEngine);
    
    // Group number with commas
    NUMBERFMTW fmt;
    fmt.NumDigits = 0;
    fmt.LeadingZero = 0;
    fmt.Grouping = 3;
    fmt.lpDecimalSep = L".";
    fmt.lpThousandSep = L",";
    fmt.NegativeOrder = 1;

    wchar_t rawTotal[64], groupedTotal[64];
    swprintf_s(rawTotal, 64, L"%zu", total);
    if (GetNumberFormatW(LOCALE_USER_DEFAULT, 0, rawTotal, &fmt, groupedTotal, 64) == 0) {
        wcscpy_s(groupedTotal, 64, rawTotal);
    }

    int foundCount = 0;
    double searchTimeMs = 0.0;
    if (data->activeTabIndex != -1 && data->activeTabIndex < data->tabsCount) {
        foundCount = (int)SearchView_GetResultCount(data->tabs[data->activeTabIndex].View);
        searchTimeMs = SearchView_GetLastSearchTimeMs(data->tabs[data->activeTabIndex].View);
    }

    wchar_t rawFound[64], groupedFound[64];
    swprintf_s(rawFound, 64, L"%d", foundCount);
    if (GetNumberFormatW(LOCALE_USER_DEFAULT, 0, rawFound, &fmt, groupedFound, 64) == 0) {
        wcscpy_s(groupedFound, 64, rawFound);
    }

    double searchTimeSecs = searchTimeMs / 1000.0;
    wchar_t timeBuf[32];
    swprintf_s(timeBuf, 32, L"%.3f", searchTimeSecs);

    // Trim trailing zeros from double format
    wchar_t* p = timeBuf + wcslen(timeBuf) - 1;
    while (p > timeBuf && *p == L'0') {
        *p = L'\0';
        p--;
    }
    if (p > timeBuf && *p == L'.') {
        *p = L'\0';
    }

    wchar_t buf[256];
    if (wcscmp(timeBuf, L"1") == 0) {
        swprintf_s(buf, 256, L"%s objects found in 1 sec | Total Indexed: %s files and folders", 
                   groupedFound, groupedTotal);
    } else {
        swprintf_s(buf, 256, L"%s objects found in %s secs | Total Indexed: %s files and folders", 
                   groupedFound, timeBuf, groupedTotal);
    }

    SendMessageW(data->statusBar, SB_SETTEXTW, 0, (LPARAM)buf);
}

// Drive dropdown synchronizer
static void UpdateDriveComboBoxes(MainFrameData* data) {
    wchar_t drives[26];
    int count = SearchEngine_GetIndexedDrives(data->searchEngine, drives, 26);

    for (int i = 0; i < data->tabsCount; i++) {
        HWND hCombo = GetDlgItem(data->tabs[i].View, IDC_VOLUME_COMBO);
        if (hCombo) {
            int curSel = (int)SendMessageW(hCombo, CB_GETCURSEL, 0, 0);
            wchar_t curText[100] = { 0 };
            if (curSel != CB_ERR) {
                SendMessageW(hCombo, CB_GETLBTEXT, curSel, (LPARAM)curText);
            }

            SendMessageW(hCombo, CB_RESETCONTENT, 0, 0);
            SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)L"All Drives");
            
            int newSel = 0;
            for (int k = 0; k < count; k++) {
                wchar_t driveStr[8];
                swprintf_s(driveStr, 8, L"%c:", drives[k]);
                SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)driveStr);
                if (wcscmp(curText, driveStr) == 0) {
                    newSel = k + 1;
                }
            }
            SendMessageW(hCombo, CB_SETCURSEL, newSel, 0);
        }
    }
}

static void AddTrayIcon(MainFrameData* data) {
    NOTIFYICONDATAW nid = { 0 };
    nid.cbSize = sizeof(nid);
    nid.hWnd = data->hWnd;
    nid.uID = IDI_TRAY;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon = LoadIconW(GetModuleHandle(NULL), MAKEINTRESOURCE(IDI_TRAY));
    wcscpy_s(nid.szTip, 128, L"FastSearch File Indexer");

    Shell_NotifyIconW(NIM_ADD, &nid);
}

static void RemoveTrayIcon(MainFrameData* data) {
    NOTIFYICONDATAW nid = { 0 };
    nid.cbSize = sizeof(nid);
    nid.hWnd = data->hWnd;
    nid.uID = IDI_TRAY;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}
