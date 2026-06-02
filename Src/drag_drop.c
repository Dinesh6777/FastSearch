#include "drag_drop.h"
#include "shell_integration.h"
#include <shlobj.h>

// Custom C COM struct for OLE IDropSource
typedef struct {
    IDropSourceVtbl* lpVtbl;
    LONG m_refCount;
} CDropSource;

// IDropSource methods implementation
static HRESULT STDMETHODCALLTYPE CDropSource_QueryInterface(IDropSource* This, REFIID riid, void** ppvObject) {
    CDropSource* self = (CDropSource*)This;
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDropSource)) {
        *ppvObject = This;
        This->lpVtbl->AddRef(This);
        return S_OK;
    }
    *ppvObject = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE CDropSource_AddRef(IDropSource* This) {
    CDropSource* self = (CDropSource*)This;
    return InterlockedIncrement(&self->m_refCount);
}

static ULONG STDMETHODCALLTYPE CDropSource_Release(IDropSource* This) {
    CDropSource* self = (CDropSource*)This;
    ULONG ref = InterlockedDecrement(&self->m_refCount);
    if (ref == 0) {
        free(self);
    }
    return ref;
}

static HRESULT STDMETHODCALLTYPE CDropSource_QueryContinueDrag(IDropSource* This, BOOL fEscapePressed, DWORD grfKeyState) {
    if (fEscapePressed) {
        return DRAGDROP_S_CANCEL;          // Cancel drag if Escape is pressed
    }
    if (!(grfKeyState & (MK_LBUTTON | MK_RBUTTON))) {
        return DRAGDROP_S_DROP;            // Commits drag when buttons are released
    }
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE CDropSource_GiveFeedback(IDropSource* This, DWORD dwEffect) {
    (void)This;
    (void)dwEffect;
    return DRAGDROP_S_USEDEFAULTCURSORS;   // Standard cursors (copy/move/link)
}

static CDropSource* CDropSource_Create(void) {
    static IDropSourceVtbl vtbl = {
        CDropSource_QueryInterface,
        CDropSource_AddRef,
        CDropSource_Release,
        CDropSource_QueryContinueDrag,
        CDropSource_GiveFeedback
    };
    CDropSource* ds = (CDropSource*)malloc(sizeof(CDropSource));
    if (ds) {
        ds->lpVtbl = &vtbl;
        ds->m_refCount = 1;
    }
    return ds;
}

// Split a path into directory and file name
static void SplitPath(const wchar_t* fullPath, wchar_t* directory, size_t dirMax, wchar_t* fileName, size_t fileMax) {
    const wchar_t* lastSlash = wcsrchr(fullPath, L'\\');
    const wchar_t* lastSlashAlt = wcsrchr(fullPath, L'/');
    if (lastSlashAlt > lastSlash) lastSlash = lastSlashAlt;
    
    if (lastSlash == NULL) {
        directory[0] = L'\0';
        wcscpy_s(fileName, fileMax, fullPath);
    } else {
        size_t dirLen = lastSlash - fullPath;
        if (dirLen >= dirMax) dirLen = dirMax - 1;
        wcsncpy_s(directory, dirMax, fullPath, dirLen);
        directory[dirLen] = L'\0';
        
        wcscpy_s(fileName, fileMax, lastSlash + 1);
        
        if (dirLen == 2 && directory[1] == L':') {
            wcscat_s(directory, dirMax, L"\\"); // Map C: to C:\ for valid Shell parsing
        }
    }
}

bool DragDrop_CopyFilesToClipboardOrDrag(HWND hwnd, const wchar_t** filePaths, size_t filePathsCount, bool drag) {
    if (filePathsCount == 0 || !filePaths) return false;

    // Fall back to standard Clipboard copying if drag is false
    if (!drag) {
        return ShellIntegration_CopyFilesToClipboard(hwnd, filePaths, filePathsCount, false);
    }

    wchar_t parentDir[MAX_PATH];
    wchar_t fileName0[MAX_PATH];
    SplitPath(filePaths[0], parentDir, MAX_PATH, fileName0, MAX_PATH);

    // Group relative paths in the same parent folder context
    PIDLIST_ABSOLUTE parentPidl = NULL;
    HRESULT hr = SHParseDisplayName(parentDir, NULL, &parentPidl, 0, NULL);
    if (FAILED(hr)) return false;

    IShellFolder* desktopFolder = NULL;
    hr = SHGetDesktopFolder(&desktopFolder);
    if (FAILED(hr)) {
        CoTaskMemFree(parentPidl);
        return false;
    }

    IShellFolder* parentFolder = NULL;
    hr = desktopFolder->lpVtbl->BindToObject(desktopFolder, parentPidl, NULL, &IID_IShellFolder, (void**)&parentFolder);
    desktopFolder->lpVtbl->Release(desktopFolder);
    CoTaskMemFree(parentPidl);

    if (FAILED(hr)) return false;

    // Convert file names to child PIDLs in parent context
    PITEMID_CHILD* relativePidls = (PITEMID_CHILD*)malloc(filePathsCount * sizeof(PITEMID_CHILD));
    size_t relativePidlsCount = 0;

    if (relativePidls) {
        for (size_t i = 0; i < filePathsCount; ++i) {
            wchar_t dir[MAX_PATH];
            wchar_t file[MAX_PATH];
            SplitPath(filePaths[i], dir, MAX_PATH, file, MAX_PATH);

            PITEMID_CHILD childPidl = NULL;
            hr = parentFolder->lpVtbl->ParseDisplayName(parentFolder, hwnd, NULL, file, NULL, &childPidl, NULL);
            if (SUCCEEDED(hr) && childPidl) {
                relativePidls[relativePidlsCount++] = childPidl;
            }
        }
    }

    if (relativePidlsCount == 0) {
        if (relativePidls) free(relativePidls);
        parentFolder->lpVtbl->Release(parentFolder);
        return false;
    }

    // Retrieve standard native IDataObject interface from parent shell folder
    IDataObject* pDataObject = NULL;
    hr = parentFolder->lpVtbl->GetUIObjectOf(
        parentFolder,
        hwnd,
        (UINT)relativePidlsCount,
        (LPCITEMIDLIST*)relativePidls,
        &IID_IDataObject,
        NULL,
        (void**)&pDataObject
    );

    // Free child PIDLs
    for (size_t i = 0; i < relativePidlsCount; i++) {
        CoTaskMemFree(relativePidls[i]);
    }
    free(relativePidls);
    parentFolder->lpVtbl->Release(parentFolder);

    if (FAILED(hr)) return false;

    // Instantiate our C COM DropSource
    CDropSource* pDropSource = CDropSource_Create();
    if (!pDropSource) {
        pDataObject->lpVtbl->Release(pDataObject);
        return false;
    }

    // Trigger standard Win32 DoDragDrop
    DWORD effect = 0;
    hr = DoDragDrop(pDataObject, (IDropSource*)pDropSource, DROPEFFECT_COPY | DROPEFFECT_MOVE, &effect);

    pDataObject->lpVtbl->Release(pDataObject);
    pDropSource->lpVtbl->Release((IDropSource*)pDropSource);

    return SUCCEEDED(hr);
}
