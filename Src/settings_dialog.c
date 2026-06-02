#include "settings_dialog.h"
#include "resource.h"

static INT_PTR CALLBACK SettingsDlgProc(HWND hwndDlg, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
        case WM_INITDIALOG: {
            unsigned int* pMask = (unsigned int*)lParam;
            SetWindowLongPtr(hwndDlg, GWLP_USERDATA, (LONG_PTR)pMask);

            // Center dialog over parent window
            HWND hwndParent = GetParent(hwndDlg);
            if (hwndParent) {
                RECT rcParent, rcDlg;
                GetWindowRect(hwndParent, &rcParent);
                GetWindowRect(hwndDlg, &rcDlg);
                int x = rcParent.left + (rcParent.right - rcParent.left - (rcDlg.right - rcDlg.left)) / 2;
                int y = rcParent.top + (rcParent.bottom - rcParent.top - (rcDlg.bottom - rcDlg.top)) / 2;
                SetWindowPos(hwndDlg, NULL, x, y, 0, 0, SWP_NOZORDER | SWP_NOSIZE);
            }

            // Name and Path columns are mandatory
            CheckDlgButton(hwndDlg, IDC_CHECK_NAME, BST_CHECKED);
            CheckDlgButton(hwndDlg, IDC_CHECK_PATH, BST_CHECKED);

            unsigned int mask = *pMask;
            CheckDlgButton(hwndDlg, IDC_CHECK_SIZE, (mask & COL_SIZE) ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hwndDlg, IDC_CHECK_SIZE_DISK, (mask & COL_SIZE_DISK) ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hwndDlg, IDC_CHECK_MODIFIED, (mask & COL_MODIFIED) ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hwndDlg, IDC_CHECK_CREATED, (mask & COL_CREATED) ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hwndDlg, IDC_CHECK_ACCESSED, (mask & COL_ACCESSED) ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hwndDlg, IDC_CHECK_ATTRIBUTES, (mask & COL_ATTRIBUTES) ? BST_CHECKED : BST_UNCHECKED);

            return TRUE;
        }
        case WM_COMMAND: {
            WORD id = LOWORD(wParam);
            if (id == IDOK) {
                unsigned int* pMask = (unsigned int*)GetWindowLongPtr(hwndDlg, GWLP_USERDATA);
                if (pMask) {
                    unsigned int newMask = COL_NAME | COL_PATH; // Mandatory
                    if (IsDlgButtonChecked(hwndDlg, IDC_CHECK_SIZE) == BST_CHECKED) newMask |= COL_SIZE;
                    if (IsDlgButtonChecked(hwndDlg, IDC_CHECK_SIZE_DISK) == BST_CHECKED) newMask |= COL_SIZE_DISK;
                    if (IsDlgButtonChecked(hwndDlg, IDC_CHECK_MODIFIED) == BST_CHECKED) newMask |= COL_MODIFIED;
                    if (IsDlgButtonChecked(hwndDlg, IDC_CHECK_CREATED) == BST_CHECKED) newMask |= COL_CREATED;
                    if (IsDlgButtonChecked(hwndDlg, IDC_CHECK_ACCESSED) == BST_CHECKED) newMask |= COL_ACCESSED;
                    if (IsDlgButtonChecked(hwndDlg, IDC_CHECK_ATTRIBUTES) == BST_CHECKED) newMask |= COL_ATTRIBUTES;
                    
                    *pMask = newMask;
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

bool SettingsDialog_Show(HWND hwndParent, unsigned int* columnMask) {
    INT_PTR res = DialogBoxParamW(
        GetModuleHandle(NULL),
        MAKEINTRESOURCE(IDD_SETTINGS),
        hwndParent,
        SettingsDlgProc,
        (LPARAM)columnMask
    );
    return res == IDOK;
}
