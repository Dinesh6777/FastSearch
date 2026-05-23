#include "stdafx.h"
#include "SettingsDialog.h"

CSettingsDialog::CSettingsDialog(unsigned int columnMask) : m_columnMask(columnMask) {
}

CSettingsDialog::~CSettingsDialog() {
}

LRESULT CSettingsDialog::OnInitDialog(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled) {
    CenterWindow(GetParent());

    // Name and Path columns are mandatory
    CheckDlgButton(IDC_CHECK_NAME, BST_CHECKED);
    CheckDlgButton(IDC_CHECK_PATH, BST_CHECKED);

    // Initial check state based on current column mask
    CheckDlgButton(IDC_CHECK_SIZE, (m_columnMask & COL_SIZE) ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(IDC_CHECK_SIZE_DISK, (m_columnMask & COL_SIZE_DISK) ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(IDC_CHECK_MODIFIED, (m_columnMask & COL_MODIFIED) ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(IDC_CHECK_CREATED, (m_columnMask & COL_CREATED) ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(IDC_CHECK_ACCESSED, (m_columnMask & COL_ACCESSED) ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(IDC_CHECK_ATTRIBUTES, (m_columnMask & COL_ATTRIBUTES) ? BST_CHECKED : BST_UNCHECKED);

    return TRUE;
}

LRESULT CSettingsDialog::OnOk(WORD wNotifyCode, WORD wID, HWND hWndCtl, BOOL& bHandled) {
    // Reconstruct column bitmask based on checkbox selections
    m_columnMask = COL_NAME | COL_PATH; // Mandatory fields

    if (IsDlgButtonChecked(IDC_CHECK_SIZE) == BST_CHECKED)      m_columnMask |= COL_SIZE;
    if (IsDlgButtonChecked(IDC_CHECK_SIZE_DISK) == BST_CHECKED) m_columnMask |= COL_SIZE_DISK;
    if (IsDlgButtonChecked(IDC_CHECK_MODIFIED) == BST_CHECKED)  m_columnMask |= COL_MODIFIED;
    if (IsDlgButtonChecked(IDC_CHECK_CREATED) == BST_CHECKED)   m_columnMask |= COL_CREATED;
    if (IsDlgButtonChecked(IDC_CHECK_ACCESSED) == BST_CHECKED)  m_columnMask |= COL_ACCESSED;
    if (IsDlgButtonChecked(IDC_CHECK_ATTRIBUTES) == BST_CHECKED) m_columnMask |= COL_ATTRIBUTES;

    EndDialog(IDOK);
    return 0;
}

LRESULT CSettingsDialog::OnCancel(WORD wNotifyCode, WORD wID, HWND hWndCtl, BOOL& bHandled) {
    EndDialog(IDCANCEL);
    return 0;
}
