#include "file_menu.hpp"

#include <shellapi.h>

#include <algorithm>
#include <array>
#include <limits>
#include <utility>

namespace nf {
namespace {

template<class T>
struct ReleaseInterface {
    void operator()(T* object) const { object->Release(); }
};

template<class T>
using InterfacePtr = std::unique_ptr<T, ReleaseInterface<T>>;

struct FreePidl {
    void operator()(void* item) const { CoTaskMemFree(item); }
};

template<class T>
InterfacePtr<T> getShellObject(HWND owner, const Directory& directory, std::span<const Index> files) {
    if (files.empty()) return {};

    PIDLIST_ABSOLUTE folderId = nullptr;
    if (FAILED(SHParseDisplayName(directory.path.c_str(), nullptr, &folderId, 0, nullptr))) return {};
    const std::unique_ptr<ITEMIDLIST, FreePidl> folderIdOwner(folderId);
    IShellFolder* folder = nullptr;
    if (FAILED(SHBindToObject(nullptr, folderId, nullptr, IID_PPV_ARGS(&folder)))) return {};
    const InterfacePtr<IShellFolder> folderOwner(folder);

    std::vector<std::unique_ptr<ITEMID_CHILD, FreePidl>> itemOwners;
    std::vector<PCUITEMID_CHILD> items;
    itemOwners.reserve(files.size());
    items.reserve(files.size());
    for (const Index index : files) {
        std::wstring name(directory.name(index));
        PIDLIST_RELATIVE item = nullptr;
        if (FAILED(folder->ParseDisplayName(owner, nullptr, name.data(), nullptr, &item, nullptr))) return {};
        const auto child = ILFindLastID(item);
        itemOwners.emplace_back(item);
        items.push_back(child);
    }

    T* object = nullptr;
    if (FAILED(folder->GetUIObjectOf(owner, static_cast<UINT>(items.size()), items.data(),
                                    __uuidof(T), nullptr, reinterpret_cast<void**>(&object)))) return {};
    return InterfacePtr<T>(object);
}

std::array<wchar_t, 64> shellVerb(IContextMenu& menu, UINT offset) {
    std::array<wchar_t, 64> verb{};
    // Some Shell handlers only expose the ANSI form of the canonical command name.
    const auto verbResult = menu.GetCommandString(offset, GCS_VERBW, nullptr,
        reinterpret_cast<char*>(verb.data()), static_cast<UINT>(std::size(verb)));
    if (FAILED(verbResult) || !verb[0]) {
        verb[0] = 0;
        char legacy[64]{};
        if (SUCCEEDED(menu.GetCommandString(offset, GCS_VERBA, nullptr, legacy, static_cast<UINT>(std::size(legacy))))) {
            legacy[std::size(legacy) - 1] = 0;
            MultiByteToWideChar(CP_ACP, 0, legacy, -1, verb.data(), static_cast<int>(std::size(verb)));
        }
    }
    verb.back() = 0;
    return verb;
}

struct ClipboardMemory {
    HGLOBAL handle;
    explicit ClipboardMemory(std::size_t bytes) : handle(GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes)) {}
    ~ClipboardMemory() { if (handle) GlobalFree(handle); }
    ClipboardMemory(const ClipboardMemory&) = delete;
    ClipboardMemory& operator=(const ClipboardMemory&) = delete;
};

struct ClipboardLock {
    explicit ClipboardLock(HWND owner) : opened(OpenClipboard(owner) != FALSE) {}
    ~ClipboardLock() { if (opened) CloseClipboard(); }
    bool opened;
};

bool publishClipboard(UINT format, ClipboardMemory& block) {
    if (!SetClipboardData(format, block.handle)) return false;
    block.handle = nullptr;
    return true;
}

} // namespace

FileMenu::FileMenu(HWND owner, const Directory& directory, std::optional<Index> single, Language language) {
    // A group menu is ready before any individual paths are sent to the Shell.
    auto menu = single ? getShellObject<IContextMenu>(owner, directory, std::span(&*single, 1))
                       : InterfacePtr<IContextMenu>{};
    popup = CreatePopupMenu();
    if (!popup) return;

    bool renameAdded = false;
    const auto renameLabel = translated(language, L"Rename…", L"Переименовать…");
    if (menu && SUCCEEDED(menu->QueryContextMenu(popup, 0, FirstShellCommand, 0x7fff, CMF_NORMAL)) &&
        GetMenuItemCount(popup) > 0) {
        shellMenu = menu.release();
        if (FAILED(shellMenu->QueryInterface(IID_PPV_ARGS(&shellMenu3)))) {
            shellMenu->QueryInterface(IID_PPV_ARGS(&shellMenu2));
        }
        for (int position = 0; position < GetMenuItemCount(popup); ++position) {
            const UINT command = GetMenuItemID(popup, position);
            if (command < FirstShellCommand || command > 0x7fff) continue;
            const auto verb = shellVerb(*shellMenu, command - FirstShellCommand);
            if (compareNames(verb.data(), L"rename") != 0) continue;
            MENUITEMINFOW item{};
            item.cbSize = sizeof(item);
            item.fMask = MIIM_ID | MIIM_STRING | MIIM_FTYPE | MIIM_STATE;
            item.wID = RenameFile;
            item.dwTypeData = const_cast<wchar_t*>(renameLabel);
            item.fType = MFT_STRING;
            item.fState = MFS_ENABLED;
            renameAdded = SetMenuItemInfoW(popup, static_cast<UINT>(position), TRUE, &item) != FALSE;
            break;
        }
        AppendMenuW(popup, MF_SEPARATOR, 0, nullptr);
    } else {
        // A handler can return an error after inserting partial menu items.
        for (int item = GetMenuItemCount(popup) - 1; item >= 0; --item) {
            DeleteMenu(popup, static_cast<UINT>(item), MF_BYPOSITION);
        }
        AppendMenuW(popup, MF_STRING, CutFiles, translated(language, L"Cut", L"Вырезать"));
        AppendMenuW(popup, MF_STRING, CopyFiles, translated(language, L"Copy", L"Копировать"));
        AppendMenuW(popup, MF_STRING, DeleteFiles, translated(language, L"Delete", L"Удалить"));
        AppendMenuW(popup, MF_SEPARATOR, 0, nullptr);
    }
    if (single && !renameAdded) AppendMenuW(popup, MF_STRING, RenameFile, renameLabel);
    AppendMenuW(popup, MF_STRING, BatchRenameFiles, translated(language, L"Batch rename…", L"Пакетное переименование…"));
    if (!shellMenu) {
        AppendMenuW(popup, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(popup, MF_STRING, FileProperties, translated(language, L"Properties", L"Свойства"));
    }
}

FileMenu::~FileMenu() {
    if (popup) DestroyMenu(popup);
    if (shellMenu3) shellMenu3->Release();
    if (shellMenu2) shellMenu2->Release();
    if (shellMenu) shellMenu->Release();
}

UINT FileMenu::track(HWND owner, POINT point) const {
    if (!popup) return 0;
    return static_cast<UINT>(TrackPopupMenuEx(popup, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                              point.x, point.y, owner, nullptr));
}

std::optional<LRESULT> FileMenu::handleMessage(UINT message, WPARAM wParam, LPARAM lParam) const {
    if (message != WM_INITMENUPOPUP && message != WM_DRAWITEM && message != WM_MEASUREITEM && message != WM_MENUCHAR) {
        return std::nullopt;
    }
    if ((message == WM_DRAWITEM || message == WM_MEASUREITEM) && wParam) return std::nullopt;
    LRESULT result = 0;
    if (shellMenu3 && shellMenu3->HandleMenuMsg2(message, wParam, lParam, &result) == S_OK) return result;
    if (shellMenu2 && message != WM_MENUCHAR && shellMenu2->HandleMenuMsg(message, wParam, lParam) == S_OK) return 0;
    return std::nullopt;
}

HRESULT FileMenu::invoke(HWND owner, UINT command, POINT point, bool& refresh) const {
    if (!shellMenu || command < FirstShellCommand) return E_INVALIDARG;
    const UINT offset = command - FirstShellCommand;
    const auto verb = shellVerb(*shellMenu, offset);

    CMINVOKECOMMANDINFOEX info{};
    info.cbSize = sizeof(info);
    info.fMask = CMIC_MASK_UNICODE | CMIC_MASK_NOASYNC | CMIC_MASK_PTINVOKE;
    info.hwnd = owner;
    info.lpVerb = MAKEINTRESOURCEA(offset);
    info.lpVerbW = MAKEINTRESOURCEW(offset);
    info.nShow = SW_SHOWNORMAL;
    info.ptInvoke = point;
    const auto result = shellMenu->InvokeCommand(reinterpret_cast<LPCMINVOKECOMMANDINFO>(&info));
    refresh = SUCCEEDED(result) && compareNames(verb.data(), L"delete") == 0;
    return result;
}

DWORD putFilesOnClipboard(HWND owner, const Directory& directory, std::span<const Index> files, bool cut) {
    if (files.empty()) return ERROR_INVALID_PARAMETER;
    const auto prefix = childPath(directory.path, {});
    constexpr auto maxCharacters = (std::numeric_limits<std::size_t>::max() - sizeof(DROPFILES)) / sizeof(wchar_t);
    std::size_t characters = 1;
    for (const Index index : files) {
        const auto length = prefix.size() + directory.name(index).size() + 1;
        if (length > maxCharacters - characters) return ERROR_NOT_ENOUGH_MEMORY;
        characters += length;
    }
    ClipboardMemory drop(sizeof(DROPFILES) + characters * sizeof(wchar_t));
    ClipboardMemory effect(sizeof(DWORD));
    if (!drop.handle || !effect.handle) return ERROR_NOT_ENOUGH_MEMORY;

    auto* header = static_cast<DROPFILES*>(GlobalLock(drop.handle));
    if (!header) return GetLastError();
    header->pFiles = sizeof(DROPFILES);
    header->fWide = TRUE;
    auto* next = reinterpret_cast<wchar_t*>(header + 1);
    for (const Index index : files) {
        next = std::copy(prefix.begin(), prefix.end(), next);
        const auto name = directory.name(index);
        next = std::copy(name.begin(), name.end(), next);
        *next++ = L'\0';
    }
    *next = L'\0';
    GlobalUnlock(drop.handle);

    auto* preferred = static_cast<DWORD*>(GlobalLock(effect.handle));
    if (!preferred) return GetLastError();
    *preferred = cut ? DROPEFFECT_MOVE : DROPEFFECT_COPY;
    GlobalUnlock(effect.handle);

    const UINT effectFormat = RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT);
    if (!effectFormat) return GetLastError();
    ClipboardLock clipboard(owner);
    if (!clipboard.opened || !EmptyClipboard()) return GetLastError();
    if (!publishClipboard(effectFormat, effect)) return GetLastError();
    if (!publishClipboard(CF_HDROP, drop)) {
        const DWORD error = GetLastError();
        EmptyClipboard();
        return error;
    }
    return ERROR_SUCCESS;
}

bool showFileProperties(HWND owner, const Directory& directory, std::span<const Index> files) {
    auto data = getShellObject<IDataObject>(owner, directory, files);
    return data && SUCCEEDED(SHMultiFileProperties(data.get(), 0));
}

} // namespace nf
