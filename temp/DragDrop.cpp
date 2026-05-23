#include "stdafx.h"
#include "DragDrop.h"
#include "ShellIntegration.h"
#include <shlobj.h>

namespace AppShell {

    // A lightweight implementation of the standard OLE IDropSource interface.
    // CDropSource manages feedback cursors and keyboard/mouse modifiers during a drag operation.
    class CDropSource : public IDropSource {
    public:
        // IUnknown Methods
        STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
            if (riid == IID_IUnknown || riid == IID_IDropSource) {
                *ppv = static_cast<IDropSource*>(this);
                AddRef();
                return S_OK;
            }
            *ppv = nullptr;
            return E_NOINTERFACE;
        }

        STDMETHODIMP_(ULONG) AddRef() override {
            return InterlockedIncrement(&m_refCount);
        }

        STDMETHODIMP_(ULONG) Release() override {
            ULONG ref = InterlockedDecrement(&m_refCount);
            if (ref == 0) {
                delete this;
            }
            return ref;
        }

        // IDropSource Methods
        // Intercepts escape keys or button releases to cancel or commit drag operations.
        STDMETHODIMP QueryContinueDrag(BOOL fEscapePressed, DWORD grfKeyState) override {
            if (fEscapePressed) {
                return DRAGDROP_S_CANCEL; // Escape pressed -> cancel drag
            }
            
            // Standard left/right click buttons released -> drop files
            if (!(grfKeyState & (MK_LBUTTON | MK_RBUTTON))) {
                return DRAGDROP_S_DROP;
            }
            
            return S_OK; // continue drag
        }

        STDMETHODIMP GiveFeedback(DWORD dwEffect) override {
            return DRAGDROP_S_USEDEFAULTCURSORS; // use standard system copy/move/link cursors
        }

        CDropSource() : m_refCount(1) {}

    private:
        LONG m_refCount;
    };

    // Helper to split a path into directory and file name components
    static void SplitPath(const std::wstring& fullPath, std::wstring& directory, std::wstring& fileName) {
        size_t lastSlash = fullPath.find_last_of(L"\\/");
        if (lastSlash == std::wstring::npos) {
            directory = L"";
            fileName = fullPath;
        } else {
            directory = fullPath.substr(0, lastSlash);
            fileName = fullPath.substr(lastSlash + 1);
            if (directory.length() == 2 && directory[1] == L':') {
                directory += L"\\"; // Map C: to C:\ for valid Shell parsing
            }
        }
    }

    // Leverages native Windows Shell elements to retrieve a fully optimized IDataObject 
    // and invokes the Win32 OLE Drag & Drop subsystem.
    bool DragDrop::CopyFilesToClipboardOrDrag(HWND hwnd, const std::vector<std::wstring>& filePaths, bool drag) {
        if (filePaths.empty()) return false;

        // If 'drag' is false, fall back to standard Clipboard copying
        if (!drag) {
            return ShellIntegration::CopyFilesToClipboard(hwnd, filePaths, false);
        }

        // Group files by parent directory. Shell PIDL binders require a unified parent IShellFolder.
        std::wstring parentDir, fileName0;
        SplitPath(filePaths[0], parentDir, fileName0);

        // Group relative paths (we use the first folder's context for simplicity in multi-folder drags)
        PIDLIST_ABSOLUTE parentPidl = nullptr;
        HRESULT hr = SHParseDisplayName(parentDir.c_str(), NULL, &parentPidl, 0, NULL);
        if (FAILED(hr)) return false;

        IShellFolder* desktopFolder = nullptr;
        hr = SHGetDesktopFolder(&desktopFolder);
        if (FAILED(hr)) {
            CoTaskMemFree(parentPidl);
            return false;
        }

        IShellFolder* parentFolder = nullptr;
        hr = desktopFolder->BindToObject(parentPidl, NULL, IID_IShellFolder, (void**)&parentFolder);
        desktopFolder->Release();
        CoTaskMemFree(parentPidl);

        if (FAILED(hr)) return false;

        // Convert file names to relative PIDLs
        std::vector<PITEMID_CHILD> relativePidls;
        std::wstring file;
        for (const auto& path : filePaths) {
            std::wstring dir;
            SplitPath(path, dir, file);

            PITEMID_CHILD childPidl = nullptr;
            hr = parentFolder->ParseDisplayName(hwnd, NULL, const_cast<wchar_t*>(file.c_str()), NULL, &childPidl, NULL);
            if (SUCCEEDED(hr)) {
                relativePidls.push_back(childPidl);
            }
        }

        if (relativePidls.empty()) {
            parentFolder->Release();
            return false;
        }

        // Get standard IDataObject interface from parent shell folder.
        // Doing this provides a fully functional native IDataObject containing 
        // formats like CF_HDROP, file sizes, extensions, and Explorer previews!
        IDataObject* pDataObject = nullptr;
        hr = parentFolder->GetUIObjectOf(
            hwnd,
            static_cast<UINT>(relativePidls.size()),
            (LPCITEMIDLIST*)relativePidls.data(),
            IID_IDataObject,
            NULL,
            (void**)&pDataObject
        );

        // Free relative PIDLs
        for (auto p : relativePidls) {
            CoTaskMemFree(p);
        }
        parentFolder->Release();

        if (FAILED(hr)) return false;

        // Allocate and reference our custom DropSource
        CDropSource* pDropSource = new CDropSource();

        // 3. Initiate the standard OLE Drag & Drop operation
        DWORD effect = 0;
        hr = DoDragDrop(pDataObject, pDropSource, DROPEFFECT_COPY | DROPEFFECT_MOVE, &effect);

        // Cleanup COM interfaces
        pDataObject->Release();
        pDropSource->Release();

        return SUCCEEDED(hr);
    }

} // namespace AppShell
