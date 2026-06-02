#ifndef MAIN_FRAME_H
#define MAIN_FRAME_H

#include "fs_common.h"
#include "search_engine.h"

typedef struct {
    wchar_t Name[64];
    HWND View;
} FrameTabContext;

typedef struct {
    HWND hWnd;
    SearchEngine* searchEngine;
    HWND statusBar;
    HWND tabCtrl;

    FrameTabContext tabs[32];
    int tabsCount;
    int activeTabIndex;

    unsigned int columnMask;
    HDEVNOTIFY hDevNotify;
} MainFrameData;

// Registers frame custom window class
bool MainFrame_RegisterClass(void);

// Creates main frame window instance
HWND MainFrame_Create(SearchEngine* engine);

#endif // MAIN_FRAME_H
