#include "stdafx.h"
#include "MainFrame.h"
#include "SettingsDialog.h"

CMainFrame::CMainFrame()
    : m_activeTabIndex(-1), m_columnMask(COL_DEFAULT) {
}

CMainFrame::~CMainFrame() {
}

LRESULT CMainFrame::OnCreate(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled) {
    // Explicitly load and set large and small window icons to ensure they render in title bar and taskbar
    HICON hIcon = (HICON)::LoadImageW(_Module.GetResourceInstance(), MAKEINTRESOURCE(IDR_MAINFRAME), IMAGE_ICON, 
                                      ::GetSystemMetrics(SM_CXICON), ::GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR);
    if (hIcon) SetIcon(hIcon, TRUE);
    
    HICON hIconSmall = (HICON)::LoadImageW(_Module.GetResourceInstance(), MAKEINTRESOURCE(IDR_MAINFRAME), IMAGE_ICON, 
                                           ::GetSystemMetrics(SM_CXSMICON), ::GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
    if (hIconSmall) SetIcon(hIconSmall, FALSE);

    // 1. Create status bar
    m_statusBar.Create(m_hWnd);
    int statusWidths[] = { -1 }; // Single pane spanning the entire width to prevent cut off
    m_statusBar.SetParts(1, statusWidths);
    m_statusBar.SetText(0, L"Scanning local drives...");

    // 2. Create tab control
    m_tabCtrl.Create(m_hWnd, rcDefault, NULL, WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN | TCS_TABS | TCS_FOCUSNEVER | WS_TABSTOP, 0, IDC_TAB_CTRL);
    
    // Set a modern UI font (Segoe UI)
    HFONT hFont = CreateFontW(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                              DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    m_tabCtrl.SetFont(hFont);

    // 3. Initialize search engine (loads fixed NTFS drives and starts raw indexing threads)
    m_searchEngine.RegisterNotifyWindow(m_hWnd);
    m_searchEngine.InitializeDrives(this);

    // 4. Create default 2 tabs
    CreateNewTab(L"Search 1");
    CreateNewTab(L"Search 2");

    // 5. Append "+" tab at the end of tab control to act as the add button
    TCITEMW plusItem = { 0 };
    plusItem.mask = TCIF_TEXT;
    plusItem.pszText = const_cast<wchar_t*>(L"+");
    m_tabCtrl.InsertItem(m_tabCtrl.GetItemCount(), &plusItem);

    // 6. Setup system tray icon integration
    AddTrayIcon();

    // Set default checked menu item for View mode (Details)
    HMENU hMenu = GetMenu();
    if (hMenu) {
        ::CheckMenuRadioItem(hMenu, ID_VIEW_DETAILS, ID_VIEW_EXTRA_LARGE_ICONS, ID_VIEW_DETAILS, MF_BYCOMMAND);
    }

    // Re-flow frame layouts
    RECT rc;
    GetClientRect(&rc);
    BOOL dummy;
    OnSize(WM_SIZE, SIZE_RESTORED, MAKELPARAM(rc.right - rc.left, rc.bottom - rc.top), dummy);

    return 0;
}

LRESULT CMainFrame::OnClose(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled) {
    // Minimize to tray instead of quitting (Everything-like behavior)
    ShowWindow(SW_HIDE);
    return 0; // Prevent window close / quit
}

LRESULT CMainFrame::OnDestroy(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled) {
    RemoveTrayIcon();
    PostQuitMessage(0);
    return 0;
}

LRESULT CMainFrame::OnSize(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled) {
    int cx = LOWORD(lParam);
    int cy = HIWORD(lParam);

    if (cx == 0 || cy == 0) return 0;

    // Status bar coordinates
    RECT rcStatus;
    m_statusBar.GetWindowRect(&rcStatus);
    int statusHeight = rcStatus.bottom - rcStatus.top;
    m_statusBar.MoveWindow(0, cy - statusHeight, cx, statusHeight);

    // Tab control coordinates
    int tabY = 0;
    int tabHeight = cy - statusHeight;
    m_tabCtrl.MoveWindow(0, tabY, cx, tabHeight);

    // Active Tab view takes the inner display space of the tab control
    if (m_activeTabIndex != -1 && m_activeTabIndex < static_cast<int>(m_tabs.size())) {
        RECT rcDisplay;
        m_tabCtrl.GetClientRect(&rcDisplay);
        m_tabCtrl.AdjustRect(FALSE, &rcDisplay); // Adjusts display rect to discard headers

        // Relocate child search view window
        m_tabs[m_activeTabIndex].View->MoveWindow(&rcDisplay);
    }

    return 0;
}

LRESULT CMainFrame::OnTrayIcon(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled) {
    if (lParam == WM_LBUTTONDBLCLK) {
        // Restore window on double click
        OnTrayRestoreCmd(0, ID_TRAY_RESTORE, NULL, bHandled);
    } 
    else if (lParam == WM_RBUTTONUP) {
        // Pop up the system tray context menu
        POINT pt;
        GetCursorPos(&pt);

        HMENU hMenu = LoadMenuW(_Module.GetResourceInstance(), MAKEINTRESOURCE(IDR_TRAYMENU));
        if (hMenu) {
            HMENU hSubMenu = GetSubMenu(hMenu, 0);
            
            // SetForegroundWindow is mandatory to prevent menu locks
            SetForegroundWindow(m_hWnd);
            TrackPopupMenu(hSubMenu, TPM_LEFTALIGN | TPM_RIGHTBUTTON, pt.x, pt.y, 0, m_hWnd, NULL);
            DestroyMenu(hMenu);
        }
    }
    return 0;
}

// Responds to Windows plug-and-play notifications (USB NTFS external drives)
LRESULT CMainFrame::OnDeviceChange(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled) {
    if (wParam == DBT_DEVICEARRIVAL || wParam == DBT_DEVICEREMOVECOMPLETE) {
        // Redetect drives on hot-plug events
        m_searchEngine.InitializeDrives(this);
    }
    return 0;
}

// Handles switching tabs
LRESULT CMainFrame::OnTabSelChange(int idCtrl, LPNMHDR pnmh, BOOL& bHandled) {
    int sel = m_tabCtrl.GetCurSel();
    if (sel == -1) return 0;

    wchar_t text[100];
    TCITEMW item = { 0 };
    item.mask = TCIF_TEXT;
    item.pszText = text;
    item.cchTextMax = 100;
    m_tabCtrl.GetItem(sel, &item);

    // If the '+' tab was clicked, create a new search tab dynamically before it
    if (wcscmp(text, L"+") == 0) {
        wchar_t tabName[50];
        swprintf_s(tabName, L"Search %d", static_cast<int>(m_tabs.size() + 1));
        CreateNewTab(tabName);
        m_tabCtrl.SetCurSel(sel); // switch focus to the new tab
        sel = sel;
    }

    // Toggle visibilities
    if (m_activeTabIndex != -1 && m_activeTabIndex < static_cast<int>(m_tabs.size()) && m_activeTabIndex != sel) {
        m_tabs[m_activeTabIndex].View->ShowWindow(SW_HIDE);
    }

    m_activeTabIndex = sel;
    if (m_activeTabIndex != -1 && m_activeTabIndex < static_cast<int>(m_tabs.size())) {
        m_tabs[m_activeTabIndex].View->ShowWindow(SW_SHOW);
        m_tabs[m_activeTabIndex].View->BringWindowToTop(); // Force draw on top of Tab control Z-order
        m_tabs[m_activeTabIndex].View->SetFocus();
        m_tabs[m_activeTabIndex].View->TriggerSearch(); // Refresh search immediately to list all items

        // Synchronize the View menu radio checkmark with this tab's view mode
        HMENU hMenu = GetMenu();
        if (hMenu) {
            int activeViewMode = m_tabs[m_activeTabIndex].View->GetViewMode();
            ::CheckMenuRadioItem(hMenu, ID_VIEW_DETAILS, ID_VIEW_EXTRA_LARGE_ICONS, activeViewMode, MF_BYCOMMAND);
        }

        // Trigger reflow resize
        RECT rc;
        GetClientRect(&rc);
        BOOL dummy;
        OnSize(WM_SIZE, SIZE_RESTORED, MAKELPARAM(rc.right - rc.left, rc.bottom - rc.top), dummy);
    }

    return 0;
}

LRESULT CMainFrame::OnTabClick(int idCtrl, LPNMHDR pnmh, BOOL& bHandled) {
    DWORD pos = GetMessagePos();
    POINT pt;
    pt.x = GET_X_LPARAM(pos);
    pt.y = GET_Y_LPARAM(pos);
    m_tabCtrl.ScreenToClient(&pt);

    TCHITTESTINFO hti = { 0 };
    hti.pt = pt;
    int tabIndex = m_tabCtrl.HitTest(&hti);

    if (tabIndex != -1 && tabIndex < static_cast<int>(m_tabs.size())) {
        RECT rcItem;
        m_tabCtrl.GetItemRect(tabIndex, &rcItem);
        
        // If click is within the rightmost 20 pixels of the tab rect (where the "x" sits)
        if (pt.x >= rcItem.right - 20) {
            CloseTab(tabIndex);
            bHandled = TRUE; // Bypass switching focus to the closed tab
            return 1;
        }
    }
    
    bHandled = FALSE; // Allow standard tab selection switching
    return 0;
}

// Command Actions
LRESULT CMainFrame::OnNewTabCmd(WORD wNotifyCode, WORD wID, HWND hWndCtl, BOOL& bHandled) {
    wchar_t tabName[50];
    swprintf_s(tabName, L"Search %d", static_cast<int>(m_tabs.size() + 1));
    CreateNewTab(tabName);
    
    // Select the new tab
    int newIdx = static_cast<int>(m_tabs.size() - 1);
    m_tabCtrl.SetCurSel(newIdx);
    
    BOOL dummy;
    OnTabSelChange(IDC_TAB_CTRL, nullptr, dummy);
    return 0;
}

LRESULT CMainFrame::OnCloseTabCmd(WORD wNotifyCode, WORD wID, HWND hWndCtl, BOOL& bHandled) {
    if (m_tabs.size() > 1 && m_activeTabIndex != -1) {
        CloseTab(m_activeTabIndex);
    }
    return 0;
}

LRESULT CMainFrame::OnExportCmd(WORD wNotifyCode, WORD wID, HWND hWndCtl, BOOL& bHandled) {
    if (m_activeTabIndex == -1 || m_activeTabIndex >= static_cast<int>(m_tabs.size())) {
        return 0;
    }

    // Open standard Windows save file dialog using standard WTL CFileDialog
    CFileDialog dlg(FALSE, L"txt", NULL, OFN_HIDEREADONLY | OFN_OVERWRITEPROMPT, 
                     L"Text Tab-Separated (*.txt)\0*.txt\0CSV Comma-Separated (*.csv)\0*.csv\0", m_hWnd);
    if (dlg.DoModal(m_hWnd) == IDOK) {
        m_tabs[m_activeTabIndex].View->ExportResults(dlg.m_szFileName);
    }
    return 0;
}

LRESULT CMainFrame::OnExitCmd(WORD wNotifyCode, WORD wID, HWND hWndCtl, BOOL& bHandled) {
    RemoveTrayIcon();
    DestroyWindow();
    return 0;
}

LRESULT CMainFrame::OnSettingsCmd(WORD wNotifyCode, WORD wID, HWND hWndCtl, BOOL& bHandled) {
    CSettingsDialog dlg(m_columnMask);
    if (dlg.DoModal(m_hWnd) == IDOK) {
        m_columnMask = dlg.GetColumnMask();
        // Update column sets on all active search views
        for (auto& tab : m_tabs) {
            tab.View->UpdateColumns(m_columnMask);
            tab.View->TriggerSearch();
        }
    }
    return 0;
}

LRESULT CMainFrame::OnAboutCmd(WORD wNotifyCode, WORD wID, HWND hWndCtl, BOOL& bHandled) {
    MessageBoxW(L"FastSearch File Indexer v1.0\n\nBuilt with C++ using WTL/ATL.\nNTFS MFT parsing completes indexing in seconds.", 
                L"About FastSearch", MB_OK | MB_ICONINFORMATION);
    return 0;
}

// Tray Context Menu Commands
LRESULT CMainFrame::OnTrayRestoreCmd(WORD wNotifyCode, WORD wID, HWND hWndCtl, BOOL& bHandled) {
    ShowWindow(SW_RESTORE);
    SetForegroundWindow(m_hWnd);
    return 0;
}

LRESULT CMainFrame::OnTrayExitCmd(WORD wNotifyCode, WORD wID, HWND hWndCtl, BOOL& bHandled) {
    return OnExitCmd(wNotifyCode, wID, hWndCtl, bHandled);
}

LRESULT CMainFrame::OnTrayNewWindowCmd(WORD wNotifyCode, WORD wID, HWND hWndCtl, BOOL& bHandled) {
    // Launch a new elevated instance of this executable
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(NULL, path, MAX_PATH);
    ShellExecuteW(NULL, L"runas", path, NULL, NULL, SW_SHOWNORMAL); // elevate as Admin
    return 0;
}

// Thread-safe callbacks from Background Indexer threads
void CMainFrame::OnIndexProgress(wchar_t drive, unsigned int current, unsigned int total) {
    wchar_t buf[256];
    unsigned int pct = total > 0 ? (current * 100) / total : 0;
    swprintf_s(buf, 256, L"Indexing Drive %c: %d%% (%u of %u records)...", drive, pct, current, total);
    m_statusBar.SetText(0, buf);
}

void CMainFrame::OnIndexComplete(wchar_t drive, bool success, unsigned int fileCount, unsigned int folderCount) {
    if (success) {
        UpdateStatusText();
        
        // Refresh active views to show newly indexed files!
        if (m_activeTabIndex != -1 && m_activeTabIndex < static_cast<int>(m_tabs.size())) {
            m_tabs[m_activeTabIndex].View->TriggerSearch();
        }
        
        // Populate drive selector combo boxes on all tab views
        std::vector<wchar_t> drives = m_searchEngine.GetIndexedDrives();
        for (auto& tab : m_tabs) {
            HWND hCombo = tab.View->GetDlgItem(IDC_VOLUME_COMBO);
            if (hCombo) {
                // Clear combo
                SendMessage(hCombo, CB_RESETCONTENT, 0, 0);
                SendMessage(hCombo, CB_ADDSTRING, 0, (LPARAM)L"All Drives");
                for (wchar_t d : drives) {
                    std::wstring driveStr = L"";
                    driveStr += d;
                    driveStr += L":";
                    SendMessage(hCombo, CB_ADDSTRING, 0, (LPARAM)driveStr.c_str());
                }
                SendMessage(hCombo, CB_SETCURSEL, 0, 0);
            }
        }
    } else {
        m_statusBar.SetText(0, L"Failed to read raw MFT block. Ensure running as Administrator.");
    }
}

void CMainFrame::CreateNewTab(const std::wstring& name) {
    auto view = std::make_unique<CSearchView>(&m_searchEngine);
    view->Create(m_hWnd, rcDefault, NULL, WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN);
    view->UpdateColumns(m_columnMask);

    TabContext tab;
    tab.Name = name;
    tab.View = std::move(view);

    int insertIndex = m_tabCtrl.GetItemCount();
    if (insertIndex > 0) {
        wchar_t text[100];
        TCITEMW item;
        item.mask = TCIF_TEXT;
        item.pszText = text;
        item.cchTextMax = 100;
        m_tabCtrl.GetItem(insertIndex - 1, &item);
        if (wcscmp(text, L"+") == 0) {
            insertIndex--; // Insert before the "+" tab
        }
    }

    std::wstring displayName = tab.Name + L"  x";
    TCITEMW tcItem = { 0 };
    tcItem.mask = TCIF_TEXT;
    tcItem.pszText = const_cast<wchar_t*>(displayName.c_str());
    m_tabCtrl.InsertItem(insertIndex, &tcItem);

    m_tabs.insert(m_tabs.begin() + insertIndex, std::move(tab));

    if (m_activeTabIndex == -1) {
        m_activeTabIndex = insertIndex;
        m_tabs[m_activeTabIndex].View->ShowWindow(SW_SHOW);
        m_tabs[m_activeTabIndex].View->BringWindowToTop(); // Force active draw on top
    } else if (insertIndex <= m_activeTabIndex) {
        m_activeTabIndex++;
    }

    // Reflow Layouts
    RECT rc;
    GetClientRect(&rc);
    BOOL dummy;
    OnSize(WM_SIZE, SIZE_RESTORED, MAKELPARAM(rc.right - rc.left, rc.bottom - rc.top), dummy);
}

void CMainFrame::CloseTab(int tabIndex) {
    if (tabIndex < 0 || tabIndex >= static_cast<int>(m_tabs.size())) return;

    m_tabs[tabIndex].View->DestroyWindow();
    m_tabs.erase(m_tabs.begin() + tabIndex);
    m_tabCtrl.DeleteItem(tabIndex);

    if (m_activeTabIndex == tabIndex) {
        m_activeTabIndex = std::min(tabIndex, static_cast<int>(m_tabs.size() - 1));
        if (m_activeTabIndex != -1) {
            m_tabs[m_activeTabIndex].View->ShowWindow(SW_SHOW);
            m_tabs[m_activeTabIndex].View->BringWindowToTop();
            m_tabCtrl.SetCurSel(m_activeTabIndex);
        }
    } else if (tabIndex < m_activeTabIndex) {
        m_activeTabIndex--;
    }

    RECT rc;
    GetClientRect(&rc);
    BOOL dummy;
    OnSize(WM_SIZE, SIZE_RESTORED, MAKELPARAM(rc.right - rc.left, rc.bottom - rc.top), dummy);
}

void CMainFrame::UpdateStatusText() {
    wchar_t buf[256];
    size_t total = m_searchEngine.GetTotalIndexedFiles();
    
    // Format total indexed count with comma separator
    std::wstringstream wssTotal;
    wssTotal.imbue(std::locale(""));
    wssTotal << total;

    int foundCount = 0;
    double searchTimeMs = 0.0;
    if (m_activeTabIndex != -1 && m_activeTabIndex < static_cast<int>(m_tabs.size())) {
        foundCount = m_tabs[m_activeTabIndex].View->GetResultCount();
        searchTimeMs = m_tabs[m_activeTabIndex].View->GetLastSearchTimeMs();
    }

    std::wstringstream wssFound;
    wssFound.imbue(std::locale(""));
    wssFound << foundCount;

    double searchTimeSecs = searchTimeMs / 1000.0;

    wchar_t timeBuf[32];
    swprintf_s(timeBuf, 32, L"%.3f", searchTimeSecs);
    std::wstring timeStr(timeBuf);
    while (timeStr.size() > 1 && timeStr.back() == L'0') {
        timeStr.pop_back();
    }
    if (timeStr.back() == L'.') {
        timeStr.pop_back();
    }

    // Use nice formatting for singular/plural "sec/secs"
    if (timeStr == L"1") {
        swprintf_s(buf, 256, L"%s objects found in 1 sec | Total Indexed: %s files and folders", 
                   wssFound.str().c_str(), wssTotal.str().c_str());
    } else {
        swprintf_s(buf, 256, L"%s objects found in %s secs | Total Indexed: %s files and folders", 
                   wssFound.str().c_str(), timeStr.c_str(), wssTotal.str().c_str());
    }
    m_statusBar.SetText(0, buf);
}

LRESULT CMainFrame::OnSearchResultsChanged(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled) {
    UpdateStatusText();
    return 0;
}

// Registers System Tray Icons
void CMainFrame::AddTrayIcon() {
    NOTIFYICONDATAW nid = { 0 };
    nid.cbSize = sizeof(nid);
    nid.hWnd = m_hWnd;
    nid.uID = IDI_TRAY;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon = LoadIconW(_Module.GetResourceInstance(), MAKEINTRESOURCE(IDI_TRAY));
    wcscpy_s(nid.szTip, 128, L"FastSearch File Indexer");

    Shell_NotifyIconW(NIM_ADD, &nid);
}

void CMainFrame::RemoveTrayIcon() {
    NOTIFYICONDATAW nid = { 0 };
    nid.cbSize = sizeof(nid);
    nid.hWnd = m_hWnd;
    nid.uID = IDI_TRAY;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

LRESULT CMainFrame::OnNtfsIndexChanged(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled) {
    if (m_activeTabIndex != -1 && m_activeTabIndex < static_cast<int>(m_tabs.size())) {
        if (m_tabs[m_activeTabIndex].View && m_tabs[m_activeTabIndex].View->IsWindow()) {
            m_tabs[m_activeTabIndex].View->PostMessageW(WM_NTFS_INDEX_CHANGED, 0, 0);
        }
    }
    return 0;
}

LRESULT CMainFrame::OnViewModeCmd(WORD wNotifyCode, WORD wID, HWND hWndCtl, BOOL& bHandled) {
    if (m_activeTabIndex != -1 && m_activeTabIndex < static_cast<int>(m_tabs.size())) {
        if (m_tabs[m_activeTabIndex].View && m_tabs[m_activeTabIndex].View->IsWindow()) {
            m_tabs[m_activeTabIndex].View->SetViewMode(wID);

            // Update radio checkmark in the View menu
            HMENU hMenu = GetMenu();
            if (hMenu) {
                ::CheckMenuRadioItem(hMenu, ID_VIEW_DETAILS, ID_VIEW_EXTRA_LARGE_ICONS, wID, MF_BYCOMMAND);
            }
        }
    }
    return 0;
}
