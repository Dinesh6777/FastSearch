#pragma once
#include "stdafx.h"
#include "resource.h"

// Columns settings bitmask flags
enum ColumnFlags {
    COL_NAME = 1 << 0,
    COL_PATH = 1 << 1,
    COL_SIZE = 1 << 2,
    COL_SIZE_DISK = 1 << 3,
    COL_MODIFIED = 1 << 4,
    COL_CREATED = 1 << 5,
    COL_ACCESSED = 1 << 6,
    COL_ATTRIBUTES = 1 << 7,
    
    COL_DEFAULT = COL_NAME | COL_PATH | COL_SIZE | COL_MODIFIED
};

// class CSettingsDialog
// A modal WTL dialog box for toggling list column visibility.
class CSettingsDialog : public CDialogImpl<CSettingsDialog> {
public:
    enum { IDD = IDD_SETTINGS };

    CSettingsDialog(unsigned int columnMask);
    ~CSettingsDialog();

    unsigned int GetColumnMask() const { return m_columnMask; }

    BEGIN_MSG_MAP(CSettingsDialog)
        MESSAGE_HANDLER(WM_INITDIALOG, OnInitDialog)
        COMMAND_ID_HANDLER(IDOK, OnOk)
        COMMAND_ID_HANDLER(IDCANCEL, OnCancel)
    END_MSG_MAP()

private:
    LRESULT OnInitDialog(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled);
    LRESULT OnOk(WORD wNotifyCode, WORD wID, HWND hWndCtl, BOOL& bHandled);
    LRESULT OnCancel(WORD wNotifyCode, WORD wID, HWND hWndCtl, BOOL& bHandled);

    unsigned int m_columnMask;
};
