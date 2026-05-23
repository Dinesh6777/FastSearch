#pragma once

// Target Windows 10 and above
#define WINVER 0x0A00
#define _WIN32_WINNT 0x0A00
#define _WIN32_IE 0x0A00

#define GDIPVER 0x0110

// Suppress security warnings for standard C library
#define _CRT_SECURE_NO_WARNINGS
#define _SCL_SECURE_NO_WARNINGS

// Ensure STRICT type checking for Windows handles
#define STRICT
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

// ATL/WTL defines
#define _WTL_NO_UNION_CLASSES
#define _WTL_NO_CSTRING
#define _WTL_NO_WTYPES

#include <atlbase.h>
#include <atlapp.h>

// Declare the WTL global application module
extern CAppModule _Module;

#include <atlwin.h>
#include <atlframe.h>
#include <atlctrls.h>
#include <atldlgs.h>
#include <atlctrlx.h>
#include <atlmisc.h>
#include <atltheme.h>

// Shell and native Windows headers
#include <windows.h>
#include <winioctl.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <uxtheme.h>
#include <vssym32.h>
#include <dbt.h>

// Standard C++ Library headers
#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <set>
#include <queue>
#include <algorithm>
#include <memory>
#include <regex>
#include <thread>
#include <mutex>
#include <shared_mutex>
#include <atomic>
#include <condition_variable>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <iostream>

// Custom Application Messages
#define WM_SEARCH_RESULTS_CHANGED (WM_USER + 101)
#define WM_NTFS_INDEX_CHANGED (WM_USER + 102)

// Link libraries directly via pragmas for cleanliness
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "shell32.lib")

// Embed Common Controls v6 manifest dependency directly from C++ compiler
#pragma comment(linker,"\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
