#include "folder_picker.hpp"
#include "job.hpp"
#include "resource.h"

#include <commctrl.h>

#include <algorithm>
#include <cwchar>

namespace nf {
namespace {

struct FolderPicker {
    std::wstring initial;
    Language language;
    HWND window = nullptr;
    HWND drive = nullptr;
    HWND list = nullptr;
    Listing folders;
    Job<Listing> job;
    std::wstring requested;
    std::wstring selected;

    FolderPicker(std::wstring path, Language currentLanguage)
        : initial(std::move(path)), language(currentLanguage) {}
    ~FolderPicker() {
        if (job.progress) job.progress->cancel = true;
    }

    const wchar_t* tr(const wchar_t* english, const wchar_t* russian) const {
        return translated(language, english, russian);
    }

    void init(HWND dialog);
    void navigate(std::wstring path, bool findParent = false);
    void finishRead();
    void updateLocation(const std::wstring& path);
    void openRow(int row);

    static INT_PTR CALLBACK callback(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK listKeys(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR owner);
};

void FolderPicker::init(HWND dialog) {
    window = dialog;
    drive = GetDlgItem(window, IDC_FOLDER_DRIVE);
    list = GetDlgItem(window, IDC_FOLDER_LIST);

    SetWindowTextW(window, tr(L"Choose a folder", L"Выбор папки"));
    SetDlgItemTextW(window, IDC_FOLDER_UP, tr(L"Up", L"Вверх"));
    SetDlgItemTextW(window, IDOK, tr(L"Select this folder", L"Выбрать эту папку"));
    SetDlgItemTextW(window, IDCANCEL, tr(L"Cancel", L"Отмена"));

    wchar_t drives[256]{};
    GetLogicalDriveStringsW(static_cast<DWORD>(std::size(drives)), drives);
    for (const auto* root = drives; *root; root += std::wcslen(root) + 1) {
        SendMessageW(drive, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(root));
    }

    ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
    RECT area{};
    GetClientRect(list, &area);
    LVCOLUMNW column{};
    column.mask = LVCF_WIDTH;
    column.cx = std::max(0L, area.right - GetSystemMetricsForDpi(SM_CXVSCROLL, GetDpiForWindow(window)));
    ListView_InsertColumn(list, 0, &column);
    SetWindowSubclass(list, listKeys, 1, reinterpret_cast<DWORD_PTR>(window));

    EnableWindow(GetDlgItem(window, IDOK), FALSE);
    EnableWindow(GetDlgItem(window, IDC_FOLDER_UP), FALSE);
    SetTimer(window, 1, 75, nullptr);
    if (initial.empty()) initial = drives;
    if (!initial.empty()) navigate(std::move(initial), true);
}

void FolderPicker::updateLocation(const std::wstring& path) {
    SetDlgItemTextW(window, IDC_FOLDER_PATH, path.c_str());
    if (path.size() >= 3 && path[1] == L':') {
        const auto root = path.substr(0, 3);
        SendMessageW(drive, CB_SELECTSTRING, static_cast<WPARAM>(-1), reinterpret_cast<LPARAM>(root.c_str()));
    } else {
        SendMessageW(drive, CB_SETCURSEL, static_cast<WPARAM>(-1), 0);
    }
    EnableWindow(GetDlgItem(window, IDC_FOLDER_UP), !parentPath(path).empty());
}

void FolderPicker::navigate(std::wstring path, bool findParent) {
    if (path.empty() || (job && requested == path)) return;
    job.start([path, findParent](Progress& progress) mutable {
        for (;;) {
            try {
                return readDirectory(path, {}, progress, DirectoryContents::FoldersOnly);
            } catch (DWORD error) {
                if (!findParent || progress.cancel ||
                    (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND && error != ERROR_DIRECTORY)) throw;
                auto parent = parentPath(path);
                if (parent.empty()) throw;
                path = std::move(parent);
            }
        }
    });
    requested = std::move(path);
    updateLocation(requested);
    SetDlgItemTextW(window, IDC_FOLDER_STATUS, tr(L"Reading folders…", L"Чтение папок…"));
    EnableWindow(GetDlgItem(window, IDOK), FALSE);
    ListView_SetItemCountEx(list, 0, 0);
}

void FolderPicker::finishRead() {
    if (!job.ready()) return;
    auto future = std::move(job.future);
    try {
        folders = future.get();
        SetDlgItemTextW(window, IDC_FOLDER_STATUS, folders.order.empty() ?
            tr(L"No subfolders", L"Вложенных папок нет") :
            tr(L"Double-click a folder to open it.", L"Двойной щелчок открывает папку."));
    } catch (DWORD error) {
        SetDlgItemTextW(window, IDC_FOLDER_STATUS, windowsError(error, language).c_str());
    } catch (...) {
        SetDlgItemTextW(window, IDC_FOLDER_STATUS, tr(L"Could not read the folder.", L"Не удалось прочитать папку."));
    }

    if (folders.directory) {
        updateLocation(folders.directory->path);
        ListView_SetItemCountEx(list, static_cast<int>(folders.order.size()), 0);
        ListView_SetItemState(list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
        if (!folders.order.empty()) ListView_EnsureVisible(list, 0, FALSE);
    }
    EnableWindow(GetDlgItem(window, IDOK), static_cast<bool>(folders.directory));
    requested.clear();
}

void FolderPicker::openRow(int row) {
    if (job || !folders.directory || row < 0 || static_cast<std::size_t>(row) >= folders.order.size()) return;
    navigate(childPath(folders.directory->path, folders.directory->name(folders.order[row])));
}

INT_PTR CALLBACK FolderPicker::callback(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_INITDIALOG) SetWindowLongPtrW(hwnd, DWLP_USER, lParam);
    auto* picker = reinterpret_cast<FolderPicker*>(GetWindowLongPtrW(hwnd, DWLP_USER));
    if (!picker) return FALSE;

    try {
        switch (message) {
            case WM_INITDIALOG:
                picker->init(hwnd);
                return TRUE;
            case WM_TIMER:
                picker->finishRead();
                return TRUE;
            case WM_CLOSE:
                EndDialog(hwnd, IDCANCEL);
                return TRUE;
            case WM_COMMAND:
                switch (LOWORD(wParam)) {
                    case IDCANCEL:
                        EndDialog(hwnd, IDCANCEL);
                        return TRUE;
                    case IDOK:
                        if (!picker->job && picker->folders.directory) {
                            picker->selected = picker->folders.directory->path;
                            EndDialog(hwnd, IDOK);
                        }
                        return TRUE;
                    case IDC_FOLDER_UP: {
                        if (HIWORD(wParam) != BN_CLICKED) return TRUE;
                        const auto& current = picker->requested.empty() && picker->folders.directory ?
                            picker->folders.directory->path : picker->requested;
                        picker->navigate(parentPath(current));
                        return TRUE;
                    }
                    case IDC_FOLDER_OPEN:
                        picker->openRow(ListView_GetNextItem(picker->list, -1, LVNI_FOCUSED));
                        return TRUE;
                    case IDC_FOLDER_DRIVE:
                        if (HIWORD(wParam) == CBN_SELENDOK ||
                            (HIWORD(wParam) == CBN_SELCHANGE && !SendMessageW(picker->drive, CB_GETDROPPEDSTATE, 0, 0))) {
                            const auto index = SendMessageW(picker->drive, CB_GETCURSEL, 0, 0);
                            if (index != CB_ERR) {
                                wchar_t root[4]{};
                                SendMessageW(picker->drive, CB_GETLBTEXT, static_cast<WPARAM>(index), reinterpret_cast<LPARAM>(root));
                                picker->navigate(root);
                            }
                        }
                        return TRUE;
                }
                break;
            case WM_NOTIFY: {
                const auto& header = *reinterpret_cast<NMHDR*>(lParam);
                if (header.hwndFrom != picker->list) break;
                if (header.code == NM_DBLCLK) {
                    picker->openRow(reinterpret_cast<NMITEMACTIVATE*>(lParam)->iItem);
                    return TRUE;
                }
                if (header.code != LVN_GETDISPINFOW) break;
                auto& item = reinterpret_cast<NMLVDISPINFOW*>(lParam)->item;
                if (!(item.mask & LVIF_TEXT) || !item.pszText || item.cchTextMax <= 0) return TRUE;
                item.pszText[0] = 0;
                if (item.iItem < 0 || static_cast<std::size_t>(item.iItem) >= picker->folders.order.size()) return TRUE;
                const auto name = picker->folders.directory->name(picker->folders.order[item.iItem]);
                const auto length = std::min(name.size(), static_cast<std::size_t>(item.cchTextMax - 1));
                std::wmemcpy(item.pszText, name.data(), length);
                item.pszText[length] = 0;
                return TRUE;
            }
        }
    } catch (...) {
        SetDlgItemTextW(hwnd, IDC_FOLDER_STATUS, picker->tr(L"Could not open the folder.", L"Не удалось открыть папку."));
        if (message == WM_INITDIALOG) EndDialog(hwnd, IDCANCEL);
        return TRUE;
    }
    return FALSE;
}

LRESULT CALLBACK FolderPicker::listKeys(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR owner) {
    if (message == WM_GETDLGCODE && lParam) {
        const auto& key = *reinterpret_cast<const MSG*>(lParam);
        if (key.message == WM_KEYDOWN && (key.wParam == VK_RETURN || key.wParam == VK_BACK)) {
            return DefSubclassProc(hwnd, message, wParam, lParam) | DLGC_WANTMESSAGE;
        }
    }
    if (message == WM_KEYDOWN && (wParam == VK_RETURN || wParam == VK_BACK)) {
        const auto command = wParam == VK_RETURN ? IDC_FOLDER_OPEN : IDC_FOLDER_UP;
        SendMessageW(reinterpret_cast<HWND>(owner), WM_COMMAND, MAKEWPARAM(command, BN_CLICKED), 0);
        return 0;
    }
    return DefSubclassProc(hwnd, message, wParam, lParam);
}

} // namespace

std::optional<std::wstring> chooseFolder(HINSTANCE instance, HWND owner, std::wstring initial, Language language) {
    FolderPicker picker{std::move(initial), language};
    const auto result = DialogBoxParamW(instance, MAKEINTRESOURCEW(IDD_FOLDER_PICKER), owner,
                                        FolderPicker::callback, reinterpret_cast<LPARAM>(&picker));
    if (result == -1) throw GetLastError();
    if (result == IDOK) return std::move(picker.selected);
    return std::nullopt;
}

}
