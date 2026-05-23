#pragma once
#include "stdafx.h"
#include "resource.h"

#define IDD_INPUT 302
#define IDC_INPUT_EDIT 1008
#define IDC_INPUT_LABEL 1009

class CInputDialog : public CDialogImpl<CInputDialog> {
public:
    enum { IDD = IDD_INPUT };

    CInputDialog(const std::wstring& title, const std::wstring& label, const std::wstring& defaultValue = L"")
        : m_title(title), m_label(label), m_value(defaultValue) {}

    std::wstring GetValue() const { return m_value; }

    BEGIN_MSG_MAP(CInputDialog)
        MESSAGE_HANDLER(WM_INITDIALOG, OnInitDialog)
        COMMAND_ID_HANDLER(IDOK, OnOk)
        COMMAND_ID_HANDLER(IDCANCEL, OnCancel)
    END_MSG_MAP()

private:
    LRESULT OnInitDialog(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled) {
        SetWindowTextW(m_title.c_str());
        SetDlgItemTextW(IDC_INPUT_LABEL, m_label.c_str());
        SetDlgItemTextW(IDC_INPUT_EDIT, m_value.c_str());
        
        // Set focus to edit control and select text
        HWND hEdit = GetDlgItem(IDC_INPUT_EDIT);
        ::SetFocus(hEdit);
        ::SendMessageW(hEdit, EM_SETSEL, 0, -1);
        return FALSE; // returning FALSE because we manually set focus
    }

    LRESULT OnOk(WORD wNotifyCode, WORD wID, HWND hWndCtl, BOOL& bHandled) {
        wchar_t buf[512] = { 0 };
        GetDlgItemTextW(IDC_INPUT_EDIT, buf, 512);
        m_value = buf;
        EndDialog(IDOK);
        return 0;
    }

    LRESULT OnCancel(WORD wNotifyCode, WORD wID, HWND hWndCtl, BOOL& bHandled) {
        EndDialog(IDCANCEL);
        return 0;
    }

    std::wstring m_title;
    std::wstring m_label;
    std::wstring m_value;
};
