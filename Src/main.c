#include "fs_common.h"
#include "search_engine.h"
#include "main_frame.h"
#include "search_view.h"
#include "fs_logger.h"
#include "resource.h"
#include <commctrl.h>

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nShowCmd) {
    (void)hPrevInstance;
    (void)lpCmdLine;

    // Initialize global logger
    Logger_Init();

    // 1. Initialize modern graphical visual styles (Common Controls v6)
    INITCOMMONCONTROLSEX icex;
    icex.dwSize = sizeof(INITCOMMONCONTROLSEX);
    icex.dwICC = ICC_TAB_CLASSES | ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icex);

    // 2. Initialize OLE subsystem (mandatory for Drag & Drop clipboard operations!)
    HRESULT hr = OleInitialize(NULL);
    if (FAILED(hr)) {
        MessageBoxW(NULL, L"Failed to initialize COM OLE.", L"Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    // 3. Register custom window classes
    if (!MainFrame_RegisterClass()) {
        MessageBoxW(NULL, L"Failed to register MainFrame class.", L"Error", MB_OK | MB_ICONERROR);
        OleUninitialize();
        return 1;
    }

    if (!SearchView_RegisterClass()) {
        MessageBoxW(NULL, L"Failed to register SearchView class.", L"Error", MB_OK | MB_ICONERROR);
        OleUninitialize();
        return 1;
    }

    // 4. Instantiate the SearchEngine database
    SearchEngine* engine = SearchEngine_Create();
    if (!engine) {
        MessageBoxW(NULL, L"Failed to create SearchEngine.", L"Error", MB_OK | MB_ICONERROR);
        OleUninitialize();
        return 1;
    }

    // Parse command line for optional -path parameter
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    wchar_t initialPath[MAX_PATH] = { 0 };
    if (argv) {
        for (int i = 1; i < argc - 1; ++i) {
            if (_wcsicmp(argv[i], L"-path") == 0) {
                wcscpy_s(initialPath, MAX_PATH, argv[i + 1]);
                break;
            }
        }
        LocalFree(argv);
    }

    // 5. Create Main Frame
    HWND hwndFrame = MainFrame_Create(engine, initialPath);
    if (!hwndFrame) {
        MessageBoxW(NULL, L"Failed to create MainFrame Window.", L"Error", MB_OK | MB_ICONERROR);
        SearchEngine_Destroy(engine);
        OleUninitialize();
        return 1;
    }

    ShowWindow(hwndFrame, nShowCmd);
    UpdateWindow(hwndFrame);

    // 6. Main Message Loop with Accelerator support
    HACCEL hAccel = LoadAcceleratorsW(hInstance, MAKEINTRESOURCEW(IDR_MAINFRAME));
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        if (!hAccel || !TranslateAcceleratorW(hwndFrame, hAccel, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    // 7. Cleanup
    SearchEngine_Destroy(engine);
    OleUninitialize();
    Logger_Shutdown();

    return (int)msg.wParam;
}
