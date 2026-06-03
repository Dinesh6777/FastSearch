#ifndef SEARCH_VIEW_H
#define SEARCH_VIEW_H

#include "fs_common.h"
#include "search_engine.h"

// Struct wrapping search view properties (stored in GWLP_USERDATA of search view window)
typedef struct {
    wchar_t text[128];
    int width;
    int subitemIndex;
} ViewColumnInfo;

typedef struct {
    HWND hWnd;
    SearchEngine* searchEngine;
    HWND volumeCombo;
    HWND searchEdit;
    HWND filterCombo;
    HWND caseCheck;
    HWND regexCheck;
    HWND listView;

    unsigned int columnMask;
    ViewColumnInfo visibleColumns[16];
    int visibleColumnsCount;

    int sortColumn;
    bool sortAscending;
    int viewMode;
    double lastSearchTimeMs;

    SearchResultList results;
    
    WNDPROC oldEditWndProc;                // Subclass backup for Search Edit box
    WNDPROC oldListWndProc;                // Subclass backup for ListView control

    HANDLE hSearchThread;                  // Background search thread
    HANDLE hSearchEvent;                   // Signaled when a new search is pending
    CRITICAL_SECTION searchInputMutex;     // Mutex protecting inputs passed to background thread
    wchar_t* pendingQuery;                 // The query string to search for
    MatchMode pendingMode;
    wchar_t pendingDrive;
    FilterType pendingFilter;
    volatile LONG cancelSearch;            // Signaled to cancel active search
    volatile LONG isSearching;             // Flag indicating search is running
    bool isTerminating;                    // Flag to clean up thread
    StringMatcher highlightMatcher;        // Matcher for visual highlight in custom draw
} SearchViewData;

// Registers search view custom window class
bool SearchView_RegisterClass(void);

// Creates a search view window instance
HWND SearchView_Create(HWND hwndParent, SearchEngine* engine);

void SearchView_UpdateColumns(HWND hwndView, unsigned int columnMask);
void SearchView_TriggerSearch(HWND hwndView);
void SearchView_SetSearchText(HWND hwndView, const wchar_t* text);

size_t SearchView_GetResultCount(HWND hwndView);
double SearchView_GetLastSearchTimeMs(HWND hwndView);

void SearchView_SetViewMode(HWND hwndView, int mode);
int SearchView_GetViewMode(HWND hwndView);

bool SearchView_ExportResults(HWND hwndView, const wchar_t* filePath);

#endif // SEARCH_VIEW_H
