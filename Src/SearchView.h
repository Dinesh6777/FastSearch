#pragma once
#include "stdafx.h"
#include "SearchEngine.h"
#include "SettingsDialog.h"

// class CSearchView
// A child window representing an individual search tab.
// Hosts the search inputs, filters, and a high-performance virtual ListView (LVS_OWNERDATA).
class CSearchView : public CWindowImpl<CSearchView> {
public:
    CSearchView(Search::SearchEngine* searchEngine);
    ~CSearchView();

    DECLARE_WND_CLASS(_T("FastSearchView"))

    // Updates column layouts based on current column visibility settings
    void UpdateColumns(unsigned int columnMask);

    // Forces a search refresh
    void TriggerSearch();

    // Export current results to file
    bool ExportResults(const std::wstring& filePath);

    // Sets the visual view mode (Details, Large Icons, Small Icons, etc.)
    void SetViewMode(int mode);

    // Retrieves current result count for parent status updates
    int GetResultCount() const { return static_cast<int>(m_results.size()); }

    // Retrieves current view mode
    int GetViewMode() const { return m_viewMode; }

    // Retrieves last search execution time in milliseconds
    double GetLastSearchTimeMs() const { return m_lastSearchTimeMs; }

    BEGIN_MSG_MAP(CSearchView)
        MESSAGE_HANDLER(WM_CREATE, OnCreate)
        MESSAGE_HANDLER(WM_SIZE, OnSize)
        MESSAGE_HANDLER(WM_TIMER, OnTimer)
        MESSAGE_HANDLER(WM_KEYDOWN, OnKeyDown)
        MESSAGE_HANDLER(WM_CONTEXTMENU, OnContextMenu)
        MESSAGE_HANDLER(WM_NTFS_INDEX_CHANGED, OnNtfsIndexChanged)
        
        // Input control change handlers
        COMMAND_HANDLER(IDC_SEARCH_EDIT, EN_CHANGE, OnSearchChanged)
        COMMAND_HANDLER(IDC_FILTER_COMBO, CBN_SELCHANGE, OnFilterChanged)
        COMMAND_HANDLER(IDC_VOLUME_COMBO, CBN_SELCHANGE, OnVolumeChanged)
        COMMAND_HANDLER(IDC_CASE_CHECK, BN_CLICKED, OnSearchOptionChanged)
        COMMAND_HANDLER(IDC_REGEX_CHECK, BN_CLICKED, OnSearchOptionChanged)

        // Virtual ListView notifications
        NOTIFY_HANDLER(IDC_RESULTS_LIST, LVN_GETDISPINFOW, OnGetDispInfo)
        NOTIFY_HANDLER(IDC_RESULTS_LIST, LVN_COLUMNCLICK, OnColumnClick)
        NOTIFY_HANDLER(IDC_RESULTS_LIST, NM_DBLCLK, OnDblClick)
        NOTIFY_HANDLER(IDC_RESULTS_LIST, LVN_BEGINDRAG, OnBeginDrag)
        NOTIFY_HANDLER(IDC_RESULTS_LIST, LVN_KEYDOWN, OnListViewKeyDown)
        NOTIFY_HANDLER(IDC_RESULTS_LIST, NM_CUSTOMDRAW, OnCustomDraw)
    ALT_MSG_MAP(1) // Map m_searchEdit subclassed messages
        MESSAGE_HANDLER(WM_CHAR, OnEditChar)
        MESSAGE_HANDLER(WM_KEYDOWN, OnEditKeyDown)
        MESSAGE_HANDLER(WM_SETFOCUS, OnEditSetFocus)
        MESSAGE_HANDLER(WM_LBUTTONDOWN, OnEditLButtonDown)
    ALT_MSG_MAP(2) // Map m_listView subclassed keyboard events (100% robust hotkeys)
        MESSAGE_HANDLER(WM_KEYDOWN, OnListViewKeyDownMessage)
    END_MSG_MAP()

private:
    LRESULT OnCreate(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled);
    LRESULT OnSize(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled);
    LRESULT OnTimer(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled);
    LRESULT OnNtfsIndexChanged(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled);
    LRESULT OnKeyDown(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled);
    LRESULT OnListViewKeyDown(int idCtrl, LPNMHDR pnmh, BOOL& bHandled);
    LRESULT OnListViewKeyDownMessage(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled);
    LRESULT OnCustomDraw(int idCtrl, LPNMHDR pnmh, BOOL& bHandled);
    LRESULT OnContextMenu(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled);
    LRESULT OnEditChar(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled);
    LRESULT OnEditKeyDown(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled);
    LRESULT OnEditSetFocus(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled);
    LRESULT OnEditLButtonDown(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled);

    void CopySelection(bool isCut);

    // Command Handlers
    LRESULT OnSearchChanged(WORD wNotifyCode, WORD wID, HWND hWndCtl, BOOL& bHandled);
    LRESULT OnFilterChanged(WORD wNotifyCode, WORD wID, HWND hWndCtl, BOOL& bHandled);
    LRESULT OnVolumeChanged(WORD wNotifyCode, WORD wID, HWND hWndCtl, BOOL& bHandled);
    LRESULT OnSearchOptionChanged(WORD wNotifyCode, WORD wID, HWND hWndCtl, BOOL& bHandled);

    // ListView Notifications
    LRESULT OnGetDispInfo(int idCtrl, LPNMHDR pnmh, BOOL& bHandled);
    LRESULT OnColumnClick(int idCtrl, LPNMHDR pnmh, BOOL& bHandled);
    LRESULT OnDblClick(int idCtrl, LPNMHDR pnmh, BOOL& bHandled);
    LRESULT OnBeginDrag(int idCtrl, LPNMHDR pnmh, BOOL& bHandled);

    // Search orchestrators
    void RunSearchInternal();
    std::wstring FormatFileSize(unsigned long long size) const;
    std::wstring FormatFileTime(unsigned long long time) const;
    std::wstring FormatAttributes(unsigned int attribs) const;

    Search::SearchEngine* m_searchEngine;
    
    // UI Elements
    CContainedWindowT<CEdit> m_searchEdit;
    CComboBox m_volumeCombo;
    CComboBox m_filterCombo;
    CButton m_caseCheck;
    CButton m_regexCheck;
    CContainedWindowT<CListViewCtrl> m_listView;

    // Search State
    std::vector<Search::SearchResult> m_results;
    std::wstring m_currentQuery;
    unsigned int m_columnMask;
    int m_viewMode;
    double m_lastSearchTimeMs;
    
    // Sorting State
    int m_sortColumn; // -1 for none, or column index
    bool m_sortAscending;

    // ListView logical column index mapping
    struct ColumnHeader {
        std::wstring text;
        int width;
        int subitemIndex; // mapped to ColumnFlags
    };
    std::vector<ColumnHeader> m_visibleColumns;
};
