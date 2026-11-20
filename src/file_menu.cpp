#include "file_menu.hpp"
#include "text_files.hpp"

#include <shlobj.h>

#include <algorithm>
#include <limits>

namespace nf {
namespace {

constexpr wchar_t dropEffectName[] = L"Preferred DropEffect";
constexpr DWORD dropCopy = 1;
constexpr DWORD dropMove = 2;

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

UINT showFileMenu(HWND owner, POINT point, bool selected, std::optional<bool> singleFolder,
                  bool canRestore, Language language) {
    const auto popup = CreatePopupMenu();
    if (!popup) return 0;
    const bool single = singleFolder.has_value();
    const bool file = single && !*singleFolder;
    auto add = [&](HMENU menu, UINT id, bool enabled, const wchar_t* english, const wchar_t* russian) {
        AppendMenuW(menu, enabled ? MF_STRING : MF_STRING | MF_GRAYED, id, translated(language, english, russian));
    };
    auto sub = [&](const wchar_t* english, const wchar_t* russian) {
        const auto child = CreatePopupMenu();
        if (child && !AppendMenuW(popup, MF_POPUP, reinterpret_cast<UINT_PTR>(child),
                                  translated(language, english, russian))) {
            DestroyMenu(child);
            return HMENU{};
        }
        return child;
    };
    auto divider = [&]() { AppendMenuW(popup, MF_SEPARATOR, 0, nullptr); };

    if (singleFolder.value_or(false)) add(popup, OpenFolder, true, L"Open folder", L"Открыть папку");
    add(popup, ViewItems, selected, L"View / Calculate size", L"Просмотр / Размер");
    add(popup, EditFile, file, L"Edit…", L"Редактировать…");
    add(popup, FileProperties, selected, L"Properties", L"Свойства");
    divider();
    add(popup, CopyItems, selected, L"Copy to folder…", L"Копировать в папку…");
    add(popup, MoveItems, selected, L"Move to folder…", L"Переместить в папку…");
    add(popup, RenameFile, single, L"Rename…", L"Переименовать…");
    add(popup, BatchRenameFiles, selected, L"Batch rename…", L"Пакетное переименование…");
    add(popup, MakeFolder, true, L"Make folder…", L"Создать папку…");
    if (const auto link = sub(L"Create link", L"Создать ссылку")) {
        add(link, HardLinkItems, selected, L"Hard link…", L"Жёсткая ссылка…");
        add(link, SymbolicLinkItems, selected, L"Symbolic link…", L"Символическая ссылка…");
        add(link, JunctionItems, selected, L"Folder junction…", L"Соединение папок…");
    }
    if (const auto clipboard = sub(L"Clipboard", L"Буфер обмена")) {
        add(clipboard, CutFiles, selected, L"Cut", L"Вырезать");
        add(clipboard, CopyFiles, selected, L"Copy", L"Копировать");
        add(clipboard, PasteFiles, IsClipboardFormatAvailable(CF_HDROP), L"Paste here", L"Вставить сюда");
    }
    divider();
    add(popup, DeleteFiles, selected, L"Delete", L"Удалить");
    add(popup, WipeFiles, selected, L"Wipe…", L"Затереть…");
    if (const auto archive = sub(L"ZIP archive", L"ZIP-архив")) {
        add(archive, PackZip, selected, L"Create archive…", L"Создать архив…");
        add(archive, ExtractZip, selected, L"Extract…", L"Распаковать…");
        add(archive, ListZip, file, L"List contents", L"Список содержимого");
        add(archive, TestZip, selected, L"Test archive", L"Проверить архив");
    }
    if (const auto attributes = sub(L"Attributes and time", L"Атрибуты и время")) {
        add(attributes, FileAttributes, selected, L"Change attributes…", L"Изменить атрибуты…");
        add(attributes, ModifiedTime, selected, L"Set modification time…", L"Изменить время записи…");
        add(attributes, CreatedTime, selected, L"Set creation time…", L"Изменить время создания…");
        add(attributes, AccessedTime, selected, L"Set access time…", L"Изменить время доступа…");
        add(attributes, ChangedTime, selected, L"Set metadata change time…", L"Изменить время изменения метаданных…");
    }
    add(popup, ApplyCommand, selected, L"Apply command…", L"Выполнить команду…");
    add(popup, DescribeItems, selected, L"Describe…", L"Описание…");
    if (const auto selection = sub(L"Selection", L"Выделение")) {
        add(selection, SelectMask, true, L"Select by mask…", L"Выделить по маске…");
        add(selection, DeselectMask, true, L"Deselect by mask…", L"Снять выделение по маске…");
        add(selection, InvertSelection, true, L"Invert selection", L"Инвертировать выделение");
        add(selection, RestoreSelection, canRestore, L"Restore selection", L"Восстановить выделение");
    }

    const auto command = static_cast<UINT>(TrackPopupMenuEx(popup, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                                           point.x, point.y, owner, nullptr));
    DestroyMenu(popup);
    return command;
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
    *preferred = cut ? dropMove : dropCopy;
    GlobalUnlock(effect.handle);

    const UINT effectFormat = RegisterClipboardFormatW(dropEffectName);
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

ClipboardFiles filesFromClipboard(HWND owner) {
    ClipboardLock clipboard(owner);
    if (!clipboard.opened) throw GetLastError();
    ClipboardFiles result;
    result.sequence = GetClipboardSequenceNumber();
    const auto drop = static_cast<HGLOBAL>(GetClipboardData(CF_HDROP));
    if (!drop) throw static_cast<DWORD>(ERROR_NO_DATA);

    const auto bytes = GlobalSize(drop);
    if (bytes < sizeof(DROPFILES)) throw static_cast<DWORD>(ERROR_INVALID_DATA);
    const auto* raw = static_cast<const std::byte*>(GlobalLock(drop));
    if (!raw) throw GetLastError();
    struct Unlock {
        HGLOBAL value;
        ~Unlock() { GlobalUnlock(value); }
    } unlock{drop};
    const auto& header = *reinterpret_cast<const DROPFILES*>(raw);
    if (header.pFiles < sizeof(DROPFILES) || header.pFiles >= bytes) throw static_cast<DWORD>(ERROR_INVALID_DATA);
    if (header.fWide) {
        if (header.pFiles % alignof(wchar_t)) throw static_cast<DWORD>(ERROR_INVALID_DATA);
        Name names(reinterpret_cast<const wchar_t*>(raw + header.pFiles), (bytes - header.pFiles) / sizeof(wchar_t));
        while (!names.empty() && names.front()) {
            const auto end = names.find(L'\0');
            if (end == Name::npos) throw static_cast<DWORD>(ERROR_INVALID_DATA);
            result.paths.emplace_back(names.substr(0, end));
            names.remove_prefix(end + 1);
        }
        if (names.empty()) throw static_cast<DWORD>(ERROR_INVALID_DATA);
    } else {
        std::string_view names(reinterpret_cast<const char*>(raw + header.pFiles), bytes - header.pFiles);
        while (!names.empty() && names.front()) {
            const auto end = names.find('\0');
            if (end == names.npos) throw static_cast<DWORD>(ERROR_INVALID_DATA);
            result.paths.push_back(decodeBytes(names.substr(0, end), GetACP()));
            names.remove_prefix(end + 1);
        }
        if (names.empty()) throw static_cast<DWORD>(ERROR_INVALID_DATA);
    }

    const auto effect = static_cast<HGLOBAL>(GetClipboardData(RegisterClipboardFormatW(dropEffectName)));
    if (effect && GlobalSize(effect) >= sizeof(DWORD)) {
        if (const auto* value = static_cast<const DWORD*>(GlobalLock(effect))) {
            result.cut = (*value & dropMove) != 0;
            GlobalUnlock(effect);
        }
    }
    return result;
}

void clearFileClipboard(HWND owner, DWORD sequence) {
    ClipboardLock clipboard(owner);
    if (clipboard.opened && sequence == GetClipboardSequenceNumber()) EmptyClipboard();
}

}
