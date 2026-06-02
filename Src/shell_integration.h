#ifndef SHELL_INTEGRATION_H
#define SHELL_INTEGRATION_H

#include "fs_common.h"

// Spawns and executes native Explorer context menu on a selection of file paths
bool ShellIntegration_ShowContextMenu(HWND hwndParent, const wchar_t** filePaths, size_t filePathsCount, POINT ptScreen);

// Copies files to the clipboard in CF_HDROP structure format
bool ShellIntegration_CopyFilesToClipboard(HWND hwndOwner, const wchar_t** filePaths, size_t filePathsCount, bool isCut);

#endif // SHELL_INTEGRATION_H
