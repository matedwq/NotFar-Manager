#include "files.hpp"
#include "resource.h"

#include <commctrl.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <cwchar>
#include <future>
#include <thread>
#include <utility>

namespace {
using namespace nf;

constexpr wchar_t appName[] = L"NotFar Manager";
constexpr auto appClass = L"NotFarManager.Window";
constexpr UINT operationNotice = WM_APP + 1;
constexpr COLORREF ink = RGB(34, 48, 55);
constexpr COLORREF accent = RGB(24, 100, 105);
constexpr COLORREF canvas = RGB(239, 244, 245);
constexpr COLORREF marked = RGB(191, 25, 40);
constexpr COLORREF activeTint = RGB(214, 237, 238);
constexpr int firstPanelId = 300;
enum Control { Threshold = 200, SelectSmall, DeleteSelected, ClearSelected, StopOperation };
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

std::wstring phaseLabel(Phase phase) {
    switch (phase) {
        case Phase::Read: return L"Чтение каталога";
        case Phase::Sort: return L"Сортировка";
        case Phase::Validate: return L"Проверка новых имён";
        case Phase::Rename: return L"Переименование";
        case Phase::Delete: return L"Удаление";
    }
    return {};
}

void updateProgress(HWND status, HWND bar, const Progress& progress) {
    const auto total = progress.total.load();
    const auto done = progress.done.load();
    auto text = phaseLabel(progress.phase);
    if (total) text += L"   " + number(std::min(done, total)) + L" / " + number(total);
    if (progress.cancel) text += L"   ·   Остановка…";
    setText(status, text);
    SendMessageW(bar, PBM_SETPOS, total ? static_cast<WPARAM>(std::min(done, total) * 1000ULL / total) : 0, 0);
}

template<class Result>
struct Job {
    std::shared_ptr<Progress> progress;
    std::future<Result> future;

    bool ready() const {
        return future.valid() && future.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
    }

    explicit operator bool() const { return future.valid(); }

    template<class Work>
    void start(Work work, Phase phase = Phase::Read) {
        auto state = std::make_shared<Progress>();
        state->phase = phase;
        std::packaged_task<Result()> task([state, work = std::move(work)]() mutable {
            return work(*state);
        });
        auto value = task.get_future();
        std::thread worker(std::move(task));
        worker.detach();
        if (progress) progress->cancel = true;
        progress = std::move(state);
        future = std::move(value);
    }
};

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
    std::vector<std::uint8_t> selected;
    std::size_t selectedCount = 0;
    std::uint64_t selectedBytes = 0;
    int anchor = -1;

    bool hasParent() const { return visible.directory && !parentPath(visible.directory->path).empty(); }
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
    HWND window = nullptr;
    HWND threshold = nullptr;
    HWND selectButton = nullptr;
    HWND deleteButton = nullptr;
    HWND clearButton = nullptr;
    HWND bytes = nullptr;
    HWND help = nullptr;
    HWND activity = nullptr;
    HWND progress = nullptr;
    HWND stop = nullptr;
    HFONT font = nullptr;
    HBRUSH background = nullptr;
    HBRUSH statusTint = nullptr;
    UINT dpi = 96;
    Pane panes[2];
    int active = 0;
    Job<OperationResult> operation;
    std::wstring operationPath;
    std::wstring notice;
    std::wstring lastStatus;
    RenameOptions previousRename;
    RenameDialog* renameDialog = nullptr;
    bool renaming = false;
    bool closeAfterOperation = false;

    explicit App(HINSTANCE module) : instance(module) {}
    ~App() {
        for (auto& pane : panes) if (pane.job.progress) pane.job.progress->cancel = true;
        if (font) DeleteObject(font);
        if (background) DeleteObject(background);
        if (statusTint) DeleteObject(statusTint);
    }

    int px(int value) const { return MulDiv(value, dpi, 96); }
    static LRESULT CALLBACK callback(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK childProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam,
                                     UINT_PTR subclass, DWORD_PTR context);
    LRESULT handle(UINT message, WPARAM wParam, LPARAM lParam);
    void create();
    void layout();
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
    void selectSmaller();
    void clearSelection();
    void enterRow(Pane& pane, int row);
    void goUp(Pane& pane);
    void goToPath(Pane& pane);
    void openRename();
    void startRename(const RenameOptions& options);
    void startDelete();
    void consumeOperation();
    bool key(HWND hwnd, UINT key);
    LRESULT notify(NMHDR& header);
    HWND make(DWORD ex, const wchar_t* klass, const wchar_t* label, DWORD style, int id = 0);

    Pane& current() { return panes[active]; }
    bool available(const Pane& pane) const { return pane.visible.directory && !pane.job && !operation; }
};

HWND App::make(DWORD ex, const wchar_t* klass, const wchar_t* label, DWORD style, int id) {
    return CreateWindowExW(ex, klass, label, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, window,
                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
}

void App::create() {
    dpi = GetDpiForWindow(window);
    background = CreateSolidBrush(canvas);
    statusTint = CreateSolidBrush(activeTint);
    selectButton = make(0, L"BUTTON", L"Выделить файлы меньше", WS_TABSTOP, SelectSmall);
    threshold = make(WS_EX_CLIENTEDGE, L"EDIT", L"2048", ES_NUMBER | ES_AUTOHSCROLL | WS_TABSTOP, Threshold);
    SendMessageW(threshold, EM_LIMITTEXT, 20, 0);
    bytes = make(0, L"STATIC", L"байт", SS_CENTERIMAGE);
    deleteButton = make(0, L"BUTTON", L"Удалить выделенное", WS_TABSTOP, DeleteSelected);
    clearButton = make(0, L"BUTTON", L"Снять выделение", WS_TABSTOP, ClearSelected);
    help = make(0, L"STATIC", L"Ctrl+M  Переименовать    Insert / Пробел  Выделить    F5  Обновить", SS_CENTERIMAGE);
    activity = make(0, L"STATIC", L"", SS_CENTERIMAGE | SS_NOPREFIX | SS_ENDELLIPSIS);
    progress = make(0, PROGRESS_CLASSW, L"", PBS_SMOOTH);
    SendMessageW(progress, PBM_SETRANGE32, 0, 1000);
    stop = make(0, L"BUTTON", L"Остановить", WS_TABSTOP, StopOperation);

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
        addColumn(pane.list, 0, L"Имя", px(230));
        addColumn(pane.list, 1, L"Тип", px(78));
        addColumn(pane.list, 2, L"Размер", px(120), LVCFMT_RIGHT);
        pane.status = make(0, L"STATIC", L"", SS_CENTERIMAGE | SS_NOPREFIX | SS_ENDELLIPSIS, id + Status);
        updateHeader(pane);
    }
    for (HWND child = GetWindow(window, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
        SetWindowSubclass(child, childProc, 1, reinterpret_cast<DWORD_PTR>(this));
    }
    rebuildFont();

    wchar_t windows[MAX_PATH]{};
    GetWindowsDirectoryW(windows, static_cast<UINT>(std::size(windows)));
    auto right = absolutePath(Name(windows).substr(0, 3));
    if (right.empty() && *drives) right = drives;
    wchar_t profile[32768]{};
    GetEnvironmentVariableW(L"USERPROFILE", profile, static_cast<DWORD>(std::size(profile)));
    auto left = absolutePath(profile);
    if (left.empty()) left = right;
    int argc = 0;
    if (auto argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
        if (argc > 1 && *argv[1]) left = absolutePath(argv[1]);
        if (argc > 2 && *argv[2]) right = absolutePath(argv[2]);
        LocalFree(argv);
    }
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
    top(selectButton, 192);
    top(threshold, 92, 26, 1);
    top(bytes, 33);
    x += px(10);
    top(deleteButton, 179);
    top(clearButton, 152);

    const int panelY = px(58);
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
    EnableWindow(selectButton, enable);
    EnableWindow(deleteButton, enable && pane.selectedCount != 0);
    EnableWindow(clearButton, enable && pane.selectedCount != 0);
    for (const auto& item : panes) {
        EnableWindow(item.drive, !operation);
        EnableWindow(item.path, !operation);
        EnableWindow(item.up, available(item) && item.parentRow);
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
        auto text = phaseLabel(phase) + L"…";
        if (count) text += L"   " + number(count);
        setText(pane.status, text);
        return;
    }
    if (!pane.visible.directory) {
        setText(pane.status, L"Выберите диск или введите путь к папке");
        return;
    }
    const auto files = pane.visible.directory->fileCount;
    const auto folders = pane.visible.directory->entries.size() - files;
    auto text = L"Файлов " + number(files) + L"   ·   Папок " + number(folders);
    if (pane.selectedCount) {
        text += L"   ·   Выделено " + number(pane.selectedCount) + L"   ·   " + number(pane.selectedBytes) + L" байт";
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
        const auto keepParent = !newListing && cursor == 0 && pane.parentRow;
        if (newListing) {
            pane.selected.assign(listing.order.size(), std::uint8_t{});
            pane.selectedCount = 0;
            pane.selectedBytes = 0;
        }
        pane.visible = std::move(listing);
        pane.parentRow = pane.hasParent();
        pane.sort = pane.requestedSort;
        pane.anchor = -1;
        ListView_SetItemCountEx(pane.list, pane.count(), 0);
        ListView_SetItemState(pane.list, -1, 0, LVIS_FOCUSED | LVIS_SELECTED);
        auto row = 0;
        if (!newListing && oldIndex && !keepParent) {
            const auto found = std::find(pane.visible.order.begin(), pane.visible.order.end(), *oldIndex);
            if (found != pane.visible.order.end()) row = static_cast<int>(found - pane.visible.order.begin()) + pane.offset();
        }
        if (pane.count()) {
            ListView_SetItemState(pane.list, row, LVIS_FOCUSED | LVIS_SELECTED, LVIS_FOCUSED | LVIS_SELECTED);
            ListView_EnsureVisible(pane.list, row, FALSE);
        }
        updateHeader(pane);
        InvalidateRect(pane.list, nullptr, FALSE);
        setText(pane.path, pane.visible.directory->path);
    } catch (DWORD code) {
        reason = windowsError(code);
    } catch (const std::bad_alloc&) {
        reason = L"Недостаточно памяти.";
    } catch (...) {
        reason = L"Не удалось прочитать или отсортировать каталог.";
    }
    if (!reason.empty()) {
        if (pane.visible.directory) setText(pane.path, pane.visible.directory->path);
        SendMessageW(pane.drive, CB_SETCURSEL, static_cast<WPARAM>(-1), 0);
        if (pane.visible.directory && pane.visible.directory->path.size() >= 3 && pane.visible.directory->path[1] == L':') {
            SendMessageW(pane.drive, CB_SELECTSTRING, static_cast<WPARAM>(-1), reinterpret_cast<LPARAM>(pane.visible.directory->path.substr(0, 3).c_str()));
        }
    }
    pane.pendingPath.clear();
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
    if (entry.folder() || static_cast<bool>(selected) == value) return;
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

void App::selectSmaller() {
    auto& pane = current();
    if (!available(pane)) return;
    const auto limit = parseNumber(controlText(threshold));
    if (!limit) {
        MessageBoxW(window, L"Укажите размер в байтах целым неотрицательным числом.", appName, MB_OK | MB_ICONINFORMATION);
        SetFocus(threshold);
        return;
    }
    const auto& entries = pane.visible.directory->entries;
    for (Index i = 0; i < entries.size(); ++i) mark(pane, i, entries[i].size < *limit);
    pane.anchor = -1;
    redrawSelection(pane);
    SetFocus(pane.list);
}

void App::clearSelection() {
    auto& pane = current();
    if (!available(pane)) return;
    std::fill(pane.selected.begin(), pane.selected.end(), std::uint8_t{});
    pane.selectedCount = 0;
    pane.selectedBytes = 0;
    pane.anchor = -1;
    redrawSelection(pane);
    SetFocus(pane.list);
}

void App::goUp(Pane& pane) {
    if (!available(pane)) return;
    startRead(pane, parentPath(pane.visible.directory->path));
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
        MessageBoxW(window, L"Укажите путь к папке, например C:\\Данные.", appName, MB_OK | MB_ICONINFORMATION);
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
        MessageBoxW(window, windowsError(GetLastError()).c_str(), appName, MB_OK | MB_ICONERROR);
    }
    SetFocus(pane.list);
}

void App::startRename(const RenameOptions& options) {
    if (operation || !renameDialog) return;
    auto directory = renameDialog->directory;
    auto files = renameDialog->files;
    previousRename = options;
    operationPath = directory->path;
    operation.start([directory, files, options](Progress& monitor) {
        return renameFiles(*directory, *files, options, monitor);
    }, Phase::Validate);
    renaming = true;
    renameDialog->setWorking(true);
    updateCommands();
}

void App::startDelete() {
    auto& pane = current();
    if (!available(pane) || !pane.selectedCount) return;
    const auto path = pane.visible.directory->path;
    const auto question = L"Удалить без корзины выбранные файлы (" + number(pane.selectedCount) + L")?\n\n" + path;
    if (MessageBoxW(window, question.c_str(), L"Удаление файлов",
                    MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) return;
    auto files = pane.orderedSelection();
    operationPath = path;
    operation.start([directory = pane.visible.directory, files = std::move(files)](Progress& monitor) {
        return deleteFiles(*directory, files, monitor);
    }, Phase::Delete);
    renaming = false;
    EnableWindow(stop, TRUE);
    updateProgress(activity, progress, *operation.progress);
    updateCommands();
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
        error = L"Недостаточно памяти.";
    } catch (DWORD code) {
        error = windowsError(code);
    } catch (...) {
        error = L"Операция прервана из-за непредвиденной ошибки.";
    }
    const bool editing = renaming && renameDialog && (!result.rejected.empty() || (phase == Phase::Validate && !error.empty()));
    if (editing) {
        renameDialog->issue = result.rejected.empty() ? error : result.rejected;
        renameDialog->setWorking(false);
        MessageBoxW(renameDialog->window, renameDialog->issue.c_str(), appName, MB_OK | MB_ICONWARNING);
        updateCommands();
        return;
    }

    lastStatus = (renaming ? L"Переименовано " : L"Удалено ") + number(result.changed);
    if (result.failed) lastStatus += L"   ·   Ошибок " + number(result.failed);
    if (result.cancelled) lastStatus += L"   ·   Остановлено";
    if (!error.empty()) lastStatus = error;
    if (renaming && renameDialog) EndDialog(renameDialog->window, IDOK);
    const bool refresh = result.changed || (phase != Phase::Validate && (!error.empty() || result.failed));
    renaming = false;
    if (closeAfterOperation) {
        DestroyWindow(window);
        return;
    }
    if (refresh) {
        for (auto& pane : panes) {
            if (pane.job && !pane.pendingPath.empty() && compareNames(pane.pendingPath, operationPath) != 0) continue;
            if (pane.visible.directory && compareNames(pane.visible.directory->path, operationPath) == 0) {
                startRead(pane, pane.visible.directory->path, true);
            } else if (pane.job && compareNames(pane.pendingPath, operationPath) == 0) {
                startRead(pane, pane.pendingPath, true);
            }
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
        if (pane.job && !pane.pendingPath.empty()) startRead(pane, pane.pendingPath, true);
        else if (pane.visible.directory) startRead(pane, pane.visible.directory->path, true);
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
        selectRange(pane, 0, pane.count() - 1);
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
    if (keyCode == VK_UP || keyCode == VK_DOWN || keyCode == VK_HOME || keyCode == VK_END) pane.anchor = -1;
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
                copyText(info, index ? pane.visible.directory->stem(*index)
                                     : (pane.parentRow && info.item.iItem == 0 ? L".." : L""));
            } else if (index && info.item.iSubItem == 1) {
                copyText(info, pane.visible.directory->entries[*index].folder()
                               ? L"Папка" : pane.visible.directory->extension(*index));
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
        if (header.code == LVN_COLUMNCLICK) {
            setActive(side);
            sortBy(pane, reinterpret_cast<NMLISTVIEW&>(header).iSubItem);
        }
        if (header.code == NM_CLICK || header.code == NM_RCLICK || header.code == NM_DBLCLK) {
            setActive(side);
            const int row = reinterpret_cast<NMITEMACTIVATE&>(header).iItem;
            if (row < 0) return 0;
            if (header.code == NM_DBLCLK) enterRow(pane, row);
            else if (header.code == NM_RCLICK || (GetKeyState(VK_CONTROL) & 0x8000)) toggle(pane, row);
            else if ((GetKeyState(VK_SHIFT) & 0x8000) && pane.anchor >= 0) selectRange(pane, pane.anchor, row);
            else pane.anchor = row;
        }
        return 0;
    }
    return 0;
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
            info.ptMinTrackSize = {px(780), px(355)};
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
            RECT pathBand{x, px(56), end, px(93)};
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
                if (renaming && renameDialog) updateProgress(renameDialog->status, renameDialog->bar, *operation.progress);
                else updateProgress(activity, progress, *operation.progress);
            }
            return 0;
        case WM_NOTIFY:
            return notify(*reinterpret_cast<NMHDR*>(lParam));
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
            if (id == SelectSmall && reason == BN_CLICKED) selectSmaller();
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
                MessageBoxW(window, messageText.c_str(), appName, MB_OK | MB_ICONWARNING);
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
        } catch (...) {
            MessageBoxW(hwnd, L"Не удалось выполнить команду.", appName, MB_OK | MB_ICONERROR);
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
        } catch (...) {
            MessageBoxW(app.window, L"Не удалось выполнить команду.", appName, MB_OK | MB_ICONERROR);
            return 0;
        }
    }
    if (message == WM_CHAR && (wParam == VK_RETURN || wParam == VK_ESCAPE || wParam == VK_TAB)) {
        const auto& pane = app.current();
        if (hwnd == pane.list || hwnd == pane.path || hwnd == app.threshold) return 0;
    }
    return DefSubclassProc(hwnd, message, wParam, lParam);
}

void RenameDialog::init(HWND hwnd) {
    window = hwnd;
    SetWindowTextW(hwnd, L"Групповое переименование");
    SetDlgItemTextW(hwnd, IDC_STEM_LABEL, L"Маска имени");
    SetDlgItemTextW(hwnd, IDC_EXTENSION_LABEL, L"Расширение");
    SetDlgItemTextW(hwnd, IDC_START_LABEL, L"Начать с");
    SetDlgItemTextW(hwnd, IDC_STEP_LABEL, L"Шаг");
    SetDlgItemTextW(hwnd, IDC_DIGITS_LABEL, L"Цифр");
    SetDlgItemTextW(hwnd, IDC_MASK_HELP, L"[C] счётчик    [N] исходное имя    [E] исходное расширение");
    SetDlgItemTextW(hwnd, IDOK, L"Выполнить");
    preview = GetDlgItem(hwnd, IDC_PREVIEW);
    status = GetDlgItem(hwnd, IDC_RENAME_STATUS);
    bar = GetDlgItem(hwnd, IDC_RENAME_PROGRESS);
    ListView_SetExtendedListViewStyle(preview, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
    addColumn(preview, 0, L"Исходное имя", 250);
    addColumn(preview, 1, L"Новое имя", 250);
    addColumn(preview, 2, L"Размер", 110, LVCFMT_RIGHT);
    SendMessageW(bar, PBM_SETRANGE32, 0, 1000);
    SetDlgItemTextW(hwnd, IDC_STEM, options.stem.c_str());
    SetDlgItemTextW(hwnd, IDC_EXTENSION, options.extension.c_str());
    SetDlgItemTextW(hwnd, IDC_START, std::to_wstring(options.start).c_str());
    SetDlgItemTextW(hwnd, IDC_STEP, std::to_wstring(options.step).c_str());
    SetDlgItemTextW(hwnd, IDC_DIGITS, std::to_wstring(options.digits).c_str());
    for (int id : {IDC_STEM, IDC_EXTENSION, IDC_START, IDC_STEP, IDC_DIGITS}) {
        SendDlgItemMessageW(hwnd, id, EM_LIMITTEXT, id == IDC_STEM || id == IDC_EXTENSION ? 255 : 20, 0);
    }
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
    const auto digits = parseNumber(controlText(GetDlgItem(window, IDC_DIGITS)));
    if (start && step && digits && *digits <= 20) {
        options.start = *start;
        options.step = *step;
        options.digits = static_cast<unsigned>(*digits);
        validationError = renameOptionsError(options, files->size());
    } else {
        validationError = L"Введите начало, шаг и ширину счётчика (1–20).";
    }
    if (!validationError.empty()) setText(status, validationError);
    else if (!issue.empty()) setText(status, issue);
    else setText(status, L"Файлов " + number(files->size()) + L"   ·   Счётчик идёт по порядку строк");
    EnableWindow(GetDlgItem(window, IDOK), !working && validationError.empty());
    InvalidateRect(preview, nullptr, FALSE);
}

void RenameDialog::setWorking(bool busy) {
    working = busy;
    for (int id : {IDC_STEM, IDC_EXTENSION, IDC_START, IDC_STEP, IDC_DIGITS}) {
        EnableWindow(GetDlgItem(window, id), !busy);
    }
    EnableWindow(GetDlgItem(window, IDCANCEL), TRUE);
    SetDlgItemTextW(window, IDCANCEL, busy ? L"Остановить" : L"Закрыть");
    ShowWindow(bar, busy ? SW_SHOWNA : SW_HIDE);
    if (busy) {
        EnableWindow(GetDlgItem(window, IDOK), FALSE);
        updateProgress(status, bar, *app.operation.progress);
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
                if (HIWORD(wParam) == EN_CHANGE && id >= IDC_STEM && id <= IDC_DIGITS && !dialog->working) {
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
                if (info.item.iSubItem == 1) copyText(info, dialog->validationError.empty()
                    ? targetName(*dialog->directory, index, dialog->options, row) : L"");
                if (info.item.iSubItem == 2) copyText(info, number(dialog->directory->entries[index].size));
                return TRUE;
            }
        }
    } catch (...) {
        if (message == WM_INITDIALOG) EndDialog(hwnd, IDCANCEL);
        MessageBoxW(hwnd, L"Не удалось выполнить команду.", appName, MB_OK | MB_ICONERROR);
        return TRUE;
    }
    return FALSE;
}
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES | ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);
    App app(instance);
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
    ShowWindow(window, show);
    UpdateWindow(window);
    SetFocus(app.panes[0].list);
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}
