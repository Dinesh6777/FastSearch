#ifndef DRAG_DROP_H
#define DRAG_DROP_H

#include "fs_common.h"

// Initiates OLE DoDragDrop or clipboard copies for files list
// hwnd: owner window handle
// filePaths: array of wide file paths
// filePathsCount: number of files
// drag: true to start OLE drag-drop, false to copy to clipboard
bool DragDrop_CopyFilesToClipboardOrDrag(HWND hwnd, const wchar_t** filePaths, size_t filePathsCount, bool drag);

#endif // DRAG_DROP_H
