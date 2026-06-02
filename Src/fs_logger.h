#ifndef FS_LOGGER_H
#define FS_LOGGER_H

#include "fs_common.h"

// Global logging flags
extern bool g_LoggingEnabled;
extern bool g_VerboseLogging;

// Logger lifecycle and settings
void Logger_Init(void);
void Logger_Shutdown(void);

void Logger_Log(const wchar_t* level, const wchar_t* format, ...);
void Logger_SetEnabled(bool enabled);
bool Logger_IsEnabled(void);
void Logger_SetVerbose(bool verbose);
bool Logger_IsVerbose(void);

// Returns the full absolute path of the FastSearch.log file
void Logger_GetLogFilePath(wchar_t* outPath, size_t maxChars);

// Logging Helper Macros
#define LOG_INFO(format, ...)   do { if (g_LoggingEnabled) Logger_Log(L"INFO", format, ##__VA_ARGS__); } while(0)
#define LOG_WARNING(format, ...) do { if (g_LoggingEnabled) Logger_Log(L"WARNING", format, ##__VA_ARGS__); } while(0)
#define LOG_ERROR(format, ...)   do { if (g_LoggingEnabled) Logger_Log(L"ERROR", format, ##__VA_ARGS__); } while(0)
#define LOG_VERBOSE(format, ...) do { if (g_LoggingEnabled && g_VerboseLogging) Logger_Log(L"VERBOSE", format, ##__VA_ARGS__); } while(0)

#endif // FS_LOGGER_H
