#include "stdafx.h"
#include <commoncontrols.h>
#include "SearchView.h"
#include "SettingsDialog.h"
#include "ShellIntegration.h"
#include "DragDrop.h"

CSearchView::CSearchView(Search::SearchEngine* searchEngine)
    : m_searchEngine(searchEngine), m_columnMask(COL_DEFAULT), m_sortColumn(-1), 
      m_sortAscending(true), m_searchEdit(this, 1), m_listView(this, 2) {
}

CSearchView::~CSearchView() {
}

LRESULT CSearchView::OnCreate(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled) {
    // 1. Create top input control bar
    // Dropdowns and Edit box with proper Win32 Control IDs
    m_volumeCombo.Create(m_hWnd, rcDefault, NULL, WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_TABSTOP, 0, IDC_VOLUME_COMBO);
    m_volumeCombo.AddString(L"All Drives");
    m_volumeCombo.SetCurSel(0);

    m_searchEdit.Create(m_hWnd, rcDefault, NULL, WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, 0, IDC_SEARCH_EDIT);
    // Setting modern cue banner text ("Type to search...")
    m_searchEdit.SetCueBannerText(L"Type as you search...");

    m_filterCombo.Create(m_hWnd, rcDefault, NULL, WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_TABSTOP, 0, IDC_FILTER_COMBO);
    m_filterCombo.AddString(L"All Files and Folders");
    m_filterCombo.AddString(L"Folders Only");
    m_filterCombo.AddString(L"Files Only");
    m_filterCombo.AddString(L"Documents");
    m_filterCombo.AddString(L"Executables");
    m_filterCombo.AddString(L"Pictures");
    m_filterCombo.AddString(L"Audio");
    m_filterCombo.AddString(L"Video");
    m_filterCombo.SetCurSel(0);

    m_caseCheck.Create(m_hWnd, rcDefault, L"Match Case", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP, 0, IDC_CASE_CHECK);
    m_regexCheck.Create(m_hWnd, rcDefault, L"RegEx", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP, 0, IDC_REGEX_CHECK);

    // Apply standard modern GUI font to all search input controls
    HFONT hFont = (HFONT)::GetStockObject(DEFAULT_GUI_FONT);
    m_volumeCombo.SetFont(hFont);
    m_searchEdit.SetFont(hFont);
    m_filterCombo.SetFont(hFont);
    m_caseCheck.SetFont(hFont);
    m_regexCheck.SetFont(hFont);

    // 2. Create high-performance Virtual ListView (LVS_OWNERDATA)
    m_listView.Create(m_hWnd, rcDefault, NULL, 
                      WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_OWNERDATA | LVS_SHOWSELALWAYS | LVS_SHAREIMAGELISTS | WS_BORDER | WS_TABSTOP,
                      0, IDC_RESULTS_LIST);

    // Attach native explorer gridlines and double buffering to prevent flickering
    m_listView.SetExtendedListViewStyle(LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);

    // 3. Attach standard Windows Shell small and large image lists for native system icons
    SHFILEINFOW sfi;
    HIMAGELIST hSysImageList = (HIMAGELIST)SHGetFileInfoW(L"C:\\", 0, &sfi, sizeof(sfi), SHGFI_SYSICONINDEX | SHGFI_SMALLICON);
    if (hSysImageList) {
        m_listView.SetImageList(hSysImageList, LVSIL_SMALL);
    }

    SHFILEINFOW sfiLarge;
    HIMAGELIST hSysImageListLarge = (HIMAGELIST)SHGetFileInfoW(L"C:\\", 0, &sfiLarge, sizeof(sfiLarge), SHGFI_SYSICONINDEX | SHGFI_LARGEICON);
    if (hSysImageListLarge) {
        m_listView.SetImageList(hSysImageListLarge, LVSIL_NORMAL);
    }

    // Initialize columns
    UpdateColumns(m_columnMask);

    return 0;
}

LRESULT CSearchView::OnSize(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled) {
    int cx = LOWORD(lParam);
    int cy = HIWORD(lParam);

    if (cx == 0 || cy == 0) return 0;

    // Layout math: Control bar height is 30 pixels, padding is 6 pixels
    int padding = 6;
    int topBarHeight = 30;
    
    // Widths
    int volWidth = 90;
    int filterWidth = 145;
    int checkWidth = 85;
    
    int editWidth = cx - (volWidth + filterWidth + (checkWidth * 2) + (padding * 5));
    if (editWidth < 50) editWidth = 50;

    int currentX = padding;
    
    // Position controls nicely. ComboBoxes are given 150px height to accommodate their drop-down list boxes.
    m_volumeCombo.MoveWindow(currentX, padding, volWidth, 150);
    currentX += volWidth + padding;

    m_searchEdit.MoveWindow(currentX, padding, editWidth, 22);
    currentX += editWidth + padding;

    m_filterCombo.MoveWindow(currentX, padding, filterWidth, 150);
    currentX += filterWidth + padding;

    m_caseCheck.MoveWindow(currentX, padding, checkWidth, 22);
    currentX += checkWidth + padding;

    m_regexCheck.MoveWindow(currentX, padding, checkWidth, 22);

    // ListView takes the remaining space below the control bar
    int listY = topBarHeight + (padding * 2);
    m_listView.MoveWindow(0, listY, cx, cy - listY);

    return 0;
}

LRESULT CSearchView::OnSearchChanged(WORD wNotifyCode, WORD wID, HWND hWndCtl, BOOL& bHandled) {
    // Kill existing debounce timer
    KillTimer(1);
    // Set 100ms debounce timer to prevent thrashing during fast keyboard typing
    SetTimer(1, 100);
    return 0;
}

LRESULT CSearchView::OnTimer(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled) {
    if (wParam == 1) {
        KillTimer(1);
        RunSearchInternal();
    }
    else if (wParam == 2) {
        KillTimer(2);
        RunSearchInternal();
    }
    return 0;
}

LRESULT CSearchView::OnNtfsIndexChanged(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled) {
    // 500ms debounce/throttle timer for live search updates
    KillTimer(2);
    SetTimer(2, 500);
    return 0;
}

LRESULT CSearchView::OnFilterChanged(WORD wNotifyCode, WORD wID, HWND hWndCtl, BOOL& bHandled) {
    RunSearchInternal();
    return 0;
}

LRESULT CSearchView::OnVolumeChanged(WORD wNotifyCode, WORD wID, HWND hWndCtl, BOOL& bHandled) {
    RunSearchInternal();
    return 0;
}

LRESULT CSearchView::OnSearchOptionChanged(WORD wNotifyCode, WORD wID, HWND hWndCtl, BOOL& bHandled) {
    RunSearchInternal();
    return 0;
}

void CSearchView::TriggerSearch() {
    RunSearchInternal();
}

// Rebuilds list columns dynamically based on visibility settings
void CSearchView::UpdateColumns(unsigned int columnMask) {
    m_columnMask = columnMask;

    // Clear existing columns
    while (m_listView.GetHeader().GetItemCount() > 0) {
        m_listView.DeleteColumn(0);
    }

    m_visibleColumns.clear();

    // Rebuild logical structure
    m_visibleColumns.push_back({ L"Name", 220, COL_NAME });
    m_visibleColumns.push_back({ L"Path", 320, COL_PATH });

    if (m_columnMask & COL_SIZE)       m_visibleColumns.push_back({ L"Size", 90, COL_SIZE });
    if (m_columnMask & COL_SIZE_DISK)  m_visibleColumns.push_back({ L"Size on Disk", 100, COL_SIZE_DISK });
    if (m_columnMask & COL_MODIFIED)   m_visibleColumns.push_back({ L"Date Modified", 130, COL_MODIFIED });
    if (m_columnMask & COL_CREATED)    m_visibleColumns.push_back({ L"Date Created", 130, COL_CREATED });
    if (m_columnMask & COL_ACCESSED)   m_visibleColumns.push_back({ L"Date Accessed", 130, COL_ACCESSED });
    if (m_columnMask & COL_ATTRIBUTES) m_visibleColumns.push_back({ L"Attributes", 70, COL_ATTRIBUTES });

    // Insert columns in UI
    for (size_t i = 0; i < m_visibleColumns.size(); ++i) {
        int fmt = (m_visibleColumns[i].subitemIndex == COL_SIZE || m_visibleColumns[i].subitemIndex == COL_SIZE_DISK) ? 
                  LVCFMT_RIGHT : LVCFMT_LEFT;
        m_listView.InsertColumn(static_cast<int>(i), m_visibleColumns[i].text.c_str(), fmt, m_visibleColumns[i].width);
    }
}

// Triggers search query on SearchEngine and displays results in virtual ListView
void CSearchView::RunSearchInternal() {
    int len = m_searchEdit.GetWindowTextLength();
    std::wstring query;
    query.resize(len);
    m_searchEdit.GetWindowText(&query[0], len + 1);
    
    // Trim extra null characters
    query.resize(len);

    Search::FilterType filter = static_cast<Search::FilterType>(m_filterCombo.GetCurSel());

    // Resolve drive letter selection
    wchar_t driveLetter = L'\0';
    int volSel = m_volumeCombo.GetCurSel();
    if (volSel > 0) {
        std::vector<wchar_t> drives = m_searchEngine->GetIndexedDrives();
        if (volSel - 1 < static_cast<int>(drives.size())) {
            driveLetter = drives[volSel - 1];
        }
    }

    Search::MatchMode mode = Search::MatchMode::PlainText;
    if (m_regexCheck.GetCheck() == BST_CHECKED) {
        mode = Search::MatchMode::Regex;
    } else if (query.find_first_of(L"*?") != std::wstring::npos) {
        mode = Search::MatchMode::Wildcard;
    }

    m_results.clear();
    m_searchEngine->ExecuteSearch(query, mode, driveLetter, filter, m_results);

    // Reapply sorting if enabled
    if (m_sortColumn != -1) {
        // Find logical flags corresponding to UI sort column
        int flag = m_visibleColumns[m_sortColumn].subitemIndex;
        
        std::sort(m_results.begin(), m_results.end(), [this, flag](const Search::SearchResult& a, const Search::SearchResult& b) {
            bool res = false;
            switch (flag) {
                case COL_NAME:
                    res = _wcsicmp(a.Name.c_str(), b.Name.c_str()) < 0;
                    break;
                case COL_PATH:
                    if (a.Drive != b.Drive) res = a.Drive < b.Drive;
                    else res = a.RecordIndex < b.RecordIndex;
                    break;
                case COL_SIZE:
                    res = a.Size < b.Size;
                    break;
                case COL_SIZE_DISK:
                    res = a.SizeOnDisk < b.SizeOnDisk;
                    break;
                case COL_MODIFIED:
                    res = a.DateModified < b.DateModified;
                    break;
                case COL_CREATED:
                    res = a.DateCreated < b.DateCreated;
                    break;
                case COL_ACCESSED:
                    res = a.DateAccessed < b.DateAccessed;
                    break;
                case COL_ATTRIBUTES:
                    res = a.Attributes < b.Attributes;
                    break;
            }
            return m_sortAscending ? res : !res;
        });
    }

    // Set virtual list items count. Controls virtual render loops.
    m_listView.SetItemCount(static_cast<int>(m_results.size()));

    // Notify MainFrame parent that search results count changed
    if (::IsWindow(GetParent())) {
        GetParent().SendMessage(WM_SEARCH_RESULTS_CHANGED, 0, 0);
    }
}

// Handles Virtual list request details (LVN_GETDISPINFOW)
LRESULT CSearchView::OnGetDispInfo(int idCtrl, LPNMHDR pnmh, BOOL& bHandled) {
    NMLVDISPINFO* pDispInfo = reinterpret_cast<NMLVDISPINFO*>(pnmh);
    int idx = pDispInfo->item.iItem;

    if (idx < 0 || idx >= static_cast<int>(m_results.size())) {
        return 0;
    }

    const auto& res = m_results[idx];

    // 1. Text request
    if (pDispInfo->item.mask & LVIF_TEXT) {
        int subItem = pDispInfo->item.iSubItem;
        if (subItem >= 0 && subItem < static_cast<int>(m_visibleColumns.size())) {
            int flag = m_visibleColumns[subItem].subitemIndex;
            std::wstring str;

            switch (flag) {
                case COL_NAME:
                    str = res.Name;
                    break;
                case COL_PATH:
                    str = m_searchEngine->GetResultFullPath(res);
                    // Strip the filename from full path to show parent directory only
                    if (str.size() > res.Name.size() + 1) {
                        str = str.substr(0, str.size() - res.Name.size() - 1);
                    }
                    break;
                case COL_SIZE:
                    str = res.IsDirectory ? L"" : FormatFileSize(res.Size);
                    break;
                case COL_SIZE_DISK:
                    str = res.IsDirectory ? L"" : FormatFileSize(res.SizeOnDisk);
                    break;
                case COL_MODIFIED:
                    str = FormatFileTime(res.DateModified);
                    break;
                case COL_CREATED:
                    str = FormatFileTime(res.DateCreated);
                    break;
                case COL_ACCESSED:
                    str = FormatFileTime(res.DateAccessed);
                    break;
                case COL_ATTRIBUTES:
                    str = FormatAttributes(res.Attributes);
                    break;
            }

            wcsncpy_s(pDispInfo->item.pszText, pDispInfo->item.cchTextMax, str.c_str(), _TRUNCATE);
        }
    }

    // 2. Icon request (uses cached shell image attributes without hitting physical disk)
    if (pDispInfo->item.mask & LVIF_IMAGE) {
        SHFILEINFOW sfiLocal;
        DWORD attribs = res.IsDirectory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
        
        DWORD dwStyle = m_listView.GetWindowLong(GWL_STYLE);
        DWORD dwView = dwStyle & LVS_TYPEMASK;
        DWORD iconFlags = SHGFI_SYSICONINDEX | SHGFI_USEFILEATTRIBUTES;
        if (dwView == LVS_ICON) {
            iconFlags |= SHGFI_LARGEICON;
        } else {
            iconFlags |= SHGFI_SMALLICON;
        }

        // SHGFI_USEFILEATTRIBUTES avoids disk seek completely
        SHGetFileInfoW(res.Name.c_str(), attribs, &sfiLocal, sizeof(sfiLocal), iconFlags);
        pDispInfo->item.iImage = sfiLocal.iIcon;
    }

    return 0;
}

LRESULT CSearchView::OnColumnClick(int idCtrl, LPNMHDR pnmh, BOOL& bHandled) {
    LPNMLISTVIEW pnmv = reinterpret_cast<LPNMLISTVIEW>(pnmh);
    int clickedCol = pnmv->iSubItem;

    if (clickedCol == m_sortColumn) {
        m_sortAscending = !m_sortAscending;
    } else {
        m_sortColumn = clickedCol;
        m_sortAscending = true;
    }

    RunSearchInternal();
    return 0;
}

LRESULT CSearchView::OnDblClick(int idCtrl, LPNMHDR pnmh, BOOL& bHandled) {
    int selIdx = m_listView.GetNextItem(-1, LVNI_SELECTED);
    if (selIdx == -1) return 0;

    const auto& res = m_results[selIdx];
    std::wstring fullPath = m_searchEngine->GetResultFullPath(res);

    // Subitem hit testing to determine clicked column
    DWORD pos = GetMessagePos();
    POINT pt;
    pt.x = GET_X_LPARAM(pos);
    pt.y = GET_Y_LPARAM(pos);
    m_listView.ScreenToClient(&pt);

    LVHITTESTINFO hti;
    hti.pt = pt;
    m_listView.SubItemHitTest(&hti);

    // If they double clicked on the Path column, open parent folder and select the item
    if (hti.iSubItem >= 0 && hti.iSubItem < static_cast<int>(m_visibleColumns.size()) && 
        m_visibleColumns[hti.iSubItem].subitemIndex == COL_PATH) {
        
        // standard explorer select argument
        std::wstring arg = L"/select,\"" + fullPath + L"\"";
        ShellExecuteW(NULL, L"open", L"explorer.exe", arg.c_str(), NULL, SW_SHOWNORMAL);
    } else {
        // Double clicked on filename or size: run the file/folder
        ShellExecuteW(NULL, L"open", fullPath.c_str(), NULL, NULL, SW_SHOWNORMAL);
    }

    return 0;
}

LRESULT CSearchView::OnBeginDrag(int idCtrl, LPNMHDR pnmh, BOOL& bHandled) {
    // 1. Gather all selected files
    std::vector<std::wstring> files;
    int idx = -1;
    while ((idx = m_listView.GetNextItem(idx, LVNI_SELECTED)) != -1) {
        if (idx < static_cast<int>(m_results.size())) {
            files.push_back(m_searchEngine->GetResultFullPath(m_results[idx]));
        }
    }

    if (files.empty()) return 0;

    // 2. Trigger OLE Drag and Drop
    AppShell::DragDrop::CopyFilesToClipboardOrDrag(m_hWnd, files, true);
    return 0;
}

LRESULT CSearchView::OnContextMenu(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled) {
    bHandled = FALSE;
    HWND hWndSource = (HWND)wParam;

    if (hWndSource == m_listView.m_hWnd) {
        // Retrieve cursor coordinates in screen space
        POINT ptCursor;
        ptCursor.x = GET_X_LPARAM(lParam);
        ptCursor.y = GET_Y_LPARAM(lParam);

        // If lParam is -1, it means the context menu was triggered by the keyboard (Shift+F10 / Apps key)
        if (lParam == -1) {
            int selIdx = m_listView.GetNextItem(-1, LVNI_SELECTED);
            if (selIdx != -1) {
                // Focus on the center of the selected item
                RECT rcItem;
                m_listView.GetItemRect(selIdx, &rcItem, LVIR_BOUNDS);
                ptCursor.x = (rcItem.left + rcItem.right) / 2;
                ptCursor.y = (rcItem.top + rcItem.bottom) / 2;
                m_listView.ClientToScreen(&ptCursor);
            } else {
                // Focus on center of the ListView control client area
                RECT rcClient;
                m_listView.GetClientRect(&rcClient);
                ptCursor.x = (rcClient.left + rcClient.right) / 2;
                ptCursor.y = (rcClient.top + rcClient.bottom) / 2;
                m_listView.ClientToScreen(&ptCursor);
            }
        } else {
            // Mouse click: ensure that the item under the cursor is selected if not already selected
            POINT ptClient = ptCursor;
            m_listView.ScreenToClient(&ptClient);

            LVHITTESTINFO hti = { 0 };
            hti.pt = ptClient;
            int idx = m_listView.SubItemHitTest(&hti);

            if (idx != -1) {
                if (!(m_listView.GetItemState(idx, LVIS_SELECTED) & LVIS_SELECTED)) {
                    m_listView.SetItemState(-1, 0, LVIS_SELECTED); // deselect all
                    m_listView.SetItemState(idx, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                }
            }
        }

        int selIdx = m_listView.GetNextItem(-1, LVNI_SELECTED);
        if (selIdx == -1) return 0;

        // Retrieve full paths of all selected search results
        std::vector<std::wstring> filePaths;
        int idx = -1;
        while ((idx = m_listView.GetNextItem(idx, LVNI_SELECTED)) != -1) {
            if (idx < static_cast<int>(m_results.size())) {
                filePaths.push_back(m_searchEngine->GetResultFullPath(m_results[idx]));
            }
        }

        if (filePaths.empty()) return 0;

        // Show native Explorer right-click context menu
        AppShell::ShellIntegration::ShowContextMenu(m_hWnd, filePaths, ptCursor);
        bHandled = TRUE;
    }
    return 0;
}

LRESULT CSearchView::OnKeyDown(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled) {
    bHandled = FALSE; // Let default handling proceed
    return 0;
}

LRESULT CSearchView::OnListViewKeyDown(int idCtrl, LPNMHDR pnmh, BOOL& bHandled) {
    LPNMLVKEYDOWN pKeyDown = reinterpret_cast<LPNMLVKEYDOWN>(pnmh);
    int key = pKeyDown->wVKey;
    bool ctrl = (::GetKeyState(VK_CONTROL) & 0x8000) != 0;

    bHandled = FALSE;

    if (ctrl) {
        if (key == 'C' || key == 'c') {
            CopySelection(false);
            bHandled = TRUE;
        }
        else if (key == 'X' || key == 'x') {
            CopySelection(true);
            bHandled = TRUE;
        }
        else if (key == 'A' || key == 'a') {
            // Select all search result items
            int count = m_listView.GetItemCount();
            m_listView.SetRedraw(FALSE); // Prevent flicker during mass selection
            for (int i = 0; i < count; ++i) {
                m_listView.SetItemState(i, LVIS_SELECTED, LVIS_SELECTED);
            }
            m_listView.SetRedraw(TRUE);
            m_listView.Invalidate();
            bHandled = TRUE;
        }
    }
    else if (key == VK_RETURN) {
        // Pressing Enter launches the currently selected search item
        int selIdx = m_listView.GetNextItem(-1, LVNI_SELECTED);
        if (selIdx != -1 && selIdx < static_cast<int>(m_results.size())) {
            std::wstring fullPath = m_searchEngine->GetResultFullPath(m_results[selIdx]);
            ::ShellExecuteW(NULL, L"open", fullPath.c_str(), NULL, NULL, SW_SHOWNORMAL);
            bHandled = TRUE;
        }
    }
    return 0;
}

void CSearchView::CopySelection(bool isCut) {
    std::vector<std::wstring> files;
    int idx = -1;
    while ((idx = m_listView.GetNextItem(idx, LVNI_SELECTED)) != -1) {
        if (idx < static_cast<int>(m_results.size())) {
            files.push_back(m_searchEngine->GetResultFullPath(m_results[idx]));
        }
    }
    if (!files.empty()) {
        AppShell::ShellIntegration::CopyFilesToClipboard(m_hWnd, files, isCut);
    }
}

bool CSearchView::ExportResults(const std::wstring& filePath) {
    FILE* file = nullptr;
    if (_wfopen_s(&file, filePath.c_str(), L"w, ccs=UTF-8") != 0 || !file) {
        return false;
    }

    // Write header
    fwprintf(file, L"Name\tPath\tSize\tSize on Disk\tDate Modified\n");

    for (const auto& res : m_results) {
        std::wstring fullPath = m_searchEngine->GetResultFullPath(res);
        std::wstring sizeStr = res.IsDirectory ? L"" : FormatFileSize(res.Size);
        std::wstring sizeDiskStr = res.IsDirectory ? L"" : FormatFileSize(res.SizeOnDisk);
        std::wstring modStr = FormatFileTime(res.DateModified);

        fwprintf(file, L"%s\t%s\t%s\t%s\t%s\n", res.Name.c_str(), fullPath.c_str(), 
                 sizeStr.c_str(), sizeDiskStr.c_str(), modStr.c_str());
    }

    fclose(file);
    return true;
}

void CSearchView::SetViewMode(int mode) {
    HIMAGELIST hImgList = nullptr;
    const IID iidImageList = { 0x46EB2DE8, 0xBE6F, 0x11d2, { 0xB8, 0x5B, 0x00, 0xC0, 0x4F, 0xC4, 0x94, 0xFD } }; // IID_IImageList
    
    if (mode == ID_VIEW_DETAILS) {
        m_listView.ModifyStyle(LVS_TYPEMASK, LVS_REPORT, SWP_FRAMECHANGED);
        ::SHGetImageList(SHIL_SMALL, iidImageList, (void**)&hImgList);
        if (hImgList) {
            m_listView.SetImageList(hImgList, LVSIL_SMALL);
        }
    }
    else if (mode == ID_VIEW_SMALL_ICONS) {
        m_listView.ModifyStyle(LVS_TYPEMASK, LVS_SMALLICON, SWP_FRAMECHANGED);
        ::SHGetImageList(SHIL_SMALL, iidImageList, (void**)&hImgList);
        if (hImgList) {
            m_listView.SetImageList(hImgList, LVSIL_SMALL);
        }
    }
    else if (mode == ID_VIEW_MEDIUM_ICONS) {
        m_listView.ModifyStyle(LVS_TYPEMASK, LVS_ICON, SWP_FRAMECHANGED);
        ::SHGetImageList(SHIL_LARGE, iidImageList, (void**)&hImgList);
        if (hImgList) {
            m_listView.SetImageList(hImgList, LVSIL_NORMAL);
        }
    }
    else if (mode == ID_VIEW_LARGE_ICONS) {
        m_listView.ModifyStyle(LVS_TYPEMASK, LVS_ICON, SWP_FRAMECHANGED);
        ::SHGetImageList(SHIL_EXTRALARGE, iidImageList, (void**)&hImgList);
        if (hImgList) {
            m_listView.SetImageList(hImgList, LVSIL_NORMAL);
        }
    }
    else if (mode == ID_VIEW_EXTRA_LARGE_ICONS) {
        m_listView.ModifyStyle(LVS_TYPEMASK, LVS_ICON, SWP_FRAMECHANGED);
        ::SHGetImageList(SHIL_JUMBO, iidImageList, (void**)&hImgList);
        if (hImgList) {
            m_listView.SetImageList(hImgList, LVSIL_NORMAL);
        }
    }
    m_listView.Invalidate();
}

std::wstring CSearchView::FormatFileSize(unsigned long long size) const {
    if (size == 0) return L"0 KB";

    // Standard Everything layout: values formatted in KB with comma grouping separators
    double sizeKb = static_cast<double>(size) / 1024.0;
    
    std::wstringstream wss;
    wss.imbue(std::locale("")); // Uses native system formatting for comma separator
    wss << std::fixed << std::setprecision(0) << ceil(sizeKb) << L" KB";
    return wss.str();
}

std::wstring CSearchView::FormatFileTime(unsigned long long time) const {
    if (time == 0) return L"";

    FILETIME ft;
    ft.dwHighDateTime = static_cast<DWORD>(time >> 32);
    ft.dwLowDateTime = static_cast<DWORD>(time & 0xFFFFFFFF);

    SYSTEMTIME st;
    if (!FileTimeToSystemTime(&ft, &st)) return L"";

    SYSTEMTIME localSt;
    if (!SystemTimeToTzSpecificLocalTime(NULL, &st, &localSt)) return L"";

    wchar_t buf[100];
    swprintf_s(buf, 100, L"%04d-%02d-%02d %02d:%02d:%02d", 
               localSt.wYear, localSt.wMonth, localSt.wDay, 
               localSt.wHour, localSt.wMinute, localSt.wSecond);
    return buf;
}

std::wstring CSearchView::FormatAttributes(unsigned int attribs) const {
    std::wstring str;
    if (attribs & FILE_ATTRIBUTE_DIRECTORY) str += L"D";
    if (attribs & FILE_ATTRIBUTE_READONLY)  str += L"R";
    if (attribs & FILE_ATTRIBUTE_HIDDEN)    str += L"H";
    if (attribs & FILE_ATTRIBUTE_SYSTEM)    str += L"S";
    if (attribs & FILE_ATTRIBUTE_ARCHIVE)   str += L"A";
    if (attribs & FILE_ATTRIBUTE_COMPRESSED) str += L"C";
    if (attribs & FILE_ATTRIBUTE_ENCRYPTED)  str += L"E";
    if (attribs & FILE_ATTRIBUTE_SPARSE_FILE) str += L"S";
    if (attribs & FILE_ATTRIBUTE_REPARSE_POINT) str += L"L";
    return str;
}

LRESULT CSearchView::OnEditChar(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled) {
    bHandled = FALSE;
    // Intercept WM_CHAR for Ctrl+Backspace (which is char code 127) to prevent Edit control from writing standard DEL block character
    if (wParam == 127 || wParam == 0x7F) {
        bHandled = TRUE; // Suppress character insertion completely
    }
    return 0;
}

LRESULT CSearchView::OnEditKeyDown(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled) {
    bHandled = FALSE;
    int key = static_cast<int>(wParam);
    bool ctrl = (::GetKeyState(VK_CONTROL) & 0x8000) != 0;

    if (key == VK_BACK && ctrl) {
        // Handle Ctrl + Backspace to delete the previous word
        int startSel = 0, endSel = 0;
        m_searchEdit.GetSel(startSel, endSel);

        if (startSel != endSel) {
            // Delete current selection
            m_searchEdit.ReplaceSel(L"");
        } else if (startSel > 0) {
            // Retrieve edit box text
            int len = m_searchEdit.GetWindowTextLength();
            std::wstring text;
            text.resize(len);
            m_searchEdit.GetWindowText(&text[0], len + 1);
            text.resize(len);

            int pos = startSel;
            
            // 1. Skip trailing spaces backward
            while (pos > 0 && iswspace(text[pos - 1])) {
                pos--;
            }

            // 2. Skip preceding word characters backward
            if (pos > 0) {
                if (iswalnum(text[pos - 1])) {
                    while (pos > 0 && iswalnum(text[pos - 1])) {
                        pos--;
                    }
                } else {
                    // Skip a single non-alphanumeric punctuation character
                    pos--;
                }
            }

            // Select the word block and replace it with an empty string (deletes it)
            m_searchEdit.SetSel(pos, startSel);
            m_searchEdit.ReplaceSel(L"");
        }
        bHandled = TRUE; // Suppress default processing of VK_BACK
    }
    return 0;
}

LRESULT CSearchView::OnListViewKeyDownMessage(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled) {
    bHandled = FALSE;
    int key = static_cast<int>(wParam);
    bool ctrl = (::GetKeyState(VK_CONTROL) & 0x8000) != 0;

    if (ctrl) {
        if (key == 'C' || key == 'c') {
            CopySelection(false);
            bHandled = TRUE;
        }
        else if (key == 'X' || key == 'x') {
            CopySelection(true);
            bHandled = TRUE;
        }
        else if (key == 'A' || key == 'a') {
            // Select all search result items
            int count = m_listView.GetItemCount();
            m_listView.SetRedraw(FALSE); // Prevent flicker during mass selection
            for (int i = 0; i < count; ++i) {
                m_listView.SetItemState(i, LVIS_SELECTED, LVIS_SELECTED);
            }
            m_listView.SetRedraw(TRUE);
            m_listView.Invalidate();
            bHandled = TRUE;
        }
    }
    else if (key == VK_RETURN) {
        // Pressing Enter launches the currently selected search item
        int selIdx = m_listView.GetNextItem(-1, LVNI_SELECTED);
        if (selIdx != -1 && selIdx < static_cast<int>(m_results.size())) {
            std::wstring fullPath = m_searchEngine->GetResultFullPath(m_results[selIdx]);
            ::ShellExecuteW(NULL, L"open", fullPath.c_str(), NULL, NULL, SW_SHOWNORMAL);
            bHandled = TRUE;
        }
    }
    return 0;
}

LRESULT CSearchView::OnCustomDraw(int idCtrl, LPNMHDR pnmh, BOOL& bHandled) {
    LPNMLVCUSTOMDRAW pLVCD = reinterpret_cast<LPNMLVCUSTOMDRAW>(pnmh);
    bHandled = TRUE;

    switch (pLVCD->nmcd.dwDrawStage) {
        case CDDS_PREPAINT:
            return CDRF_NOTIFYITEMDRAW;

        case CDDS_ITEMPREPAINT:
            return CDRF_NOTIFYSUBITEMDRAW;

        case CDDS_SUBITEM | CDDS_ITEMPREPAINT: {
            int itemIdx = static_cast<int>(pLVCD->nmcd.dwItemSpec);
            int subItemIdx = pLVCD->iSubItem;

            if (itemIdx < 0 || itemIdx >= static_cast<int>(m_results.size()) ||
                subItemIdx < 0 || subItemIdx >= static_cast<int>(m_visibleColumns.size())) {
                return CDRF_DODEFAULT;
            }

            // Return default drawing if we are not in details (report) view mode
            DWORD dwStyle = m_listView.GetWindowLong(GWL_STYLE);
            if ((dwStyle & LVS_TYPEMASK) != LVS_REPORT) {
                return CDRF_DODEFAULT;
            }

            int flag = m_visibleColumns[subItemIdx].subitemIndex;

            // We only perform matched term highlighting and icon drawing on the Name column (COL_NAME)
            if (flag == COL_NAME) {
                HDC hdc = pLVCD->nmcd.hdc;
                RECT rc;
                // Query complete bounds of the cell to cover the icon space in our highlight
                m_listView.GetSubItemRect(itemIdx, subItemIdx, LVIR_BOUNDS, &rc);

                // Set text color and background brush depending on selection state
                bool isSelected = (m_listView.GetItemState(itemIdx, LVIS_SELECTED) & LVIS_SELECTED) != 0;
                bool isFocused = (::GetFocus() == m_listView.m_hWnd);
                COLORREF clrBg = isSelected ? ::GetSysColor(isFocused ? COLOR_HIGHLIGHT : COLOR_BTNFACE) : ::GetSysColor(COLOR_WINDOW);
                COLORREF clrText = isSelected ? ::GetSysColor(isFocused ? COLOR_HIGHLIGHTTEXT : COLOR_BTNTEXT) : ::GetSysColor(COLOR_WINDOWTEXT);

                // Clear background of the cell
                HBRUSH hBgBrush = ::CreateSolidBrush(clrBg);
                ::FillRect(hdc, &rc, hBgBrush);
                ::DeleteObject(hBgBrush);

                // Get shell icon index (matches exactly how we retrieve it in OnGetDispInfo)
                const auto& res = m_results[itemIdx];
                SHFILEINFOW sfiLocal;
                DWORD attribs = res.IsDirectory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
                SHGetFileInfoW(res.Name.c_str(), attribs, &sfiLocal, sizeof(sfiLocal), 
                               SHGFI_SYSICONINDEX | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES);

                // Get the Small ImageList associated with the ListView
                HIMAGELIST hSmallImgList = m_listView.GetImageList(LVSIL_SMALL);
                if (hSmallImgList && sfiLocal.iIcon >= 0) {
                    // Draw icon (16x16) centered vertically on the left
                    int iconX = rc.left + 2;
                    int iconY = rc.top + (rc.bottom - rc.top - 16) / 2;
                    ::ImageList_Draw(hSmallImgList, sfiLocal.iIcon, hdc, iconX, iconY, ILD_TRANSPARENT);
                }

                COLORREF clrHighlightText = RGB(0, 0, 0); // black text on yellow background
                COLORREF clrHighlightBg = RGB(255, 235, 156); // modern soft yellow highlight background

                // Set font
                HFONT hOldFont = (HFONT)::SelectObject(hdc, m_listView.GetFont());

                // Position the text starting after the icon (padding: 2px margin + 16px icon + 6px space = 24px)
                rc.left += 24;

                // Draw segments
                const std::wstring& name = res.Name;

                // Obtain the active search words from StringMatcher
                const auto& wordsLower = m_searchEngine->GetLastMatcher().GetWordsLower();

                // Compute segments
                std::wstring nameLower = name;
                std::transform(name.begin(), name.end(), nameLower.begin(), ::towlower);

                std::vector<bool> highlighted(name.length(), false);
                for (const auto& w : wordsLower) {
                    if (w.empty()) continue;
                    size_t pos = nameLower.find(w);
                    while (pos != std::wstring::npos) {
                        for (size_t i = 0; i < w.length(); ++i) {
                            highlighted[pos + i] = true;
                        }
                        pos = nameLower.find(w, pos + 1);
                    }
                }

                struct TextSegment {
                    std::wstring text;
                    bool highlight;
                };
                std::vector<TextSegment> segments;

                if (!name.empty()) {
                    bool currentState = highlighted[0];
                    size_t start = 0;
                    for (size_t i = 1; i < name.length(); ++i) {
                        if (highlighted[i] != currentState) {
                            segments.push_back({ name.substr(start, i - start), currentState });
                            currentState = highlighted[i];
                            start = i;
                        }
                    }
                    segments.push_back({ name.substr(start), currentState });
                }

                int x = rc.left;
                for (const auto& seg : segments) {
                    SIZE sz;
                    ::GetTextExtentPoint32W(hdc, seg.text.c_str(), static_cast<int>(seg.text.length()), &sz);

                    RECT rcSeg = { x, rc.top, x + sz.cx, rc.bottom };
                    
                    if (seg.highlight) {
                        // Draw soft yellow background for matched word
                        HBRUSH hHighlightBrush = ::CreateSolidBrush(clrHighlightBg);
                        ::FillRect(hdc, &rcSeg, hHighlightBrush);
                        ::DeleteObject(hHighlightBrush);
                        
                        ::SetTextColor(hdc, clrHighlightText);
                        ::SetBkMode(hdc, TRANSPARENT);
                    } else {
                        ::SetTextColor(hdc, clrText);
                        ::SetBkMode(hdc, TRANSPARENT);
                    }

                    ::DrawTextW(hdc, seg.text.c_str(), static_cast<int>(seg.text.length()), &rcSeg, 
                                DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);

                    x += sz.cx;
                }

                ::SelectObject(hdc, hOldFont);
                return CDRF_SKIPDEFAULT; // Skip standard paint for this subitem
            }
            break;
        }
    }
    return CDRF_DODEFAULT;
}
