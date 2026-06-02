#include "fs_logger.h"
#include <shellapi.h>
#include <stdio.h>

bool g_LoggingEnabled = false;
bool g_VerboseLogging = false;

static CRITICAL_SECTION g_LogMutex;
static bool g_LogMutexInitialized = false;

static void LoadLoggingSettings(bool* pEnabled, bool* pVerbose) {
    HKEY hKey;
    *pEnabled = false;
    *pVerbose = false;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\FastSearch", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        DWORD dwType = REG_DWORD;
        DWORD dwSize = sizeof(DWORD);
        DWORD val = 0;
        if (RegQueryValueExW(hKey, L"EnableLogging", NULL, &dwType, (LPBYTE)&val, &dwSize) == ERROR_SUCCESS) {
            *pEnabled = (val != 0);
        }
        val = 0;
        dwSize = sizeof(DWORD);
        if (RegQueryValueExW(hKey, L"VerboseLogging", NULL, &dwType, (LPBYTE)&val, &dwSize) == ERROR_SUCCESS) {
            *pVerbose = (val != 0);
        }
        RegCloseKey(hKey);
    }
}

static void SaveLoggingSettings(bool enabled, bool verbose) {
    HKEY hKey;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\FastSearch", 0, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
        DWORD val = enabled ? 1 : 0;
        RegSetValueExW(hKey, L"EnableLogging", 0, REG_DWORD, (const BYTE*)&val, sizeof(DWORD));
        val = verbose ? 1 : 0;
        RegSetValueExW(hKey, L"VerboseLogging", 0, REG_DWORD, (const BYTE*)&val, sizeof(DWORD));
        RegCloseKey(hKey);
    }
}

void Logger_GetLogFilePath(wchar_t* outPath, size_t maxChars) {
    GetModuleFileNameW(NULL, outPath, (DWORD)maxChars);
    wchar_t* lastSlash = wcsrchr(outPath, L'\\');
    if (lastSlash) {
        *(lastSlash + 1) = L'\0';
    }
    wcscat_s(outPath, maxChars, L"FastSearch.log");
}

void Logger_Init(void) {
    if (!g_LogMutexInitialized) {
        InitializeCriticalSection(&g_LogMutex);
        g_LogMutexInitialized = true;
    }

    // 1. Load persisted preference from Registry
    bool regEnabled = false;
    bool regVerbose = false;
    LoadLoggingSettings(&regEnabled, &regVerbose);

    g_LoggingEnabled = regEnabled;
    g_VerboseLogging = regVerbose;

    // 2. Parse command-line parameters to override Registry settings
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv) {
        for (int i = 1; i < argc; ++i) {
            if (_wcsicmp(argv[i], L"-debug-log") == 0) {
                g_LoggingEnabled = true;
            }
            else if (_wcsicmp(argv[i], L"-verbose") == 0) {
                g_VerboseLogging = true;
            }
        }
        LocalFree(argv);
    }

    if (g_LoggingEnabled) {
        wchar_t logPath[MAX_PATH];
        Logger_GetLogFilePath(logPath, MAX_PATH);

        // Open log file in write/create mode to truncate older logs upon application launch
        FILE* file = NULL;
        if (_wfopen_s(&file, logPath, L"w, ccs=UTF-8") == 0 && file) {
            fclose(file);
        }

        LOG_INFO(L"=== FastSearch Logging Initialized (Verbose=%d) ===", g_VerboseLogging ? 1 : 0);
    }
}

void Logger_Shutdown(void) {
    if (g_LoggingEnabled) {
        LOG_INFO(L"=== FastSearch Logging Shutdown ===");
    }
    if (g_LogMutexInitialized) {
        DeleteCriticalSection(&g_LogMutex);
        g_LogMutexInitialized = false;
    }
}

void Logger_Log(const wchar_t* level, const wchar_t* format, ...) {
    if (!g_LoggingEnabled || !g_LogMutexInitialized) return;

    EnterCriticalSection(&g_LogMutex);

    wchar_t logPath[MAX_PATH];
    Logger_GetLogFilePath(logPath, MAX_PATH);

    FILE* file = NULL;
    // Open file in append mode with UTF-8 encoding
    if (_wfopen_s(&file, logPath, L"ab, ccs=UTF-8") == 0 && file) {
        SYSTEMTIME st;
        GetLocalTime(&st);

        // Print timestamp, thread ID, log level
        fwprintf(file, L"%04d-%02d-%02d %02d:%02d:%02d.%03d [%-7s] [TID:%05d] ", 
                 st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                 level, GetCurrentThreadId());

        va_list args;
        va_start(args, format);
        vfwprintf(file, format, args);
        va_end(args);

        fwprintf(file, L"\n");
        fclose(file);
    }

    LeaveCriticalSection(&g_LogMutex);
}

void Logger_SetEnabled(bool enabled) {
    if (g_LoggingEnabled != enabled) {
        g_LoggingEnabled = enabled;
        SaveLoggingSettings(g_LoggingEnabled, g_VerboseLogging);
        if (g_LoggingEnabled) {
            Logger_Init();
        } else {
            Logger_Shutdown();
        }
    }
}

bool Logger_IsEnabled(void) {
    return g_LoggingEnabled;
}

void Logger_SetVerbose(bool verbose) {
    if (g_VerboseLogging != verbose) {
        g_VerboseLogging = verbose;
        SaveLoggingSettings(g_LoggingEnabled, g_VerboseLogging);
        LOG_INFO(L"Logging verbosity changed: Verbose=%d", g_VerboseLogging ? 1 : 0);
    }
}

bool Logger_IsVerbose(void) {
    return g_VerboseLogging;
}
