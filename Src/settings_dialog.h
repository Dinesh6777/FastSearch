#ifndef SETTINGS_DIALOG_H
#define SETTINGS_DIALOG_H

#include "fs_common.h"

// Columns settings bitmask flags
typedef enum {
    COL_NAME = 1 << 0,
    COL_PATH = 1 << 1,
    COL_SIZE = 1 << 2,
    COL_SIZE_DISK = 1 << 3,
    COL_MODIFIED = 1 << 4,
    COL_CREATED = 1 << 5,
    COL_ACCESSED = 1 << 6,
    COL_ATTRIBUTES = 1 << 7,
    
    COL_DEFAULT = COL_NAME | COL_PATH | COL_SIZE | COL_MODIFIED
} ColumnFlags;

// Shows modal columns settings dialog
bool SettingsDialog_Show(HWND hwndParent, unsigned int* columnMask);

#endif // SETTINGS_DIALOG_H
