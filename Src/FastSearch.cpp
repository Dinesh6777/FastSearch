#include "stdafx.h"
#include "resource.h"
#include "MainFrame.h"

// Define the global WTL application module instance required by ATL/WTL internally
CAppModule _Module;

// The main Win32 application entry point
int WINAPI _tWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPTSTR lpCmdLine, int nShowCmd) {
    
    // 0. Register explicit Application User Model ID (AppID) for taskbar grouping and shift-clicking
    ::SetCurrentProcessExplicitAppUserModelID(L"DeepMind.Antigravity.FastSearch");

    // 1. Single Instance Enforcement (Disabled to allow multiple simultaneous instances)
    /*
    HANDLE hMutex = CreateMutexW(NULL, TRUE, L"Local\\FastSearch_InstanceMutex");
    if (hMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        // Find the main frame window of the existing running instance
        HWND hWndExisting = FindWindowW(L"FastSearchMainFrame", NULL);
        if (hWndExisting) {
            // Restore window if minimized, and transfer user input focus to it
            ShowWindow(hWndExisting, SW_RESTORE);
            SetForegroundWindow(hWndExisting);
        }
        
        CloseHandle(hMutex);
        return 0; // Terminate this secondary instance immediately
    }
    */
    HANDLE hMutex = NULL;

    // 2. Initialize OLE/COM Libraries.
    // OleInitialize is mandatory (instead of CoInitializeEx) because we utilize OLE drag and drop
    // capabilities (DoDragDrop/IDropSource) inside the list control.
    HRESULT hr = OleInitialize(NULL);
    if (FAILED(hr)) {
        CloseHandle(hMutex);
        return 0;
    }

    // 3. Initialize WTL application module
    hr = _Module.Init(NULL, hInstance);
    if (FAILED(hr)) {
        OleUninitialize();
        CloseHandle(hMutex);
        return 0;
    }

    // 4. Initialize Windows Common Controls
    // Registers class structures for ListView and Tab controls, loading modern visual designs.
    INITCOMMONCONTROLSEX iccx = { 0 };
    iccx.dwSize = sizeof(iccx);
    iccx.dwICC = ICC_TAB_CLASSES | ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&iccx);

    // 5. Spin the standard message loop
    CMessageLoop theLoop;
    _Module.AddMessageLoop(&theLoop);

    // Create the Main Shell Window Frame
    CMainFrame wndMain;
    if (wndMain.CreateEx() == NULL) {
        _Module.RemoveMessageLoop();
        _Module.Term();
        OleUninitialize();
        CloseHandle(hMutex);
        return 0;
    }

    // Set the application title programmatically beside the icon
    wndMain.SetWindowText(_T("FastSearch"));

    // Show and update frame
    wndMain.ShowWindow(nShowCmd);
    wndMain.UpdateWindow();

    // Block thread inside event loops until WM_QUIT is pushed
    int nRet = theLoop.Run();

    // 6. Shutdown cleanup
    _Module.RemoveMessageLoop();
    wndMain.DestroyWindow();
    _Module.Term();
    
    // Shutdown OLE/COM
    OleUninitialize();

    CloseHandle(hMutex);
    return nRet;
}
