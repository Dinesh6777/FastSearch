#pragma once
#include "stdafx.h"
#include "resource.h"
#include "SearchEngine.h"
#include "SearchView.h"

// Define custom window messages
#define WM_TRAYICON (WM_USER + 100)

// class CMainFrame
// The primary shell frame hosting the tab control, status bar, system tray integrations,
// and single-instance locks.
class CMainFrame : public CFrameWindowImpl<CMainFrame>,
                   public Search::IIndexProgressCallback {
public:
    DECLARE_FRAME_WND_CLASS(_T("FastSearchMainFrame"), IDR_MAINFRAME)

    CMainFrame();
    ~CMainFrame();

    // IndexProgressCallback implementations
    void OnIndexProgress(wchar_t drive, unsigned int current, unsigned int total) override;
    void OnIndexComplete(wchar_t drive, bool success, unsigned int fileCount, unsigned int folderCount) override;

    BEGIN_MSG_MAP(CMainFrame)
        MESSAGE_HANDLER(WM_CREATE, OnCreate)
        MESSAGE_HANDLER(WM_CLOSE, OnClose)
        MESSAGE_HANDLER(WM_DESTROY, OnDestroy)
        MESSAGE_HANDLER(WM_SIZE, OnSize)
        MESSAGE_HANDLER(WM_SETFOCUS, OnSetFocus)
        MESSAGE_HANDLER(WM_TRAYICON, OnTrayIcon)
        MESSAGE_HANDLER(WM_DEVICECHANGE, OnDeviceChange)
        MESSAGE_HANDLER(WM_SEARCH_RESULTS_CHANGED, OnSearchResultsChanged)
        MESSAGE_HANDLER(WM_NTFS_INDEX_CHANGED, OnNtfsIndexChanged)
        
        // Tab notifications
        NOTIFY_CODE_HANDLER(TCN_SELCHANGE, OnTabSelChange)
        NOTIFY_HANDLER(IDC_TAB_CTRL, NM_CLICK, OnTabClick)

        // Menu commands
        COMMAND_ID_HANDLER(ID_FILE_NEW_TAB, OnNewTabCmd)
        COMMAND_ID_HANDLER(ID_FILE_CLOSE_TAB, OnCloseTabCmd)
        COMMAND_ID_HANDLER(ID_FILE_EXPORT, OnExportCmd)
        COMMAND_ID_HANDLER(ID_FILE_EXIT, OnExitCmd)
        COMMAND_ID_HANDLER(ID_VIEW_SETTINGS, OnSettingsCmd)
        COMMAND_ID_HANDLER(ID_HELP_ABOUT, OnAboutCmd)
        COMMAND_RANGE_HANDLER(40020, 40024, OnViewModeCmd)

        // System Tray Commands
        COMMAND_ID_HANDLER(ID_TRAY_RESTORE, OnTrayRestoreCmd)
        COMMAND_ID_HANDLER(ID_TRAY_EXIT, OnTrayExitCmd)
        COMMAND_ID_HANDLER(ID_TRAY_NEW_WINDOW, OnTrayNewWindowCmd)

        CHAIN_MSG_MAP(CFrameWindowImpl<CMainFrame>)
    END_MSG_MAP()

private:
    LRESULT OnCreate(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled);
    LRESULT OnClose(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled);
    LRESULT OnDestroy(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled);
    LRESULT OnSize(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled);
    LRESULT OnSetFocus(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled);
    LRESULT OnTrayIcon(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled);
    LRESULT OnDeviceChange(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled);
    LRESULT OnSearchResultsChanged(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled);
    LRESULT OnNtfsIndexChanged(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled);

    // Tab Control notifications
    LRESULT OnTabSelChange(int idCtrl, LPNMHDR pnmh, BOOL& bHandled);
    LRESULT OnTabClick(int idCtrl, LPNMHDR pnmh, BOOL& bHandled);

    // Command Handlers
    LRESULT OnNewTabCmd(WORD wNotifyCode, WORD wID, HWND hWndCtl, BOOL& bHandled);
    LRESULT OnCloseTabCmd(WORD wNotifyCode, WORD wID, HWND hWndCtl, BOOL& bHandled);
    LRESULT OnExportCmd(WORD wNotifyCode, WORD wID, HWND hWndCtl, BOOL& bHandled);
    LRESULT OnExitCmd(WORD wNotifyCode, WORD wID, HWND hWndCtl, BOOL& bHandled);
    LRESULT OnSettingsCmd(WORD wNotifyCode, WORD wID, HWND hWndCtl, BOOL& bHandled);
    LRESULT OnAboutCmd(WORD wNotifyCode, WORD wID, HWND hWndCtl, BOOL& bHandled);
    LRESULT OnViewModeCmd(WORD wNotifyCode, WORD wID, HWND hWndCtl, BOOL& bHandled);

    // Tray Menu Command Handlers
    LRESULT OnTrayRestoreCmd(WORD wNotifyCode, WORD wID, HWND hWndCtl, BOOL& bHandled);
    LRESULT OnTrayExitCmd(WORD wNotifyCode, WORD wID, HWND hWndCtl, BOOL& bHandled);
    LRESULT OnTrayNewWindowCmd(WORD wNotifyCode, WORD wID, HWND hWndCtl, BOOL& bHandled);

    // Tab Management
    void CreateNewTab(const std::wstring& name);
    void CloseTab(int tabIndex);
    void UpdateStatusText();
    void UpdateDriveComboBoxes();
    
    // System Tray Management
    void AddTrayIcon();
    void RemoveTrayIcon();

    Search::SearchEngine m_searchEngine;
    HDEVNOTIFY m_hDevNotify = nullptr;
    
    // UI controls
    CTabCtrl m_tabCtrl;
    CMultiPaneStatusBarCtrl m_statusBar;
    unsigned int m_columnMask; // current columns mask from settings

    struct TabContext {
        std::wstring Name;
        std::unique_ptr<CSearchView> View;
    };
    std::vector<TabContext> m_tabs;
    int m_activeTabIndex;
};
