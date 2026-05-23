#pragma once
#include "stdafx.h"

namespace AppShell {

    // class DragDrop
    // Implements custom OLE Drag & Drop source interfaces (IDropSource) 
    // to enable dragging files directly out of FastSearch into Windows Explorer.
    class DragDrop {
    public:
        // Initiates a OLE Drag & Drop operation or copies to clipboard
        // hwnd: owner window handle
        // filePaths: list of files to transfer
        // drag: if true, initiates OLE DoDragDrop. If false, copies to clipboard.
        static bool CopyFilesToClipboardOrDrag(HWND hwnd, const std::vector<std::wstring>& filePaths, bool drag);
    };

} // namespace AppShell
