#pragma once
#include "stdafx.h"

namespace AppShell {

    // class ShellIntegration
    // Handles all native Windows shell bindings including right-click context menus
    // and standard Clipboard (Copy/Cut) interactions compatible with Windows Explorer.
    class ShellIntegration {
    public:
        // Spawns and executes native Explorer context menu on a selection of file paths
        static bool ShowContextMenu(HWND hwndParent, const std::vector<std::wstring>& filePaths, POINT ptScreen);

        // Copies files to the clipboard in CF_HDROP structure format
        // isCut: if true, sets the Preferred DropEffect to DROPEFFECT_MOVE (Cut action)
        static bool CopyFilesToClipboard(HWND hwndOwner, const std::vector<std::wstring>& filePaths, bool isCut);
    };

} // namespace AppShell
