#include "file_menu.hpp"
#include "folder_picker.hpp"
#include "job.hpp"
#include "operations.hpp"
#include "text_files.hpp"
#include "resource.h"

#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <cwchar>
#include <cwctype>
#include <utility>
#include <unordered_set>

namespace {
using namespace nf;

constexpr wchar_t appName[] = L"NotFar Manager";
constexpr auto appClass = L"NotFarManager.Window";
constexpr UINT operationNotice = WM_APP + 1;
constexpr UINT beginDragCapture = WM_APP + 2;
constexpr wchar_t settingsKey[] = L"Software\\NotFar Manager";
constexpr wchar_t languageValue[] = L"Language";
constexpr COLORREF ink = RGB(34, 48, 55);
constexpr COLORREF accent = RGB(24, 100, 105);
constexpr COLORREF canvas = RGB(239, 244, 245);
constexpr COLORREF marked = RGB(191, 25, 40);
constexpr COLORREF activeTint = RGB(214, 237, 238);
constexpr int firstPanelId = 300;
enum Control {
    Threshold = 200, SelectSmall, DeleteSelected, ClearSelected, StopOperation, LanguageChoice,
    SelectAll, BatchRename, Refresh, IncludeSubfolders
};
enum PanelControl { Drive, Path, Up, List, Status };

std::wstring controlText(HWND control) {
    const int length = GetWindowTextLengthW(control);
    std::wstring result(static_cast<std::size_t>(length) + 1, L'\0');
    result.resize(GetWindowTextW(control, result.data(), length + 1));
    return result;
}

void setText(HWND control, const std::wstring& text) {
    if (controlText(control) != text) SetWindowTextW(control, text.c_str());
}

void copyText(NMLVDISPINFOW& info, Name text) {
    if (!(info.item.mask & LVIF_TEXT) || !info.item.pszText || info.item.cchTextMax <= 0) return;
    const auto length = std::min(text.size(), static_cast<std::size_t>(info.item.cchTextMax - 1));
    if (length) std::wmemcpy(info.item.pszText, text.data(), length);
    info.item.pszText[length] = 0;
}

void addColumn(HWND list, int index, const wchar_t* text, int width, int format = LVCFMT_LEFT) {
    LVCOLUMNW column{};
    column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
    column.pszText = const_cast<wchar_t*>(text);
    column.cx = width;
    column.fmt = format;
    ListView_InsertColumn(list, index, &column);
}

std::wstring phaseLabel(Phase phase, Language language) {
    switch (phase) {
        case Phase::Read: return translated(language, L"Reading folder", L"Чтение каталога");
        case Phase::Search: return translated(language, L"Searching subfolders", L"Поиск в подпапках");
        case Phase::Sort: return translated(language, L"Sorting", L"Сортировка");
        case Phase::Validate: return translated(language, L"Checking new names", L"Проверка новых имён");
        case Phase::Rename: return translated(language, L"Renaming", L"Переименование");
        case Phase::Delete: return translated(language, L"Deleting", L"Удаление");
        case Phase::Copy: return translated(language, L"Copying", L"Копирование");
        case Phase::Move: return translated(language, L"Moving", L"Перемещение");
        case Phase::Wipe: return translated(language, L"Wiping", L"Затирание");
        case Phase::Change: return translated(language, L"Applying changes", L"Применение изменений");
        case Phase::Run: return translated(language, L"Running commands", L"Выполнение команд");
        case Phase::Archive: return translated(language, L"Processing ZIP", L"Обработка ZIP");
    }
    return {};
}

void updateProgress(HWND status, HWND bar, const Progress& progress, Language language) {
    const auto total = progress.total.load();
    const auto done = progress.done.load();
    auto text = phaseLabel(progress.phase, language);
    if (total) text += L"   " + number(std::min(done, total)) + L" / " + number(total);
    if (progress.cancel) text += translated(language, L"   ·   Stopping…", L"   ·   Остановка…");
    setText(status, text);
    SendMessageW(bar, PBM_SETPOS, total ? static_cast<WPARAM>(std::min(done, total) * 1000ULL / total) : 0, 0);
}

struct Pane {
    HWND drive = nullptr;
    HWND path = nullptr;
    HWND up = nullptr;
    HWND list = nullptr;
    HWND status = nullptr;
    Listing visible;
    Sort sort;
    Sort requestedSort;
    Job<Listing> job;
    std::wstring pendingPath;
    std::optional<std::uint64_t> pendingLimit;
    std::vector<std::uint8_t> selected;
    std::shared_ptr<const Directory> previousDirectory;
    std::vector<std::uint8_t> previousSelection;
    std::size_t selectedCount = 0;
    std::uint64_t selectedBytes = 0;
    int anchor = -1;

    bool hasParent() const {
        return visible.directory && !visible.directory->recursive && !parentPath(visible.directory->path).empty();
    }
    int offset() const { return parentRow ? 1 : 0; }
    int count() const { return static_cast<int>(visible.order.size()) + offset(); }
    bool parentRow = false;

    std::optional<Index> index(int row) const {
        const int value = row - offset();
        if (value < 0 || value >= static_cast<int>(visible.order.size())) return {};
        return visible.order[value];
    }

    std::vector<Index> orderedSelection() const {
        std::vector<Index> result;
        result.reserve(selectedCount);
        for (Index i : visible.order) if (selected[i]) result.push_back(i);
        return result;
    }
};

struct App;

enum class PromptPath { Text, Folder, File };

struct CopyDestination {
    std::wstring path;
    bool permissions = false;
};

struct PromptDialog {
    App& app;
    const wchar_t* title;
    const wchar_t* label;
    std::wstring value;
    PromptPath pathKind;
    Name base;
    bool* copyPermissions;
    static INT_PTR CALLBACK callback(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
};

struct TextDialog {
    App& app;
    std::wstring title;
    TextFile file;
    bool editable;
    static INT_PTR CALLBACK callback(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
};

struct FileRenameDialog {
    App& app;
    const Directory& directory;
    Index file;

    static INT_PTR CALLBACK callback(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
};

struct RenameDialog {
    App& app;
    std::shared_ptr<const Directory> directory;
    std::shared_ptr<const std::vector<Index>> files;
    RenameOptions options;
    HWND window = nullptr;
    HWND preview = nullptr;
    HWND status = nullptr;
    HWND bar = nullptr;
    SIZE minimum{};
    RECT initialPreview{};
    RECT initialStatus{};
    RECT initialBar{};
    RECT initialExecute{};
    RECT initialCancel{};
    std::wstring issue;
    std::wstring validationError;
    bool working = true;

    RenameDialog(App& owner, std::shared_ptr<const Directory> source,
                 std::shared_ptr<const std::vector<Index>> selection, RenameOptions settings)
        : app(owner), directory(std::move(source)), files(std::move(selection)), options(std::move(settings)) {}
    static INT_PTR CALLBACK callback(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    void init(HWND hwnd);
    void layout();
    void refresh();
    void setWorking(bool busy);
};

struct App {
    HINSTANCE instance;
    Name initialLeft;
    Name initialRight;
    HWND window = nullptr;
    HWND threshold = nullptr;
    HWND selectButton = nullptr;
    HWND includeSubfolders = nullptr;
    HWND selectAllButton = nullptr;
    HWND renameButton = nullptr;
    HWND refreshButton = nullptr;
    HWND deleteButton = nullptr;
    HWND clearButton = nullptr;
    HWND bytes = nullptr;
    HWND help = nullptr;
    HWND activity = nullptr;
    HWND languageChoice = nullptr;
    HWND progress = nullptr;
    HWND stop = nullptr;
    HFONT font = nullptr;
    HBRUSH background = nullptr;
    HBRUSH statusTint = nullptr;
    UINT dpi = 96;
    int panelY = 58;
    Language language = Language::English;
    Pane panes[2];
    int active = 0;
    int dragSource = -1;
    Job<OperationResult> operation;
    std::wstring operationPath;
    std::wstring operationDestination;
    DWORD clipboardSequence = 0;
    std::wstring notice;
    std::wstring lastStatus;
    RenameOptions previousRename;
    RenameDialog* renameDialog = nullptr;
    bool renaming = false;
    bool closeAfterOperation = false;

    App(HINSTANCE module, Name left, Name right) : instance(module), initialLeft(left), initialRight(right) {}
    ~App() {
        for (auto& pane : panes) if (pane.job.progress) pane.job.progress->cancel = true;
        if (font) DeleteObject(font);
        if (background) DeleteObject(background);
        if (statusTint) DeleteObject(statusTint);
    }

    int px(int value) const { return MulDiv(value, dpi, 96); }
    const wchar_t* tr(const wchar_t* english, const wchar_t* russian) const {
        return translated(language, english, russian);
    }
    int message(HWND owner, const wchar_t* content, const wchar_t* title, UINT type) const;
    static LRESULT CALLBACK callback(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK childProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam,
                                     UINT_PTR subclass, DWORD_PTR context);
    LRESULT handle(UINT message, WPARAM wParam, LPARAM lParam);
    void create();
    void layout();
    void localize();
    void changeLanguage();
    void rebuildFont();
    void setActive(int side);
    void updateCommands();
    void updatePane(Pane& pane);
    void updateHeader(Pane& pane);
    void startRead(Pane& pane, std::wstring path, bool force = false);
    void sortBy(Pane& pane, int col);
    void consumeListing(Pane& pane);
    void toggle(Pane& pane, int row);
    void selectRange(Pane& pane, int first, int last);
    void mark(Pane& pane, Index index, bool value);
    void redrawSelection(Pane& pane);
    void clickFile(Pane& pane, int row, bool ctrl, bool shift);
    void resetSelection(Pane& pane);
    void selectSmaller();
    void selectAll();
    void clearSelection();
    void refresh();
    void refreshPath(Name path);
    void showContextMenu(Pane& pane, int row, POINT point);
    void rememberSelection(Pane& pane);
    void changeSelection(Pane& pane, UINT command);
    std::optional<std::wstring> prompt(const wchar_t* title, const wchar_t* label, std::wstring value,
                                       PromptPath pathKind = PromptPath::Text, Name base = {},
                                       bool* copyPermissions = nullptr);
    std::optional<std::wstring> destination(Pane& pane, const wchar_t* title);
    std::optional<CopyDestination> copyDestination(Name source, std::wstring initial);
    void copyTo(Pane& source, const Pane& target);
    bool canDrop(POINT screenPoint) const;
    void finishDrag(bool copy);
    void showText(std::wstring title, TextFile file, bool editable);
    template<class Action>
    void startAction(Pane& pane, Phase phase, Name sourceToRefresh, Name destinationToRefresh, Action action);
    void renameOne(const Directory& directory, Index file);
    void enterRow(Pane& pane, int row);
    void goUp(Pane& pane);
    void goToPath(Pane& pane);
    void openRename();
    void startRename(const RenameOptions& options);
    void startDelete(bool wipe = false);
    void consumeOperation();
    bool key(HWND hwnd, UINT key);
    LRESULT notify(NMHDR& header);
    HWND make(DWORD ex, const wchar_t* klass, const wchar_t* label, DWORD style, int id = 0);

    Pane& current() { return panes[active]; }
    bool available(const Pane& pane) const { return pane.visible.directory && !pane.job && !operation; }
};

int App::message(HWND owner, const wchar_t* content, const wchar_t* title, UINT type) const {
    const bool confirm = (type & MB_TYPEMASK) == MB_YESNO;
    const TASKDIALOG_BUTTON buttons[] = {
        {confirm ? IDYES : IDOK, confirm ? tr(L"Yes", L"Да") : L"OK"},
        {IDNO, tr(L"No", L"Нет")}
    };
    TASKDIALOGCONFIG config{};
    config.cbSize = sizeof(config);
    config.hwndParent = owner;
    config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
    config.pszWindowTitle = title;
    config.pszContent = content;
    config.cButtons = confirm ? 2 : 1;
    config.pButtons = buttons;
    config.nDefaultButton = confirm ? IDNO : IDOK;
    switch (type & MB_ICONMASK) {
        case MB_ICONERROR: config.pszMainIcon = TD_ERROR_ICON; break;
        case MB_ICONWARNING: config.pszMainIcon = TD_WARNING_ICON; break;
        case MB_ICONINFORMATION: config.pszMainIcon = TD_INFORMATION_ICON; break;
    }
    int button = 0;
    if (SUCCEEDED(TaskDialogIndirect(&config, &button, nullptr, nullptr))) return button;
    const auto locale = language == Language::Russian ? LANG_RUSSIAN : LANG_ENGLISH;
    return MessageBoxExW(owner, content, title, type, MAKELANGID(locale, SUBLANG_DEFAULT));
}

HWND App::make(DWORD ex, const wchar_t* klass, const wchar_t* label, DWORD style, int id) {
    return CreateWindowExW(ex, klass, label, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, window,
                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
}

void App::create() {
    dpi = GetDpiForWindow(window);
    background = CreateSolidBrush(canvas);
    statusTint = CreateSolidBrush(activeTint);
    DWORD setting = 0;
    DWORD settingSize = sizeof(setting);
    if (RegGetValueW(HKEY_CURRENT_USER, settingsKey, languageValue, RRF_RT_REG_DWORD,
                     nullptr, &setting, &settingSize) == ERROR_SUCCESS && setting == 1) {
        language = Language::Russian;
    }
    selectButton = make(0, L"BUTTON", L"", WS_TABSTOP, SelectSmall);
    threshold = make(WS_EX_CLIENTEDGE, L"EDIT", L"2048", ES_NUMBER | ES_AUTOHSCROLL | WS_TABSTOP, Threshold);
    SendMessageW(threshold, EM_LIMITTEXT, 20, 0);
    bytes = make(0, L"STATIC", L"", SS_CENTERIMAGE);
    includeSubfolders = make(0, L"BUTTON", L"", BS_AUTOCHECKBOX | WS_TABSTOP, IncludeSubfolders);
    deleteButton = make(0, L"BUTTON", L"", WS_TABSTOP, DeleteSelected);
    clearButton = make(0, L"BUTTON", L"", WS_TABSTOP, ClearSelected);
    selectAllButton = make(0, L"BUTTON", L"", WS_TABSTOP, SelectAll);
    renameButton = make(0, L"BUTTON", L"", WS_TABSTOP, BatchRename);
    refreshButton = make(0, L"BUTTON", L"", WS_TABSTOP, Refresh);
    help = make(0, L"STATIC", L"", SS_CENTERIMAGE);
    activity = make(0, L"STATIC", L"", SS_CENTERIMAGE | SS_NOPREFIX | SS_ENDELLIPSIS);
    progress = make(0, PROGRESS_CLASSW, L"", PBS_SMOOTH);
    SendMessageW(progress, PBM_SETRANGE32, 0, 1000);
    stop = make(0, L"BUTTON", L"", WS_TABSTOP, StopOperation);
    languageChoice = make(0, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, LanguageChoice);
    SendMessageW(languageChoice, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"English"));
    SendMessageW(languageChoice, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Русский"));
    SendMessageW(languageChoice, CB_SETCURSEL, language == Language::Russian ? 1 : 0, 0);

    wchar_t drives[256]{};
    GetLogicalDriveStringsW(static_cast<DWORD>(std::size(drives)), drives);
    for (int i = 0; i < 2; ++i) {
        auto& pane = panes[i];
        const int id = firstPanelId + i * 10;
        pane.drive = make(0, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, id + Drive);
        for (const wchar_t* p = drives; *p; p += std::wcslen(p) + 1) {
            SendMessageW(pane.drive, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(p));
        }
        pane.path = make(WS_EX_CLIENTEDGE, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, id + Path);
        SendMessageW(pane.path, EM_LIMITTEXT, 32760, 0);
        pane.up = make(0, L"BUTTON", L"↑", WS_TABSTOP, id + Up);
        pane.list = make(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                         LVS_REPORT | LVS_OWNERDATA | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_TABSTOP, id + List);
        ListView_SetExtendedListViewStyle(pane.list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
        addColumn(pane.list, 0, L"", px(230));
        addColumn(pane.list, 1, L"", px(78));
        addColumn(pane.list, 2, L"", px(120), LVCFMT_RIGHT);
        pane.status = make(0, L"STATIC", L"", SS_CENTERIMAGE | SS_NOPREFIX | SS_ENDELLIPSIS, id + Status);
        updateHeader(pane);
    }
    for (HWND child = GetWindow(window, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
        SetWindowSubclass(child, childProc, 1, reinterpret_cast<DWORD_PTR>(this));
    }
    rebuildFont();
    localize();

    wchar_t windows[MAX_PATH]{};
    GetWindowsDirectoryW(windows, static_cast<UINT>(std::size(windows)));
    auto right = absolutePath(Name(windows).substr(0, 3));
    if (right.empty() && *drives) right = drives;
    wchar_t profile[32768]{};
    GetEnvironmentVariableW(L"USERPROFILE", profile, static_cast<DWORD>(std::size(profile)));
    auto left = absolutePath(profile);
    if (left.empty()) left = right;
    if (!initialLeft.empty()) left = absolutePath(initialLeft);
    if (!initialRight.empty()) right = absolutePath(initialRight);
    if (!left.empty()) startRead(panes[0], std::move(left));
    if (!right.empty()) startRead(panes[1], std::move(right));
    SetTimer(window, 1, 85, nullptr);
    layout();
    updateCommands();
    SetFocus(panes[0].list);
}

void App::rebuildFont() {
    HFONT next = CreateFontW(-px(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    if (!next) return;
    for (HWND child = GetWindow(window, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
        SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(next), TRUE);
    }
    if (font) DeleteObject(font);
    font = next;
}

void App::layout() {
    RECT rc{};
    GetClientRect(window, &rc);
    const int width = rc.right;
    const int height = rc.bottom;
    const int margin = px(12);
    const int gap = px(8);
    auto move = [](HWND control, int x, int y, int w, int h) {
        MoveWindow(control, x, y, std::max(0, w), std::max(0, h), TRUE);
    };
    int x = margin;
    int y = px(14);
    auto top = [&](HWND control, int w, int h = 28, int offsetY = 0) {
        move(control, x, y + px(offsetY), px(w), px(h));
        x += px(w) + gap;
    };
    const int languageX = width - margin - px(112);
    move(languageChoice, languageX, y + px(1), px(112), px(140));
    top(selectButton, 192);
    top(threshold, 82, 26, 1);
    top(bytes, 33);
    top(includeSubfolders, 160);

    constexpr int selectAllWidth = 116;
    constexpr int clearWidth = 152;
    constexpr int renameWidth = 196;
    constexpr int deleteWidth = 179;
    constexpr int refreshWidth = 104;
    if (x + px(selectAllWidth + clearWidth + renameWidth + deleteWidth + refreshWidth) + 5 * gap > languageX) {
        x = margin;
        y += px(36);
    }
    top(selectAllButton, selectAllWidth);
    top(clearButton, clearWidth);
    top(renameButton, renameWidth);
    top(deleteButton, deleteWidth);
    top(refreshButton, refreshWidth);

    panelY = y + px(44);
    const int footerY = height - px(34);
    const int paneWidth = (width - margin * 2 - gap) / 2;
    for (int i = 0; i < 2; ++i) {
        Pane& pane = panes[i];
        x = margin + i * (paneWidth + gap);
        const int w = i ? width - margin - x : paneWidth;
        move(pane.drive, x + px(4), panelY + px(4), px(72), px(260));
        move(pane.path, x + px(82), panelY + px(4), w - px(116), px(25));
        move(pane.up, x + w - px(30), panelY + px(4), px(26), px(25));
        const int listY = panelY + px(33);
        const int statusY = footerY - px(30);
        move(pane.list, x, listY, w, statusY - listY);
        move(pane.status, x + px(6), statusY + px(1), w - px(12), px(25));
        RECT listArea{};
        GetClientRect(pane.list, &listArea);
        const int typeWidth = px(68);
        const int sizeWidth = px(117);
        ListView_SetColumnWidth(pane.list, 0, std::max(px(85), static_cast<int>(listArea.right) - typeWidth - sizeWidth - px(18)));
        ListView_SetColumnWidth(pane.list, 1, typeWidth);
        ListView_SetColumnWidth(pane.list, 2, sizeWidth);
    }

    move(help, margin, footerY, width - 2 * margin, px(25));
    move(activity, margin, footerY, width - px(340), px(25));
    move(progress, width - px(321), footerY + px(6), px(198), px(13));
    move(stop, width - px(115), footerY, px(103), px(25));
    InvalidateRect(window, nullptr, FALSE);
}

void App::localize() {
    setText(selectButton, tr(L"Select files smaller than", L"Выделить файлы меньше"));
    setText(bytes, tr(L"bytes", L"байт"));
    setText(includeSubfolders, tr(L"Include subfolders", L"Включая подпапки"));
    setText(deleteButton, tr(L"Delete selected", L"Удалить выделенное"));
    setText(clearButton, tr(L"Clear selection", L"Снять выделение"));
    setText(selectAllButton, tr(L"Select all", L"Выделить всё"));
    setText(renameButton, tr(L"Batch rename", L"Пакетное переименование"));
    setText(refreshButton, tr(L"Refresh", L"Обновить"));
    setText(stop, tr(L"Stop", L"Остановить"));
    setText(help, tr(L"Ctrl+M  Rename    Insert / Space  Select    F5  Refresh",
                     L"Ctrl+M  Переименовать    Insert / Пробел  Выделить    F5  Обновить"));

    const wchar_t* labels[] = {tr(L"Name", L"Имя"), tr(L"Type", L"Тип"), tr(L"Size", L"Размер")};
    for (auto& pane : panes) {
        const auto header = ListView_GetHeader(pane.list);
        for (int col = 0; col < 3; ++col) {
            HDITEMW item{};
            item.mask = HDI_TEXT;
            item.pszText = const_cast<wchar_t*>(labels[col]);
            Header_SetItem(header, col, &item);
        }
        updatePane(pane);
        InvalidateRect(pane.list, nullptr, FALSE);
    }
}

void App::changeLanguage() {
    const auto selection = SendMessageW(languageChoice, CB_GETCURSEL, 0, 0);
    if (selection == CB_ERR) return;
    const auto next = selection == 1 ? Language::Russian : Language::English;
    if (next == language) return;

    language = next;
    const DWORD setting = language == Language::Russian ? 1 : 0;
    RegSetKeyValueW(HKEY_CURRENT_USER, settingsKey, languageValue, REG_DWORD, &setting, sizeof(setting));
    lastStatus.clear();
    localize();
    updateCommands();
}

void App::setActive(int side) {
    if (active == side) return;
    active = side;
    InvalidateRect(window, nullptr, FALSE);
    for (auto& pane : panes) InvalidateRect(pane.status, nullptr, TRUE);
    updateCommands();
}

void App::updateCommands() {
    const auto& pane = current();
    const bool enable = available(pane);
    const bool hasSelection = enable && pane.selectedCount != 0;
    EnableWindow(selectButton, enable);
    EnableWindow(includeSubfolders, enable);
    EnableWindow(selectAllButton, enable && !pane.visible.order.empty());
    EnableWindow(refreshButton, !operation && (pane.visible.directory || !pane.pendingPath.empty()));
    EnableWindow(languageChoice, !operation);
    EnableWindow(renameButton, hasSelection);
    EnableWindow(deleteButton, hasSelection);
    EnableWindow(clearButton, hasSelection);
    for (const auto& item : panes) {
        EnableWindow(item.drive, !operation);
        EnableWindow(item.path, !operation);
        EnableWindow(item.up, available(item) && (item.parentRow || item.visible.directory->recursive));
    }
    const bool showing = operation && !renaming;
    const bool report = !operation && !lastStatus.empty();
    ShowWindow(help, showing || report ? SW_HIDE : SW_SHOWNA);
    ShowWindow(activity, showing || report ? SW_SHOWNA : SW_HIDE);
    ShowWindow(progress, showing ? SW_SHOWNA : SW_HIDE);
    ShowWindow(stop, showing ? SW_SHOWNA : SW_HIDE);
    if (report) setText(activity, lastStatus);
}

void App::updatePane(Pane& pane) {
    if (pane.job) {
        const auto phase = pane.job.progress->phase.load();
        const auto count = pane.job.progress->done.load();
        auto text = phaseLabel(phase, language) + L"…";
        if (count) text += L"   " + number(count);
        setText(pane.status, text);
        return;
    }
    if (!pane.visible.directory) {
        setText(pane.status, tr(L"Choose a drive or enter a folder path", L"Выберите диск или введите путь к папке"));
        return;
    }
    const auto files = pane.visible.directory->fileCount;
    const auto folders = pane.visible.directory->entries.size() - files;
    const auto& dir = *pane.visible.directory;
    auto text = dir.recursive
        ? tr(L"Matches ", L"Найдено ") + number(files)
        : tr(L"Files ", L"Файлов ") + number(files) + tr(L"   ·   Folders ", L"   ·   Папок ") + number(folders);
    if (dir.skippedFolders) {
        text += tr(L"   ·   Skipped folders ", L"   ·   Пропущено папок ") + number(dir.skippedFolders);
    }
    if (pane.selectedCount) {
        text += tr(L"   ·   Selected ", L"   ·   Выделено ") + number(pane.selectedCount) + L"   ·   " + number(pane.selectedBytes) + tr(L" bytes", L" байт");
    }
    setText(pane.status, text);
}

void App::updateHeader(Pane& pane) {
    HWND header = ListView_GetHeader(pane.list);
    for (int col = 0; col < 3; ++col) {
        HDITEMW item{};
        item.mask = HDI_FORMAT;
        Header_GetItem(header, col, &item);
        item.fmt &= ~(HDF_SORTUP | HDF_SORTDOWN);
        if (col == static_cast<int>(pane.sort.column)) item.fmt |= pane.sort.descending ? HDF_SORTDOWN : HDF_SORTUP;
        Header_SetItem(header, col, &item);
    }
}

void App::startRead(Pane& pane, std::wstring path, bool force) {
    if (path.empty() || operation) return;
    if (!force && ((!pane.job && pane.visible.directory && pane.visible.directory->path == path) ||
                   (pane.job && pane.pendingPath == path))) return;
    pane.requestedSort = pane.sort;
    pane.job.start([path, sort = pane.requestedSort](Progress& monitor) {
        return readDirectory(path, sort, monitor);
    });
    pane.pendingPath = std::move(path);
    pane.pendingLimit.reset();
    setText(pane.path, pane.pendingPath);
    if (pane.pendingPath.size() >= 3 && pane.pendingPath[1] == L':') {
        const auto drive = pane.pendingPath.substr(0, 3);
        SendMessageW(pane.drive, CB_SELECTSTRING, static_cast<WPARAM>(-1), reinterpret_cast<LPARAM>(drive.c_str()));
    } else {
        SendMessageW(pane.drive, CB_SETCURSEL, static_cast<WPARAM>(-1), 0);
    }
    updatePane(pane);
    updateCommands();
}

void App::sortBy(Pane& pane, int col) {
    if (!available(pane) || col < 0 || col > 2) return;
    Sort spec{static_cast<Column>(col), false};
    if (pane.sort.column == spec.column) spec.descending = !pane.sort.descending;
    pane.requestedSort = spec;
    pane.pendingPath.clear();
    pane.job.start([directory = pane.visible.directory, spec](Progress& monitor) {
        return Listing{directory, sortDirectory(*directory, spec, monitor)};
    }, Phase::Sort);
    updatePane(pane);
    updateCommands();
}

void App::consumeListing(Pane& pane) {
    if (!pane.job.ready()) return;
    auto future = std::move(pane.job.future);
    std::wstring reason;
    try {
        auto listing = future.get();
        const bool newListing = listing.directory != pane.visible.directory;
        const int cursor = ListView_GetNextItem(pane.list, -1, LVNI_FOCUSED);
        const auto oldIndex = pane.index(cursor);
        const auto oldAnchor = newListing ? std::nullopt : pane.index(pane.anchor);
        if (newListing) {
            if (pane.previousDirectory && compareNames(pane.previousDirectory->path, listing.directory->path) != 0) {
                pane.previousDirectory.reset();
                pane.previousSelection.clear();
            }
            pane.selected.assign(listing.order.size(), std::uint8_t{});
            pane.selectedCount = 0;
            pane.selectedBytes = 0;
        }
        pane.visible = std::move(listing);
        if (newListing && (pane.visible.directory->recursive || pane.pendingLimit)) {
            const auto& entries = pane.visible.directory->entries;
            for (Index i = 0; i < entries.size(); ++i) {
                if (pane.visible.directory->recursive ||
                    (!entries[i].folder() && entries[i].size < *pane.pendingLimit)) mark(pane, i, true);
            }
        }
        pane.parentRow = pane.hasParent();
        pane.sort = pane.requestedSort;
        auto rowOf = [&](Index index) {
            const auto found = std::find(pane.visible.order.begin(), pane.visible.order.end(), index);
            if (found == pane.visible.order.end()) return -1;
            return static_cast<int>(found - pane.visible.order.begin()) + pane.offset();
        };
        const int previousRow = !newListing && oldIndex ? rowOf(*oldIndex) : -1;
        pane.anchor = -1;
        if (oldAnchor) pane.anchor = oldAnchor == oldIndex ? previousRow : rowOf(*oldAnchor);
        const int row = std::max(0, previousRow);
        ListView_SetItemCountEx(pane.list, pane.count(), 0);
        ListView_SetItemState(pane.list, -1, 0, LVIS_FOCUSED | LVIS_SELECTED);
        if (pane.count()) {
            ListView_SetItemState(pane.list, row, LVIS_FOCUSED | LVIS_SELECTED, LVIS_FOCUSED | LVIS_SELECTED);
            ListView_EnsureVisible(pane.list, row, FALSE);
        }
        updateHeader(pane);
        InvalidateRect(pane.list, nullptr, FALSE);
        setText(pane.path, pane.visible.directory->path);
    } catch (DWORD code) {
        reason = windowsError(code, language);
    } catch (const std::bad_alloc&) {
        reason = tr(L"Out of memory.", L"Недостаточно памяти.");
    } catch (...) {
        reason = tr(L"Could not read or sort the folder.", L"Не удалось прочитать или отсортировать каталог.");
    }
    if (!reason.empty()) {
        if (pane.visible.directory) setText(pane.path, pane.visible.directory->path);
        SendMessageW(pane.drive, CB_SETCURSEL, static_cast<WPARAM>(-1), 0);
        if (pane.visible.directory && pane.visible.directory->path.size() >= 3 && pane.visible.directory->path[1] == L':') {
            SendMessageW(pane.drive, CB_SELECTSTRING, static_cast<WPARAM>(-1), reinterpret_cast<LPARAM>(pane.visible.directory->path.substr(0, 3).c_str()));
        }
    }
    pane.pendingPath.clear();
    pane.pendingLimit.reset();
    updatePane(pane);
    updateCommands();
    if (!reason.empty()) {
        lastStatus = reason;
        updateCommands();
    }
}

void App::mark(Pane& pane, Index index, bool value) {
    auto& selected = pane.selected[index];
    const auto& entry = pane.visible.directory->entries[index];
    if (static_cast<bool>(selected) == value) return;
    selected = value;
    if (value) {
        ++pane.selectedCount;
        pane.selectedBytes += entry.size;
    } else {
        --pane.selectedCount;
        pane.selectedBytes -= entry.size;
    }
}

void App::redrawSelection(Pane& pane) {
    InvalidateRect(pane.list, nullptr, FALSE);
    updatePane(pane);
    updateCommands();
}

void App::toggle(Pane& pane, int row) {
    if (!available(pane)) return;
    if (const auto index = pane.index(row)) {
        mark(pane, *index, !pane.selected[*index]);
        pane.anchor = row;
        redrawSelection(pane);
    }
}

void App::selectRange(Pane& pane, int first, int last) {
    if (!available(pane)) return;
    first = std::clamp(first, 0, std::max(0, pane.count() - 1));
    last = std::clamp(last, 0, std::max(0, pane.count() - 1));
    if (first > last) std::swap(first, last);
    for (int row = first; row <= last; ++row) if (auto index = pane.index(row)) mark(pane, *index, true);
    redrawSelection(pane);
}

void App::clickFile(Pane& pane, int row, bool ctrl, bool shift) {
    if (!available(pane)) return;
    const auto index = pane.index(row);
    if (shift) {
        if (!index) return;
        if (pane.anchor < 0) pane.anchor = row;
        if (!ctrl) resetSelection(pane);
        selectRange(pane, pane.anchor, row);
    } else if (ctrl) {
        toggle(pane, row);
    } else {
        resetSelection(pane);
        pane.anchor = index ? row : -1;
        if (index) mark(pane, *index, true);
        redrawSelection(pane);
    }
}

void App::resetSelection(Pane& pane) {
    if (!pane.selectedCount) return;
    if (pane.selectedCount == 1) {
        if (const auto i = pane.index(pane.anchor); i && pane.selected[*i]) {
            mark(pane, *i, false);
            return;
        }
    }
    std::fill(pane.selected.begin(), pane.selected.end(), std::uint8_t{});
    pane.selectedCount = 0;
    pane.selectedBytes = 0;
}

void App::selectSmaller() {
    auto& pane = current();
    if (!available(pane)) return;
    const auto limit = parseNumber(controlText(threshold));
    if (!limit) {
        message(window, tr(L"Enter a nonnegative whole number of bytes.", L"Укажите размер в байтах целым неотрицательным числом."), appName, MB_OK | MB_ICONINFORMATION);
        SetFocus(threshold);
        return;
    }
    const auto& directory = *pane.visible.directory;
    rememberSelection(pane);
    if (SendMessageW(includeSubfolders, BM_GETCHECK, 0, 0) == BST_CHECKED) {
        pane.requestedSort = pane.sort;
        pane.job.start([path = directory.path, limit = *limit, sort = pane.sort](Progress& monitor) {
            return findSmallFiles(path, limit, sort, monitor);
        }, Phase::Search);
        pane.pendingPath = directory.path;
        updatePane(pane);
        updateCommands();
        SetFocus(pane.list);
        return;
    }
    if (directory.recursive) {
        startRead(pane, directory.path, true);
        pane.pendingLimit = *limit;
        SetFocus(pane.list);
        return;
    }
    const auto& entries = directory.entries;
    for (Index i = 0; i < entries.size(); ++i) mark(pane, i, !entries[i].folder() && entries[i].size < *limit);
    pane.anchor = -1;
    redrawSelection(pane);
    SetFocus(pane.list);
}

void App::selectAll() {
    auto& pane = current();
    if (!available(pane)) return;
    rememberSelection(pane);
    for (Index index = 0; index < pane.selected.size(); ++index) mark(pane, index, true);
    pane.anchor = -1;
    redrawSelection(pane);
    SetFocus(pane.list);
}

void App::clearSelection() {
    auto& pane = current();
    if (!available(pane)) return;
    rememberSelection(pane);
    resetSelection(pane);
    pane.anchor = -1;
    redrawSelection(pane);
    SetFocus(pane.list);
}

void App::refresh() {
    auto& pane = current();
    if (!pane.pendingPath.empty()) startRead(pane, pane.pendingPath, true);
    else if (pane.visible.directory) startRead(pane, pane.visible.directory->path, true);
}

void App::refreshPath(Name path) {
    for (auto& pane : panes) {
        const Name displayedPath = pane.pendingPath.empty() && pane.visible.directory
            ? Name(pane.visible.directory->path) : Name(pane.pendingPath);
        if (compareNames(displayedPath, path) == 0) startRead(pane, std::wstring(displayedPath), true);
    }
}

void App::rememberSelection(Pane& pane) {
    pane.previousDirectory = pane.visible.directory;
    pane.previousSelection = pane.selected;
}

void App::changeSelection(Pane& pane, UINT command) {
    if (command == RestoreSelection) {
        if (!pane.previousDirectory) return;
        auto directory = std::exchange(pane.previousDirectory, pane.visible.directory);
        auto selection = std::exchange(pane.previousSelection, pane.selected);
        if (directory == pane.visible.directory) {
            for (Index i = 0; i < selection.size(); ++i) mark(pane, i, selection[i] != 0);
        } else {
            std::unordered_set<Name> names;
            for (Index i = 0; i < selection.size(); ++i) if (selection[i]) names.insert(directory->name(i));
            for (Index i = 0; i < pane.selected.size(); ++i) mark(pane, i, names.contains(pane.visible.directory->name(i)));
        }
    } else if (command == InvertSelection) {
        rememberSelection(pane);
        for (Index i = 0; i < pane.selected.size(); ++i) mark(pane, i, !pane.selected[i]);
    } else {
        const auto mask = prompt(tr(L"Select by mask", L"Выделение по маске"),
                                 tr(L"Wildcard pattern (* and ?)", L"Маска (* и ?)"), L"*");
        if (!mask || mask->empty()) return;
        rememberSelection(pane);
        for (Index i = 0; i < pane.selected.size(); ++i) {
            if (matchesMask(*mask, pane.visible.directory->name(i))) mark(pane, i, command == SelectMask);
        }
    }
    pane.anchor = -1;
    redrawSelection(pane);
}

std::optional<std::wstring> App::prompt(const wchar_t* title, const wchar_t* label, std::wstring value,
                                        PromptPath pathKind, Name base, bool* copyPermissions) {
    PromptDialog dialog{*this, title, label, std::move(value), pathKind, base, copyPermissions};
    const auto resource = copyPermissions ? IDD_COPY_PROMPT :
        pathKind == PromptPath::Text ? IDD_FILE_RENAME : IDD_PATH_PROMPT;
    const auto result = DialogBoxParamW(instance, MAKEINTRESOURCEW(resource), window,
                                        PromptDialog::callback, reinterpret_cast<LPARAM>(&dialog));
    if (result == -1) throw GetLastError();
    if (result == IDOK) return std::move(dialog.value);
    return std::nullopt;
}

void App::showText(std::wstring title, TextFile file, bool editable) {
    TextDialog dialog{*this, std::move(title), std::move(file), editable};
    const auto result = DialogBoxParamW(instance, MAKEINTRESOURCEW(IDD_TEXT_VIEW), window,
                                        TextDialog::callback, reinterpret_cast<LPARAM>(&dialog));
    if (result == -1) throw GetLastError();
    if (result == IDOK && editable) refreshPath(parentPath(dialog.file.path));
}

std::optional<std::wstring> App::destination(Pane& pane, const wchar_t* title) {
    const auto& other = panes[1 - active];
    const auto& source = pane.visible.directory->path;
    const auto initial = other.visible.directory ? other.visible.directory->path : source;
    const auto answer = prompt(title, tr(L"Destination folder", L"Папка назначения"), initial, PromptPath::Folder, source);
    if (!answer) return std::nullopt;
    auto path = absolutePath(*answer, source);
    if (path.empty()) throw static_cast<DWORD>(ERROR_INVALID_NAME);
    return path;
}

std::optional<CopyDestination> App::copyDestination(Name source, std::wstring initial) {
    CopyDestination target;
    const auto answer = prompt(tr(L"Copy", L"Копирование"), tr(L"Destination folder", L"Папка назначения"),
                               std::move(initial), PromptPath::Folder, source, &target.permissions);
    if (!answer) return std::nullopt;
    target.path = absolutePath(*answer, source);
    if (target.path.empty()) throw static_cast<DWORD>(ERROR_INVALID_NAME);
    return target;
}

void App::copyTo(Pane& source, const Pane& target) {
    if (!available(source) || !source.selectedCount) return;
    const auto& path = source.visible.directory->path;
    const auto copy = copyDestination(path, target.visible.directory ? target.visible.directory->path : path);
    if (!copy) return;
    startAction(source, Phase::Copy, {}, copy->path,
        [copy = *copy](const Directory& dir, const auto& items, Progress& progress, Language language) {
            return transferItems(dir, items, copy.path, false, progress, language, copy.permissions);
        });
}

template<class Action>
void App::startAction(Pane& pane, Phase phase, Name sourceToRefresh, Name destinationToRefresh, Action action) {
    if (operation) return;
    operationPath = sourceToRefresh;
    operationDestination = destinationToRefresh;
    renaming = false;
    clipboardSequence = 0;
    if (!sourceToRefresh.empty() || !destinationToRefresh.empty()) rememberSelection(pane);
    auto directory = pane.visible.directory;
    auto selected = pane.orderedSelection();
    operation.start([directory = std::move(directory), selected = std::move(selected),
                     action = std::move(action), language = language](Progress& monitor) {
        return action(*directory, selected, monitor, language);
    }, phase);
    EnableWindow(stop, TRUE);
    updateProgress(activity, progress, *operation.progress, language);
    updateCommands();
}

void App::showContextMenu(Pane& pane, int row, POINT point) {
    if (!available(pane)) return;
    const auto item = pane.index(row);
    if (item && !pane.selected[*item]) clickFile(pane, row, false, false);
    // A click on blank space gives access to folder and selection commands.
    const auto directory = pane.visible.directory;
    const auto single = pane.selectedCount == 1 && item && pane.selected[*item] ? item : std::nullopt;
    std::optional<bool> singleFolder;
    if (single) singleFolder = directory->entries[*single].folder();
    const UINT command = showFileMenu(window, point, item && pane.selected[*item], singleFolder,
                                      static_cast<bool>(pane.previousDirectory), language);
    switch (command) {
        case OpenFolder:
            if (single) enterRow(pane, row);
            break;
        case RenameFile:
            if (single) renameOne(*directory, *single);
            break;
        case BatchRenameFiles:
            openRename();
            break;
        case DeleteFiles:
        case WipeFiles:
            startDelete(command == WipeFiles);
            break;
        case CutFiles:
        case CopyFiles:
            if (const auto error = putFilesOnClipboard(window, *directory, pane.orderedSelection(), command == CutFiles)) {
                throw error;
            }
            break;
        case PasteFiles: {
            auto data = filesFromClipboard(window);
            if (data.paths.empty()) break;
            CopyDestination copy{directory->path};
            if (!data.cut) {
                const auto target = copyDestination(directory->path, directory->path);
                if (!target) break;
                copy = *target;
            }
            const auto& other = panes[1 - active];
            const Name sourceToRefresh = data.cut && other.visible.directory ? Name(other.visible.directory->path) : Name{};
            const DWORD sequence = data.cut ? data.sequence : 0;
            const auto phase = data.cut ? Phase::Move : Phase::Copy;
            startAction(pane, phase, sourceToRefresh, copy.path,
                [data = std::move(data), copy](const Directory&, const auto&, Progress& progress, Language language) {
                    return pasteItems(data.paths, copy.path, data.cut, progress, language, copy.permissions);
                });
            clipboardSequence = sequence;
            break;
        }
        case CopyItems:
            copyTo(pane, panes[1 - active]);
            break;
        case MoveItems:
        case HardLinkItems:
        case SymbolicLinkItems:
        case JunctionItems:
        case ExtractZip: {
            const bool move = command == MoveItems;
            const auto title = move ? tr(L"Move", L"Перемещение") :
                command == ExtractZip ? tr(L"Extract ZIP", L"Распаковать ZIP") : tr(L"Create link", L"Создать ссылку");
            const auto target = destination(pane, title);
            if (!target) break;
            const auto phase = move ? Phase::Move :
                command == ExtractZip ? Phase::Archive : Phase::Change;
            startAction(pane, phase, move ? Name(directory->path) : Name{}, *target,
                [command, target = *target](const Directory& dir, const auto& items, Progress& progress, Language language) {
                    if (command == MoveItems) {
                        return transferItems(dir, items, target, true, progress, language);
                    }
                    if (command == ExtractZip) return processArchives(dir, items, target, true, progress, language);
                    const auto kind = command == HardLinkItems ? LinkKind::Hard :
                        command == JunctionItems ? LinkKind::Junction : LinkKind::Symbolic;
                    return createLinks(dir, items, target, kind, progress, language);
                });
            break;
        }
        case MakeFolder: {
            const auto name = prompt(tr(L"Make folder", L"Создать папку"), tr(L"Folder name", L"Имя папки"), {});
            if (!name) break;
            if (!validFileName(*name)) throw static_cast<DWORD>(ERROR_INVALID_NAME);
            if (!CreateDirectoryW(nativePath(childPath(directory->path, *name)).c_str(), nullptr)) throw GetLastError();
            refreshPath(directory->path);
            break;
        }
        case ViewItems:
        case EditFile:
        case FileProperties:
            if (single && !*singleFolder && command != FileProperties) {
                auto file = loadTextFile(childPath(directory->path, directory->name(*single)));
                const bool edit = command == EditFile;
                if (edit && (file.binary || file.truncated)) throw static_cast<DWORD>(ERROR_NOT_SUPPORTED);
                auto title = std::wstring(directory->name(*single));
                if (file.binary) title += tr(L" — Hex preview", L" — Шестнадцатеричный просмотр");
                if (file.truncated) title += tr(L" (partial)", L" (фрагмент)");
                showText(std::move(title), std::move(file), edit);
            } else {
                startAction(pane, Phase::Read, {}, {},
                    [](const Directory& dir, const auto& items, Progress& progress, Language language) {
                        const auto info = inspectItems(dir, items, progress);
                        OperationResult result;
                        const auto tr = [language](const wchar_t* en, const wchar_t* ru) { return translated(language, en, ru); };
                        result.cancelled = progress.cancel;
                        if (!result.cancelled) {
                            result.information = dir.path + L"\r\n\r\n" +
                                tr(L"Files ", L"Файлов ") + number(info.files) + L"\r\n" +
                                tr(L"Folders ", L"Папок ") + number(info.folders) + L"\r\n" +
                                tr(L"Links ", L"Ссылок ") + number(info.links) + L"\r\n" +
                                tr(L"Bytes ", L"Байт ") + number(info.bytes);
                            if (items.size() == 1) {
                                result.information = std::wstring(dir.name(items.front())) + L"\r\n" + result.information;
                                WIN32_FILE_ATTRIBUTE_DATA data{};
                                const auto path = nativePath(childPath(dir.path, dir.name(items.front())));
                                FILETIME local{};
                                SYSTEMTIME time{};
                                if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data) &&
                                    FileTimeToLocalFileTime(&data.ftLastWriteTime, &local) && FileTimeToSystemTime(&local, &time)) {
                                    wchar_t date[32]{};
                                    swprintf(date, std::size(date), L"%04u-%02u-%02u %02u:%02u:%02u",
                                        time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond);
                                    result.information += L"\r\n";
                                    result.information += tr(L"Modified ", L"Изменён ");
                                    result.information += date;
                                    result.information += L"\r\n";
                                    result.information += tr(L"Attributes ", L"Атрибуты ");
                                    if (data.dwFileAttributes & FILE_ATTRIBUTE_READONLY) result.information += L"R ";
                                    if (data.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) result.information += L"H ";
                                    if (data.dwFileAttributes & FILE_ATTRIBUTE_SYSTEM) result.information += L"S ";
                                    if (data.dwFileAttributes & FILE_ATTRIBUTE_ARCHIVE) result.information += L"A ";
                                    if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) result.information += L"D ";
                                    if (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) result.information += L"L ";
                                    if (data.dwFileAttributes == FILE_ATTRIBUTE_NORMAL) result.information += tr(L"Normal", L"Обычный");
                                }
                            }
                        }
                        return result;
                    });
            }
            break;
        case FileAttributes: {
            const auto input = prompt(tr(L"Change attributes", L"Изменить атрибуты"),
                tr(L"Use + or − with R, H, S, A, N, T, O. Example +R -H", L"Укажите + или − перед R, H, S, A, N, T, O. Пример +R -H"), {});
            if (!input || input->empty()) break;
            DWORD set = 0, clear = 0;
            Name rules(*input);
            while (!rules.empty()) {
                if (iswspace(rules.front())) { rules.remove_prefix(1); continue; }
                if (rules.size() < 2 || (rules[0] != L'+' && rules[0] != L'-' && rules[0] != L'−')) {
                    throw static_cast<DWORD>(ERROR_INVALID_PARAMETER);
                }
                DWORD flag = 0;
                switch (towupper(rules[1])) {
                    case L'R': flag = FILE_ATTRIBUTE_READONLY; break;
                    case L'H': flag = FILE_ATTRIBUTE_HIDDEN; break;
                    case L'S': flag = FILE_ATTRIBUTE_SYSTEM; break;
                    case L'A': flag = FILE_ATTRIBUTE_ARCHIVE; break;
                    case L'N': flag = FILE_ATTRIBUTE_NOT_CONTENT_INDEXED; break;
                    case L'T': flag = FILE_ATTRIBUTE_TEMPORARY; break;
                    case L'O': flag = FILE_ATTRIBUTE_OFFLINE; break;
                    default: throw static_cast<DWORD>(ERROR_INVALID_PARAMETER);
                }
                if (rules[0] == L'+') { set |= flag; clear &= ~flag; } else { clear |= flag; set &= ~flag; }
                rules.remove_prefix(2);
            }
            if (set || clear) startAction(pane, Phase::Change, directory->path, {},
                [set, clear](const Directory& dir, const auto& items, Progress& progress, Language language) {
                    return changeAttributes(dir, items, set, clear, progress, language);
                });
            break;
        }
        case ModifiedTime:
        case CreatedTime:
        case AccessedTime:
        case ChangedTime: {
            const auto kind = command == CreatedTime ? TimeKind::Created : command == AccessedTime ? TimeKind::Accessed :
                command == ChangedTime ? TimeKind::Changed : TimeKind::Modified;
            const auto input = prompt(tr(L"Set file time", L"Установить время"), L"YYYY-MM-DD HH:MM:SS", {});
            if (!input) break;
            const auto time = parseLocalTime(*input);
            if (!time) throw static_cast<DWORD>(ERROR_INVALID_DATA);
            startAction(pane, Phase::Change, directory->path, {},
                [kind, time = *time](const Directory& dir, const auto& items, Progress& progress, Language language) {
                    return changeTime(dir, items, time, kind, progress, language);
                });
            break;
        }
        case ApplyCommand:
        case DescribeItems: {
            const bool execute = command == ApplyCommand;
            const auto input = execute ?
                prompt(tr(L"Apply command", L"Выполнить команду"),
                       tr(L"Program and arguments. {} inserts the item path.", L"Программа и аргументы. {} подставляет путь."), {}) :
                prompt(tr(L"Describe items", L"Описание объектов"),
                       tr(L"Description (empty removes it)", L"Описание (пустое значение удаляет его)"), {});
            if (!input || (execute && input->find_first_not_of(L" \t") == Name::npos)) break;
            startAction(pane, execute ? Phase::Run : Phase::Change, directory->path, {},
                [execute, input = *input](const Directory& dir, const auto& items, Progress& progress, Language language) {
                    if (execute) return applyCommand(dir, items, input, progress, language);
                    return describeItems(dir, items, input, progress, language);
                });
            break;
        }
        case PackZip: {
            const auto& other = panes[1 - active];
            const auto initial = other.visible.directory ? other.visible.directory->path : directory->path;
            const auto input = prompt(tr(L"Create ZIP", L"Создать ZIP"), tr(L"New archive path", L"Путь к новому архиву"),
                                      childPath(initial, L"archive.zip"), PromptPath::File, directory->path);
            if (!input) break;
            const auto path = absolutePath(*input, directory->path);
            if (path.empty() || !validFileName(Name(path).substr(path.find_last_of(L'\\') + 1))) {
                throw static_cast<DWORD>(ERROR_INVALID_NAME);
            }
            startAction(pane, Phase::Archive, {}, parentPath(path),
                [path](const Directory& dir, const auto& items, Progress& progress, Language language) {
                    return createArchive(dir, items, path, progress, language);
                });
            break;
        }
        case ListZip:
            if (single) {
                const auto path = childPath(directory->path, directory->name(*single));
                startAction(pane, Phase::Archive, {}, {},
                    [path](const Directory&, const auto&, Progress&, Language language) {
                        OperationResult result;
                        result.information = listArchive(path, language);
                        return result;
                    });
            }
            break;
        case TestZip:
            startAction(pane, Phase::Archive, {}, {},
                [](const Directory& dir, const auto& items, Progress& progress, Language language) {
                    return processArchives(dir, items, {}, false, progress, language);
                });
            break;
        case SelectMask:
        case DeselectMask:
        case InvertSelection:
        case RestoreSelection:
            changeSelection(pane, command);
            break;
        default:
            break;
    }
}

void App::renameOne(const Directory& directory, Index file) {
    FileRenameDialog dialog{*this, directory, file};
    const auto result = DialogBoxParamW(instance, MAKEINTRESOURCEW(IDD_FILE_RENAME), window,
                                        FileRenameDialog::callback, reinterpret_cast<LPARAM>(&dialog));
    if (result == IDOK) {
        lastStatus = tr(L"Renamed 1", L"Переименовано 1");
        refreshPath(directory.path);
    } else if (result == -1) {
        message(window, windowsError(GetLastError(), language).c_str(), appName, MB_OK | MB_ICONERROR);
    }
}

void App::goUp(Pane& pane) {
    if (!available(pane)) return;
    const auto& dir = *pane.visible.directory;
    if (dir.recursive) startRead(pane, dir.path, true);
    else startRead(pane, parentPath(dir.path));
    SetFocus(pane.list);
}

void App::enterRow(Pane& pane, int row) {
    if (!available(pane)) return;
    if (row == 0 && pane.parentRow) return goUp(pane);
    const auto index = pane.index(row);
    if (index && pane.visible.directory->entries[*index].folder()) {
        startRead(pane, childPath(pane.visible.directory->path, pane.visible.directory->name(*index)));
    }
}

void App::goToPath(Pane& pane) {
    const Name base = pane.visible.directory ? Name(pane.visible.directory->path) : Name{};
    auto path = absolutePath(controlText(pane.path), base);
    if (path.empty()) {
        message(window, tr(L"Enter a folder path, for example C:\\Data.", L"Укажите путь к папке, например C:\\Данные."), appName, MB_OK | MB_ICONINFORMATION);
        return;
    }
    startRead(pane, std::move(path));
    SetFocus(pane.list);
}

void App::openRename() {
    auto& pane = current();
    if (!available(pane) || !pane.selectedCount) return;
    auto files = std::make_shared<const std::vector<Index>>(pane.orderedSelection());
    RenameDialog dialog{*this, pane.visible.directory, std::move(files), previousRename};
    renameDialog = &dialog;
    const auto result = DialogBoxParamW(instance, MAKEINTRESOURCEW(IDD_RENAME), window,
                                        RenameDialog::callback, reinterpret_cast<LPARAM>(&dialog));
    renameDialog = nullptr;
    if (result == -1) {
        message(window, windowsError(GetLastError(), language).c_str(), appName, MB_OK | MB_ICONERROR);
    }
    SetFocus(pane.list);
}

void App::startRename(const RenameOptions& options) {
    if (operation || !renameDialog) return;
    auto directory = renameDialog->directory;
    auto files = renameDialog->files;
    previousRename = options;
    rememberSelection(current());
    operationPath = directory->path;
    operationDestination.clear();
    clipboardSequence = 0;
    operation.start([directory, files, options, language = language](Progress& monitor) {
        return renameFiles(*directory, *files, options, monitor, language);
    }, Phase::Validate);
    renaming = true;
    renameDialog->setWorking(true);
    updateCommands();
}

void App::startDelete(bool wipe) {
    auto& pane = current();
    if (!available(pane) || !pane.selectedCount) return;
    const auto path = pane.visible.directory->path;
    const auto items = number(pane.selectedCount);
    auto question = (wipe ? tr(L"Overwrite selected files with zeros and permanently delete the selected items (",
                               L"Перезаписать выделенные файлы нулями и удалить выбранные объекты (")
                          : tr(L"Permanently delete the selected items (", L"Удалить без корзины выбранные объекты (")) +
        items + L")?\n\n" + path;
    if (std::any_of(pane.visible.order.begin(), pane.visible.order.end(),
                    [&](Index index) { return pane.selected[index] && pane.visible.directory->entries[index].folder(); })) {
        question += tr(L"\n\nThis includes all contents of the selected folders.",
                       L"\n\nБудет удалено всё содержимое выбранных папок.");
    }
    if (message(window, question.c_str(), wipe ? tr(L"Wipe items", L"Затереть объекты") : tr(L"Delete items", L"Удаление объектов"),
                    MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) return;
    startAction(pane, wipe ? Phase::Wipe : Phase::Delete, path, {},
        [wipe](const Directory& dir, const auto& items, Progress& progress, Language language) {
            return deleteFiles(dir, items, progress, language, wipe);
        });
}

void App::consumeOperation() {
    if (!operation.ready()) return;
    auto future = std::move(operation.future);
    const auto phase = operation.progress->phase.load();
    OperationResult result;
    std::wstring error;
    try {
        result = future.get();
    } catch (const std::bad_alloc&) {
        error = tr(L"Out of memory.", L"Недостаточно памяти.");
    } catch (DWORD code) {
        error = windowsError(code, language);
    } catch (...) {
        error = tr(L"The operation stopped because of an unexpected error.", L"Операция прервана из-за непредвиденной ошибки.");
    }
    const bool editing = renaming && renameDialog && (!result.rejected.empty() || (phase == Phase::Validate && !error.empty()));
    if (editing) {
        renameDialog->issue = result.rejected.empty() ? error : result.rejected;
        renameDialog->setWorking(false);
        message(renameDialog->window, renameDialog->issue.c_str(), appName, MB_OK | MB_ICONWARNING);
        updateCommands();
        return;
    }

    const wchar_t* completed = tr(L"Processed ", L"Обработано ");
    switch (phase) {
        case Phase::Rename: completed = tr(L"Renamed ", L"Переименовано "); break;
        case Phase::Delete: completed = tr(L"Deleted ", L"Удалено "); break;
        case Phase::Wipe: completed = tr(L"Wiped ", L"Затёрто "); break;
        case Phase::Copy: completed = tr(L"Copied ", L"Скопировано "); break;
        case Phase::Move: completed = tr(L"Moved ", L"Перемещено "); break;
        default: break;
    }
    lastStatus = completed + number(result.changed);
    if (result.failed) lastStatus += tr(L"   ·   Errors ", L"   ·   Ошибок ") + number(result.failed);
    if (result.cancelled) lastStatus += tr(L"   ·   Stopped", L"   ·   Остановлено");
    if (!error.empty()) lastStatus = error;
    if (renaming && renameDialog) EndDialog(renameDialog->window, IDOK);
    const bool refresh = result.changed || result.cancelled ||
                         (phase != Phase::Validate && (!error.empty() || result.failed));
    renaming = false;
    if (closeAfterOperation) {
        DestroyWindow(window);
        return;
    }
    if (clipboardSequence && result.changed && !result.failed && !result.cancelled && error.empty()) {
        clearFileClipboard(window, clipboardSequence);
    }
    clipboardSequence = 0;
    if (!result.information.empty()) {
        lastStatus.clear();
        updateCommands();
        TextFile text;
        text.text = std::move(result.information);
        showText(phase == Phase::Archive ? tr(L"ZIP contents", L"Содержимое ZIP") : tr(L"Properties", L"Свойства"),
                 std::move(text), false);
        return;
    }
    if (refresh) {
        if (!operationPath.empty()) refreshPath(operationPath);
        if (!operationDestination.empty() && compareNames(operationPath, operationDestination) != 0) {
            refreshPath(operationDestination);
        }
    }
    if (result.failed || !error.empty()) {
        notice = lastStatus;
        for (const auto& item : result.errors) notice += L"\n\n" + item;
        PostMessageW(window, operationNotice, 0, 0);
    }
    updateCommands();
}

bool App::key(HWND hwnd, UINT keyCode) {
    if (dragSource >= 0 && keyCode == VK_ESCAPE) {
        finishDrag(false);
        return true;
    }
    const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    const bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
    auto& pane = current();
    if (ctrl && keyCode == 'M') {
        openRename();
        return true;
    }
    if (alt && keyCode == 'D') {
        if (!operation) {
            SetFocus(pane.path);
            SendMessageW(pane.path, EM_SETSEL, 0, -1);
        }
        return true;
    }
    if (keyCode == VK_F5 && !operation) {
        refresh();
        return true;
    }
    if (keyCode == VK_ESCAPE && operation && !renaming) {
        operation.progress->cancel = true;
        return true;
    }
    if (keyCode == VK_RETURN && hwnd == pane.path && !operation) {
        goToPath(pane);
        return true;
    }
    if (keyCode == VK_RETURN && hwnd == threshold) {
        selectSmaller();
        return true;
    }
    if (hwnd != pane.list) return false;

    if (keyCode == VK_TAB) {
        SetFocus(panes[1 - active].list);
        return true;
    }
    if (!available(pane)) return false;
    const int row = ListView_GetNextItem(pane.list, -1, LVNI_FOCUSED);
    if (keyCode == VK_APPS || (shift && keyCode == VK_F10)) {
        RECT rect{};
        if (row >= 0 && ListView_GetItemRect(pane.list, row, &rect, LVIR_LABEL)) {
            POINT point{rect.left, rect.bottom};
            ClientToScreen(pane.list, &point);
            showContextMenu(pane, row, point);
        }
        return true;
    }
    if (keyCode == VK_BACK) { goUp(pane); return true; }
    if (keyCode == VK_RETURN) { enterRow(pane, row); return true; }
    if (keyCode == VK_DELETE || keyCode == VK_F8) { startDelete(); return true; }
    if (keyCode == VK_ESCAPE) { clearSelection(); return true; }
    if (keyCode == VK_SPACE || keyCode == VK_INSERT) {
        toggle(pane, row);
        if (keyCode == VK_INSERT && row >= 0 && row + 1 < pane.count()) {
            ListView_SetItemState(pane.list, row + 1, LVIS_FOCUSED | LVIS_SELECTED, LVIS_FOCUSED | LVIS_SELECTED);
            ListView_EnsureVisible(pane.list, row + 1, FALSE);
        }
        return true;
    }
    if (ctrl && keyCode == 'A') {
        selectAll();
        return true;
    }
    if (shift && row >= 0 && (keyCode == VK_UP || keyCode == VK_DOWN || keyCode == VK_HOME || keyCode == VK_END)) {
        if (pane.anchor < 0) pane.anchor = row;
        int next = row;
        if (keyCode == VK_HOME) next = 0;
        if (keyCode == VK_END) next = pane.count() - 1;
        if (keyCode == VK_UP) next = std::max(0, row - 1);
        if (keyCode == VK_DOWN) next = std::min(pane.count() - 1, row + 1);
        selectRange(pane, row, next);
        ListView_SetItemState(pane.list, next, LVIS_FOCUSED | LVIS_SELECTED, LVIS_FOCUSED | LVIS_SELECTED);
        ListView_EnsureVisible(pane.list, next, FALSE);
        return true;
    }
    return false;
}

LRESULT App::notify(NMHDR& header) {
    for (int side = 0; side < 2; ++side) {
        auto& pane = panes[side];
        if (header.hwndFrom != pane.list) continue;
        if (header.code == LVN_GETDISPINFOW) {
            auto& info = reinterpret_cast<NMLVDISPINFOW&>(header);
            const auto index = pane.index(info.item.iItem);
            if (info.item.iSubItem == 0) {
                copyText(info, index ? (pane.visible.directory->recursive ? pane.visible.directory->name(*index)
                                                                        : pane.visible.directory->stem(*index))
                                     : (pane.parentRow && info.item.iItem == 0 ? L".." : L""));
            } else if (index && info.item.iSubItem == 1) {
                copyText(info, pane.visible.directory->entries[*index].folder()
                               ? tr(L"Folder", L"Папка") : pane.visible.directory->extension(*index));
            } else if (index && info.item.iSubItem == 2 && !pane.visible.directory->entries[*index].folder()) {
                copyText(info, number(pane.visible.directory->entries[*index].size));
            } else {
                copyText(info, L"");
            }
            return 0;
        }
        if (header.code == NM_CUSTOMDRAW) {
            auto& draw = reinterpret_cast<NMLVCUSTOMDRAW&>(header);
            if (draw.nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
            if (draw.nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
                const int row = static_cast<int>(draw.nmcd.dwItemSpec);
                const auto index = pane.index(row);
                draw.clrText = index && pane.selected[*index] ? marked : ink;
                draw.clrTextBk = row % 2 ? RGB(247, 250, 250) : RGB(255, 255, 255);
                if (draw.nmcd.uItemState & CDIS_SELECTED) {
                    draw.nmcd.uItemState &= ~(CDIS_SELECTED | CDIS_FOCUS);
                    draw.clrTextBk = side == active ? activeTint : RGB(230, 235, 237);
                }
                return CDRF_NEWFONT;
            }
            return CDRF_DODEFAULT;
        }
        if (header.code == NM_SETFOCUS) setActive(side);
        if (header.code == LVN_BEGINDRAG && dragSource < 0 && available(pane)) {
            const int row = reinterpret_cast<NMLISTVIEW&>(header).iItem;
            if (const auto index = pane.index(row)) {
                setActive(side);
                if (!pane.selected[*index]) clickFile(pane, row, false, false);
                // The list view can release its own capture after this notification.
                PostMessageW(window, beginDragCapture, side, 0);
            }
            return 0;
        }
        if (header.code == LVN_COLUMNCLICK) {
            setActive(side);
            sortBy(pane, reinterpret_cast<NMLISTVIEW&>(header).iSubItem);
        }
        if (header.code == NM_CLICK || header.code == NM_RCLICK || header.code == NM_DBLCLK) {
            setActive(side);
            const int row = reinterpret_cast<NMITEMACTIVATE&>(header).iItem;
            if (header.code == NM_DBLCLK) enterRow(pane, row);
            else if (header.code == NM_RCLICK) {
                const auto position = GetMessagePos();
                showContextMenu(pane, row, POINT{GET_X_LPARAM(position), GET_Y_LPARAM(position)});
                return TRUE;
            } else clickFile(pane, row, (GetKeyState(VK_CONTROL) & 0x8000) != 0,
                                   (GetKeyState(VK_SHIFT) & 0x8000) != 0);
        }
        return 0;
    }
    return 0;
}

bool App::canDrop(POINT screenPoint) const {
    if (dragSource < 0 || !available(panes[dragSource])) return false;
    const auto& target = panes[1 - dragSource];
    if (!available(target) ||
        compareNames(panes[dragSource].visible.directory->path, target.visible.directory->path) == 0) return false;
    RECT area{};
    GetWindowRect(target.list, &area);
    return PtInRect(&area, screenPoint) != FALSE;
}

void App::finishDrag(bool copy) {
    const int source = std::exchange(dragSource, -1);
    if (source < 0) return;
    if (GetCapture() == window) ReleaseCapture();
    SetCursor(LoadCursorW(nullptr, IDC_ARROW));
    if (copy) copyTo(panes[source], panes[1 - source]);
}

LRESULT App::handle(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_CREATE:
            create();
            return 0;
        case WM_SIZE:
            if (wParam != SIZE_MINIMIZED) layout();
            return 0;
        case WM_GETMINMAXINFO: {
            auto& info = *reinterpret_cast<MINMAXINFO*>(lParam);
            info.ptMinTrackSize = {px(860), px(355)};
            return 0;
        }
        case WM_DPICHANGED: {
            dpi = HIWORD(wParam);
            rebuildFont();
            const auto& rc = *reinterpret_cast<RECT*>(lParam);
            SetWindowPos(window, nullptr, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            layout();
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(window, &paint);
            FillRect(dc, &paint.rcPaint, background);
            RECT rc{};
            GetClientRect(window, &rc);
            HBRUSH strip = CreateSolidBrush(accent);
            RECT top{0, 0, rc.right, px(4)};
            FillRect(dc, &top, strip);
            const int margin = px(12);
            const int gap = px(8);
            const int firstWidth = (rc.right - margin * 2 - gap) / 2;
            const int x = margin + active * (firstWidth + gap);
            const int end = active ? rc.right - margin : x + firstWidth;
            RECT pathBand{x, panelY - px(2), end, panelY + px(35)};
            FillRect(dc, &pathBand, statusTint);
            RECT statusBand{x, rc.bottom - px(64), end, rc.bottom - px(37)};
            FillRect(dc, &statusBand, statusTint);
            DeleteObject(strip);
            EndPaint(window, &paint);
            return 0;
        }
        case WM_CTLCOLORSTATIC: {
            const bool tinted = reinterpret_cast<HWND>(lParam) == current().status;
            const auto dc = reinterpret_cast<HDC>(wParam);
            SetTextColor(dc, ink);
            SetBkColor(dc, tinted ? activeTint : canvas);
            return reinterpret_cast<LRESULT>(tinted ? statusTint : background);
        }
        case WM_TIMER:
            for (auto& pane : panes) {
                if (pane.job.ready()) consumeListing(pane);
                else if (pane.job) updatePane(pane);
            }
            if (operation.ready()) consumeOperation();
            else if (operation) {
                if (renaming && renameDialog) updateProgress(renameDialog->status, renameDialog->bar, *operation.progress, language);
                else updateProgress(activity, progress, *operation.progress, language);
            }
            return 0;
        case WM_NOTIFY:
            return notify(*reinterpret_cast<NMHDR*>(lParam));
        case beginDragCapture: {
            const auto side = static_cast<int>(wParam);
            if (dragSource >= 0 || !available(panes[side])) return 0;
            dragSource = side;
            POINT point{};
            GetCursorPos(&point);
            if (GetAsyncKeyState(VK_LBUTTON) & 0x8000) {
                SetCapture(window);
                SetCursor(LoadCursorW(nullptr, canDrop(point) ? IDC_ARROW : IDC_NO));
            } else {
                finishDrag(canDrop(point));
            }
            return 0;
        }
        case WM_MOUSEMOVE:
        case WM_LBUTTONUP:
            if (dragSource >= 0) {
                POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                ClientToScreen(window, &point);
                const bool drop = canDrop(point);
                if (message == WM_LBUTTONUP) finishDrag(drop);
                else SetCursor(LoadCursorW(nullptr, drop ? IDC_ARROW : IDC_NO));
                return 0;
            }
            break;
        case WM_CANCELMODE:
        case WM_CAPTURECHANGED:
            finishDrag(false);
            break;
        case WM_COMMAND: {
            const int id = LOWORD(wParam);
            const int reason = HIWORD(wParam);
            if (id >= firstPanelId && id < firstPanelId + 20) {
                const int side = (id - firstPanelId) / 10;
                const auto control = (id - firstPanelId) % 10;
                auto& pane = panes[side];
                if ((control == Drive && reason == CBN_SETFOCUS) || (control == Path && reason == EN_SETFOCUS)) {
                    setActive(side);
                }
                if (control == Drive && reason == CBN_SELENDOK && !operation) {
                    const auto drive = SendMessageW(pane.drive, CB_GETCURSEL, 0, 0);
                    if (drive != CB_ERR) {
                        wchar_t root[8]{};
                        SendMessageW(pane.drive, CB_GETLBTEXT, drive, reinterpret_cast<LPARAM>(root));
                        setActive(side);
                        startRead(pane, root);
                        PostMessageW(window, WM_NEXTDLGCTL, reinterpret_cast<WPARAM>(pane.list), TRUE);
                    }
                }
                if (control == Up && reason == BN_CLICKED) { setActive(side); goUp(pane); }
                return 0;
            }
            if (id == LanguageChoice && !operation &&
                (reason == CBN_SELENDOK ||
                 (reason == CBN_SELCHANGE && !SendMessageW(languageChoice, CB_GETDROPPEDSTATE, 0, 0)))) {
                changeLanguage();
            }
            if (id == SelectSmall && reason == BN_CLICKED) selectSmaller();
            if (id == SelectAll && reason == BN_CLICKED) selectAll();
            if (id == BatchRename && reason == BN_CLICKED) openRename();
            if (id == Refresh && reason == BN_CLICKED) {
                refresh();
                SetFocus(current().list);
            }
            if (id == ClearSelected && reason == BN_CLICKED) clearSelection();
            if (id == DeleteSelected && reason == BN_CLICKED) startDelete();
            if (id == StopOperation && reason == BN_CLICKED && operation) {
                operation.progress->cancel = true;
                EnableWindow(stop, FALSE);
            }
            return 0;
        }
        case WM_NEXTDLGCTL:
            if (lParam) SetFocus(reinterpret_cast<HWND>(wParam));
            return 0;
        case operationNotice:
            if (!notice.empty()) {
                auto messageText = std::exchange(notice, {});
                this->message(window, messageText.c_str(), appName, MB_OK | MB_ICONWARNING);
            }
            return 0;
        case WM_CLOSE:
            if (operation) {
                operation.progress->cancel = true;
                closeAfterOperation = true;
                EnableWindow(stop, FALSE);
                return 0;
            }
            DestroyWindow(window);
            return 0;
        case WM_DESTROY:
            KillTimer(window, 1);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

LRESULT CALLBACK App::callback(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        auto& app = *static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        app.window = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&app));
    }
    if (auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA))) {
        try {
            return app->handle(message, wParam, lParam);
        } catch (DWORD error) {
            app->message(hwnd, windowsError(error, app->language).c_str(), appName, MB_OK | MB_ICONERROR);
            return message == WM_CREATE ? -1 : 0;
        } catch (...) {
            app->message(hwnd, app->tr(L"Could not complete the command.", L"Не удалось выполнить команду."),
                         appName, MB_OK | MB_ICONERROR);
            return message == WM_CREATE ? -1 : 0;
        }
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT CALLBACK App::childProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam,
                               UINT_PTR, DWORD_PTR context) {
    auto& app = *reinterpret_cast<App*>(context);
    if (message == WM_KEYDOWN || message == WM_SYSKEYDOWN) {
        try {
            if (app.key(hwnd, static_cast<UINT>(wParam))) return 0;
        } catch (DWORD error) {
            app.message(app.window, windowsError(error, app.language).c_str(), appName, MB_OK | MB_ICONERROR);
            return 0;
        } catch (...) {
            app.message(app.window, app.tr(L"Could not complete the command.", L"Не удалось выполнить команду."),
                        appName, MB_OK | MB_ICONERROR);
            return 0;
        }
    }
    if (message == WM_CHAR && (wParam == VK_RETURN || wParam == VK_ESCAPE || wParam == VK_TAB)) {
        const auto& pane = app.current();
        if (hwnd == pane.list || hwnd == pane.path || hwnd == app.threshold) return 0;
    }
    return DefSubclassProc(hwnd, message, wParam, lParam);
}

INT_PTR CALLBACK PromptDialog::callback(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_INITDIALOG) SetWindowLongPtrW(hwnd, DWLP_USER, lParam);
    auto* dialog = reinterpret_cast<PromptDialog*>(GetWindowLongPtrW(hwnd, DWLP_USER));
    if (!dialog) return FALSE;
    auto& app = dialog->app;
    if (message == WM_INITDIALOG) {
        SetWindowTextW(hwnd, dialog->title);
        SetDlgItemTextW(hwnd, IDC_FILENAME_LABEL, dialog->label);
        SetDlgItemTextW(hwnd, IDOK, L"OK");
        SetDlgItemTextW(hwnd, IDCANCEL, app.tr(L"Cancel", L"Отмена"));
        if (dialog->pathKind != PromptPath::Text) SetDlgItemTextW(hwnd, IDC_BROWSE_FOLDER, app.tr(L"Browse…", L"Обзор…"));
        if (dialog->copyPermissions) {
            SetDlgItemTextW(hwnd, IDC_COPY_PERMISSIONS,
                           app.tr(L"Copy NTFS permissions", L"Копировать права NTFS"));
        }
        const auto input = GetDlgItem(hwnd, IDC_FILENAME);
        SendMessageW(input, EM_LIMITTEXT, 32760, 0);
        SetWindowTextW(input, dialog->value.c_str());
        SetFocus(input);
        SendMessageW(input, EM_SETSEL, 0, -1);
        return FALSE;
    }
    if (message == WM_COMMAND && LOWORD(wParam) == IDC_BROWSE_FOLDER && dialog->pathKind != PromptPath::Text) {
        const auto input = GetDlgItem(hwnd, IDC_FILENAME);
        try {
            auto initial = absolutePath(controlText(input), dialog->base);
            std::wstring filename;
            if (dialog->pathKind == PromptPath::File) {
                const auto parent = parentPath(initial);
                filename = parent.empty() ? L"archive.zip" : initial.substr(initial.find_last_of(L'\\') + 1);
                initial = parent;
            }
            if (initial.empty()) initial = dialog->base;
            if (auto folder = chooseFolder(app.instance, hwnd, std::move(initial), app.language)) {
                if (dialog->pathKind == PromptPath::File) *folder = childPath(*folder, filename);
                SetWindowTextW(input, folder->c_str());
                SendMessageW(input, EM_SETSEL, static_cast<WPARAM>(-1), 0);
                SetFocus(input);
            }
        } catch (DWORD error) {
            app.message(hwnd, windowsError(error, app.language).c_str(), appName, MB_OK | MB_ICONERROR);
        } catch (...) {
            app.message(hwnd, app.tr(L"Could not open the folder picker.", L"Не удалось открыть выбор папки."),
                        appName, MB_OK | MB_ICONERROR);
        }
        return TRUE;
    }
    if (message == WM_COMMAND && LOWORD(wParam) == IDOK) {
        try {
            dialog->value = controlText(GetDlgItem(hwnd, IDC_FILENAME));
            if (dialog->copyPermissions) {
                *dialog->copyPermissions = IsDlgButtonChecked(hwnd, IDC_COPY_PERMISSIONS) == BST_CHECKED;
            }
            EndDialog(hwnd, IDOK);
        } catch (...) { EndDialog(hwnd, IDCANCEL); }
        return TRUE;
    }
    if (message == WM_CLOSE || (message == WM_COMMAND && LOWORD(wParam) == IDCANCEL)) {
        EndDialog(hwnd, IDCANCEL);
        return TRUE;
    }
    return FALSE;
}

INT_PTR CALLBACK TextDialog::callback(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_INITDIALOG) SetWindowLongPtrW(hwnd, DWLP_USER, lParam);
    auto* dialog = reinterpret_cast<TextDialog*>(GetWindowLongPtrW(hwnd, DWLP_USER));
    if (!dialog) return FALSE;
    auto& app = dialog->app;
    const auto input = GetDlgItem(hwnd, IDC_DOCUMENT_TEXT);
    try {
        if (message == WM_INITDIALOG) {
            SetWindowTextW(hwnd, dialog->title.c_str());
            SetDlgItemTextW(hwnd, IDOK, app.tr(L"Save", L"Сохранить"));
            SetDlgItemTextW(hwnd, IDCANCEL, app.tr(L"Close", L"Закрыть"));
            ShowWindow(GetDlgItem(hwnd, IDOK), dialog->editable ? SW_SHOW : SW_HIDE);
            SendMessageW(input, EM_LIMITTEXT, 16 * 1024 * 1024, 0);
            SendMessageW(input, EM_SETREADONLY, !dialog->editable, 0);
            SetWindowTextW(input, dialog->file.text.c_str());
            SendMessageW(input, EM_SETMODIFY, FALSE, 0);
            SetFocus(input);
            SendMessageW(input, EM_SETSEL, 0, 0);
            return FALSE;
        }
        if (message == WM_COMMAND && LOWORD(wParam) == IDOK && dialog->editable) {
            const auto error = saveTextFile(dialog->file, controlText(input));
            if (error) throw error;
            EndDialog(hwnd, IDOK);
            return TRUE;
        }
        if (message == WM_CLOSE || (message == WM_COMMAND && LOWORD(wParam) == IDCANCEL)) {
            if (dialog->editable && SendMessageW(input, EM_GETMODIFY, 0, 0) &&
                app.message(hwnd, app.tr(L"Close without saving changes?", L"Закрыть без сохранения изменений?"),
                            appName, MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) return TRUE;
            EndDialog(hwnd, IDCANCEL);
            return TRUE;
        }
    } catch (DWORD error) {
        app.message(hwnd, windowsError(error, app.language).c_str(), appName, MB_OK | MB_ICONERROR);
        return TRUE;
    } catch (...) {
        app.message(hwnd, app.tr(L"Could not complete the command.", L"Не удалось выполнить команду."),
                    appName, MB_OK | MB_ICONERROR);
        return TRUE;
    }
    return FALSE;
}

INT_PTR CALLBACK FileRenameDialog::callback(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_INITDIALOG) SetWindowLongPtrW(hwnd, DWLP_USER, lParam);
    auto* dialog = reinterpret_cast<FileRenameDialog*>(GetWindowLongPtrW(hwnd, DWLP_USER));
    if (!dialog) return FALSE;
    auto& app = dialog->app;
    try {
        if (message == WM_INITDIALOG) {
            SetWindowTextW(hwnd, app.tr(L"Rename item", L"Переименовать объект"));
            SetDlgItemTextW(hwnd, IDC_FILENAME_LABEL, app.tr(L"New name", L"Новое имя"));
            SetDlgItemTextW(hwnd, IDOK, app.tr(L"Rename", L"Переименовать"));
            SetDlgItemTextW(hwnd, IDCANCEL, app.tr(L"Cancel", L"Отмена"));
            const auto edit = GetDlgItem(hwnd, IDC_FILENAME);
            SendMessageW(edit, EM_LIMITTEXT, 255, 0);
            SetWindowTextW(edit, std::wstring(dialog->directory.leaf(dialog->file)).c_str());
            SetFocus(edit);
            SendMessageW(edit, EM_SETSEL, 0, static_cast<LPARAM>(dialog->directory.stem(dialog->file).size()));
            return FALSE;
        }
        if (message == WM_COMMAND && LOWORD(wParam) == IDOK) {
            const auto name = controlText(GetDlgItem(hwnd, IDC_FILENAME));
            if (name == dialog->directory.leaf(dialog->file)) {
                EndDialog(hwnd, IDCANCEL);
            } else if (const auto error = renameFile(dialog->directory, dialog->file, name)) {
                app.message(hwnd, windowsError(error, app.language).c_str(), appName, MB_OK | MB_ICONWARNING);
            } else {
                EndDialog(hwnd, IDOK);
            }
            return TRUE;
        }
        if (message == WM_CLOSE || (message == WM_COMMAND && LOWORD(wParam) == IDCANCEL)) {
            EndDialog(hwnd, IDCANCEL);
            return TRUE;
        }
    } catch (...) {
        if (message == WM_INITDIALOG) EndDialog(hwnd, IDCANCEL);
        app.message(hwnd, app.tr(L"Could not complete the command.", L"Не удалось выполнить команду."),
                    appName, MB_OK | MB_ICONERROR);
        return TRUE;
    }
    return FALSE;
}

void RenameDialog::init(HWND hwnd) {
    window = hwnd;
    SetWindowTextW(hwnd, app.tr(L"Batch rename", L"Групповое переименование"));
    SetDlgItemTextW(hwnd, IDC_STEM_LABEL, app.tr(L"Name mask", L"Маска имени"));
    SetDlgItemTextW(hwnd, IDC_EXTENSION_LABEL, app.tr(L"Extension", L"Расширение"));
    SetDlgItemTextW(hwnd, IDC_START_LABEL, app.tr(L"Start at", L"Начать с"));
    SetDlgItemTextW(hwnd, IDC_STEP_LABEL, app.tr(L"Step", L"Шаг"));
    SetDlgItemTextW(hwnd, IDC_DIGITS_LABEL, app.tr(L"Digits", L"Цифр"));
    SetDlgItemTextW(hwnd, IDC_MASK_HELP, app.tr(L"[C] counter    [N] original name    [E] original extension", L"[C] счётчик    [N] исходное имя    [E] исходное расширение"));
    SetDlgItemTextW(hwnd, IDOK, app.tr(L"Run", L"Выполнить"));
    preview = GetDlgItem(hwnd, IDC_PREVIEW);
    status = GetDlgItem(hwnd, IDC_RENAME_STATUS);
    bar = GetDlgItem(hwnd, IDC_RENAME_PROGRESS);
    ListView_SetExtendedListViewStyle(preview, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
    addColumn(preview, 0, app.tr(L"Original name", L"Исходное имя"), 250);
    addColumn(preview, 1, app.tr(L"New name", L"Новое имя"), 250);
    addColumn(preview, 2, app.tr(L"Size", L"Размер"), 110, LVCFMT_RIGHT);
    SendMessageW(bar, PBM_SETRANGE32, 0, 1000);
    SetDlgItemTextW(hwnd, IDC_STEM, options.stem.c_str());
    SetDlgItemTextW(hwnd, IDC_EXTENSION, options.extension.c_str());
    SetDlgItemTextW(hwnd, IDC_START, std::to_wstring(options.start).c_str());
    SetDlgItemTextW(hwnd, IDC_STEP, std::to_wstring(options.step).c_str());
    for (int id : {IDC_STEM, IDC_EXTENSION, IDC_START, IDC_STEP}) {
        SendDlgItemMessageW(hwnd, id, EM_LIMITTEXT, id == IDC_STEM || id == IDC_EXTENSION ? 255 : 20, 0);
    }
    const auto digits = GetDlgItem(hwnd, IDC_DIGITS);
    for (unsigned width = 1; width <= RenameOptions::maxDigits; ++width) {
        const auto label = std::to_wstring(width);
        SendMessageW(digits, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
    }
    SendMessageW(digits, CB_SETCURSEL, std::clamp(options.digits, 1u, RenameOptions::maxDigits) - 1, 0);
    RECT parent{}, rect{};
    GetWindowRect(app.window, &parent);
    GetWindowRect(hwnd, &rect);
    minimum = {rect.right - rect.left, rect.bottom - rect.top};
    SetWindowPos(hwnd, nullptr, parent.left + (parent.right - parent.left - minimum.cx) / 2,
        parent.top + (parent.bottom - parent.top - minimum.cy) / 2, 0, 0, SWP_NOZORDER | SWP_NOSIZE);
    auto measure = [&](HWND control) {
        RECT bounds{};
        GetWindowRect(control, &bounds);
        MapWindowPoints(nullptr, hwnd, reinterpret_cast<POINT*>(&bounds), 2);
        return bounds;
    };
    initialPreview = measure(preview);
    initialStatus = measure(status);
    initialBar = measure(bar);
    initialExecute = measure(GetDlgItem(hwnd, IDOK));
    initialCancel = measure(GetDlgItem(hwnd, IDCANCEL));
    ListView_SetItemCountEx(preview, static_cast<int>(files->size()), 0);
    setWorking(false);
    layout();
}

void RenameDialog::layout() {
    if (!minimum.cx) return;
    RECT rect{};
    GetWindowRect(window, &rect);
    const int dx = rect.right - rect.left - minimum.cx;
    const int dy = rect.bottom - rect.top - minimum.cy;
    auto resize = [](HWND control, const RECT& base, int x, int y, int extraWidth, int extraHeight) {
        MoveWindow(control, base.left + x, base.top + y,
                   base.right - base.left + extraWidth, base.bottom - base.top + extraHeight, TRUE);
    };
    resize(preview, initialPreview, 0, 0, dx, dy);
    resize(status, initialStatus, 0, dy, dx, 0);
    resize(bar, initialBar, 0, dy, dx, 0);
    resize(GetDlgItem(window, IDOK), initialExecute, dx, dy, 0, 0);
    resize(GetDlgItem(window, IDCANCEL), initialCancel, dx, dy, 0, 0);
    GetClientRect(preview, &rect);
    const int width = rect.right - app.px(18);
    ListView_SetColumnWidth(preview, 0, (width - app.px(110)) / 2);
    ListView_SetColumnWidth(preview, 1, (width - app.px(110)) / 2);
    ListView_SetColumnWidth(preview, 2, app.px(110));
}

void RenameDialog::refresh() {
    options.stem = controlText(GetDlgItem(window, IDC_STEM));
    options.extension = controlText(GetDlgItem(window, IDC_EXTENSION));
    const auto start = parseCounter(controlText(GetDlgItem(window, IDC_START)));
    const auto step = parseCounter(controlText(GetDlgItem(window, IDC_STEP)));
    const auto digits = SendDlgItemMessageW(window, IDC_DIGITS, CB_GETCURSEL, 0, 0);
    if (start && step && digits != CB_ERR) {
        options.start = *start;
        options.step = *step;
        options.digits = static_cast<unsigned>(digits + 1);
        validationError = renameOptionsError(options, files->size(), app.language);
    } else {
        validationError = app.tr(L"Enter a start value and step, and select 1 to 15 digits.",
                                 L"Укажите начало и шаг и выберите число цифр от 1 до 15.");
    }
    if (!validationError.empty()) setText(status, validationError);
    else if (!issue.empty()) setText(status, issue);
    else setText(status, app.tr(L"Files ", L"Файлов ") + number(files->size()) + app.tr(L"   ·   The counter follows the row order", L"   ·   Счётчик идёт по порядку строк"));
    EnableWindow(GetDlgItem(window, IDOK), !working && validationError.empty());
    InvalidateRect(preview, nullptr, FALSE);
}

void RenameDialog::setWorking(bool busy) {
    working = busy;
    for (int id : {IDC_STEM, IDC_EXTENSION, IDC_START, IDC_STEP, IDC_DIGITS}) {
        EnableWindow(GetDlgItem(window, id), !busy);
    }
    EnableWindow(GetDlgItem(window, IDCANCEL), TRUE);
    SetDlgItemTextW(window, IDCANCEL, busy ? app.tr(L"Stop", L"Остановить") : app.tr(L"Close", L"Закрыть"));
    ShowWindow(bar, busy ? SW_SHOWNA : SW_HIDE);
    if (busy) {
        EnableWindow(GetDlgItem(window, IDOK), FALSE);
        updateProgress(status, bar, *app.operation.progress, app.language);
    } else {
        refresh();
    }
}

INT_PTR CALLBACK RenameDialog::callback(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_INITDIALOG) SetWindowLongPtrW(hwnd, DWLP_USER, lParam);
    auto* dialog = reinterpret_cast<RenameDialog*>(GetWindowLongPtrW(hwnd, DWLP_USER));
    if (!dialog) return FALSE;
    try {
        switch (message) {
            case WM_INITDIALOG:
                dialog->init(hwnd);
                return TRUE;
            case WM_SIZE:
                dialog->layout();
                return TRUE;
            case WM_GETMINMAXINFO:
                if (dialog->minimum.cx) reinterpret_cast<MINMAXINFO*>(lParam)->ptMinTrackSize =
                    {dialog->minimum.cx, dialog->minimum.cy};
                return TRUE;
            case WM_CLOSE:
                SendMessageW(hwnd, WM_COMMAND, IDCANCEL, 0);
                return TRUE;
            case WM_COMMAND: {
                const int id = LOWORD(wParam);
                const int notification = HIWORD(wParam);
                if (id == IDOK && !dialog->working && dialog->validationError.empty()) {
                    dialog->app.startRename(dialog->options);
                    return TRUE;
                }
                if (id == IDCANCEL) {
                    if (dialog->working) {
                        dialog->app.operation.progress->cancel = true;
                        EnableWindow(GetDlgItem(hwnd, IDCANCEL), FALSE);
                    } else {
                        EndDialog(hwnd, IDCANCEL);
                    }
                    return TRUE;
                }
                const bool editChanged = notification == EN_CHANGE && id >= IDC_STEM && id <= IDC_STEP;
                const bool digitsChanged = id == IDC_DIGITS && (notification == CBN_CLOSEUP ||
                    (notification == CBN_SELCHANGE && !SendDlgItemMessageW(hwnd, IDC_DIGITS, CB_GETDROPPEDSTATE, 0, 0)));
                if ((editChanged || digitsChanged) && !dialog->working) {
                    dialog->issue.clear();
                    dialog->refresh();
                    return TRUE;
                }
                break;
            }
            case WM_NOTIFY: {
                auto& header = *reinterpret_cast<NMHDR*>(lParam);
                if (header.idFrom != IDC_PREVIEW || header.code != LVN_GETDISPINFOW) break;
                auto& info = reinterpret_cast<NMLVDISPINFOW&>(header);
                const auto row = static_cast<std::size_t>(info.item.iItem);
                if (row >= dialog->files->size()) break;
                const auto index = (*dialog->files)[row];
                if (info.item.iSubItem == 0) copyText(info, dialog->directory->name(index));
                if (info.item.iSubItem == 1) {
                    if (!dialog->validationError.empty()) copyText(info, L"");
                    else {
                        const auto newName = targetName(*dialog->directory, index, dialog->options, row);
                        copyText(info, dialog->directory->recursive ? dialog->directory->sibling(index, newName) : newName);
                    }
                }
                if (info.item.iSubItem == 2) copyText(info, number(dialog->directory->entries[index].size));
                return TRUE;
            }
        }
    } catch (...) {
        if (message == WM_INITDIALOG) EndDialog(hwnd, IDCANCEL);
        dialog->app.message(hwnd, dialog->app.tr(L"Could not complete the command.", L"Не удалось выполнить команду."),
                            appName, MB_OK | MB_ICONERROR);
        return TRUE;
    }
    return FALSE;
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    const auto instance = GetModuleHandleW(nullptr);
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES | ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);
    App app(instance, argc > 1 ? argv[1] : L"", argc > 2 ? argv[2] : L"");
    auto icon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_NOTFAR));
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = App::callback;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = icon;
    wc.hIconSm = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_NOTFAR),
        IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED));
    wc.lpszClassName = appClass;
    if (!RegisterClassExW(&wc)) return 1;

    const UINT dpi = GetDpiForSystem();
    HWND window = CreateWindowExW(0, appClass, appName, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                  CW_USEDEFAULT, CW_USEDEFAULT, MulDiv(1120, dpi, 96), MulDiv(690, dpi, 96),
                                  nullptr, nullptr, instance, &app);
    if (!window) return 1;
    ShowWindow(window, SW_SHOWDEFAULT);
    UpdateWindow(window);
    SetFocus(app.panes[0].list);
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}
