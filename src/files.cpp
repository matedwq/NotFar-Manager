#include "files.hpp"

#include <objbase.h>
#include <shlobj.h>
#include <sherrors.h>

#include <algorithm>
#include <array>
#include <bit>
#include <bitset>
#include <limits>
#include <numeric>
#include <stdexcept>

namespace nf {
namespace {

constexpr Index none = std::numeric_limits<Index>::max();
constexpr std::uint64_t signBit = std::uint64_t{1} << 63;

struct Cancelled {};

struct FileHandle {
    HANDLE value;
    ~FileHandle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};

struct SearchHandle {
    HANDLE value;
    ~SearchHandle() { if (value != INVALID_HANDLE_VALUE) FindClose(value); }
};

std::wstring nativePath(Name path) {
    if (path.starts_with(L"\\\\?\\")) return std::wstring(path);
    if (path.starts_with(L"\\\\")) return L"\\\\?\\UNC\\" + std::wstring(path.substr(2));
    return L"\\\\?\\" + std::wstring(path);
}

std::size_t rootLength(Name path) {
    if (path.size() >= 3 && path[1] == L':' && path[2] == L'\\') return 3;
    if (path.starts_with(L"\\\\")) {
        const auto serverEnd = path.find(L'\\', 2);
        if (serverEnd == Name::npos) return 0;
        const auto shareEnd = path.find(L'\\', serverEnd + 1);
        if (serverEnd + 1 == path.size() || shareEnd == serverEnd + 1) return 0;
        return shareEnd == Name::npos ? path.size() : shareEnd;
    }
    return 0;
}

struct FilePath {
    explicit FilePath(Name directory) : value(nativePath(childPath(directory, {}))), prefix(value.size()) {
        value.reserve(prefix + 260);
    }
    const wchar_t* set(Name file) {
        value.resize(prefix);
        value.append(file);
        return value.c_str();
    }
    std::wstring value;
    std::size_t prefix;
};

void appendRecord(Directory& dir, Name name, std::uint64_t size, DWORD attributes) {
    if (name != L"." && name != L"..") dir.add(name, size, (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
}

bool readBuffered(Directory& dir, Progress& progress) {
    FileHandle handle{CreateFileW(nativePath(dir.path).c_str(), FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS, nullptr)};
    if (handle.value == INVALID_HANDLE_VALUE) return false;

    alignas(FILE_FULL_DIR_INFO) std::array<std::byte, 64 * 1024> buffer;
    auto mode = FileFullDirectoryRestartInfo;
    while (!progress.cancel) {
        if (!GetFileInformationByHandleEx(handle.value, mode, buffer.data(), static_cast<DWORD>(buffer.size()))) {
            const DWORD error = GetLastError();
            if (error == ERROR_NO_MORE_FILES) return true;
            if (mode == FileFullDirectoryRestartInfo) return false;
            throw error;
        }

        const auto* record = buffer.data();
        for (;;) {
            const auto& info = *reinterpret_cast<const FILE_FULL_DIR_INFO*>(record);
            appendRecord(dir, Name(info.FileName, info.FileNameLength / sizeof(wchar_t)),
                         static_cast<std::uint64_t>(info.EndOfFile.QuadPart), info.FileAttributes);
            if (!info.NextEntryOffset) break;
            record += info.NextEntryOffset;
        }
        mode = FileFullDirectoryInfo;
        progress.done = dir.entries.size();
    }
    throw Cancelled{};
}

void readWithFind(Directory& dir, Progress& progress) {
    FilePath pattern(dir.path);
    WIN32_FIND_DATAW data;
    SearchHandle handle{FindFirstFileExW(pattern.set(L"*"), FindExInfoBasic,
                                         &data, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH)};
    if (handle.value == INVALID_HANDLE_VALUE &&
        (GetLastError() == ERROR_INVALID_PARAMETER || GetLastError() == ERROR_NOT_SUPPORTED)) {
        handle.value = FindFirstFileExW(pattern.set(L"*"), FindExInfoBasic,
                                        &data, FindExSearchNameMatch, nullptr, 0);
    }
    if (handle.value == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND) {
            const DWORD attrs = GetFileAttributesW(nativePath(dir.path).c_str());
            if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY)) return;
        }
        throw error;
    }

    do {
        const auto size = (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
        appendRecord(dir, data.cFileName, size, data.dwFileAttributes);
        if ((dir.entries.size() & 1023) == 0) {
            if (progress.cancel) throw Cancelled{};
            progress.done = dir.entries.size();
        }
    } while (FindNextFileW(handle.value, &data));
    const DWORD error = GetLastError();
    if (error != ERROR_NO_MORE_FILES) throw error;
}

wchar_t foldAscii(wchar_t ch) {
    return ch >= L'a' && ch <= L'z' ? ch - (L'a' - L'A') : ch;
}

bool validMask(Name mask) {
    for (std::size_t i = 0; i < mask.size(); ++i) {
        if (mask[i] != L'[' && mask[i] != L']') continue;
        if (i + 2 >= mask.size() || mask[i] != L'[' || mask[i + 2] != L']' ||
            Name(L"NCE").find(foldAscii(mask[i + 1])) == Name::npos) return false;
        i += 2;
    }
    return true;
}

void expand(std::wstring& target, Name mask, Name stem, Name extension, Name counter) {
    for (std::size_t i = 0; i < mask.size(); ++i) {
        if (mask[i] == L'[') {
            switch (foldAscii(mask[i + 1])) {
                case L'N': target += stem; break;
                case L'E': target += extension; break;
                case L'C': target += counter; break;
            }
            i += 2;
        } else {
            target += mask[i];
        }
    }
}

bool validTarget(Name target) {
    if (target.empty() || target.size() > 255 || target.back() == L'.' ||
        target.back() == L' ' || target.find_first_of(L"\\/:*?\"<>|") != Name::npos) return false;
    for (wchar_t ch : target) if (ch < 32) return false;

    Name base = target.substr(0, target.find(L'.'));
    while (!base.empty() && base.back() == L' ') base.remove_suffix(1);
    for (Name reserved : {L"CON", L"PRN", L"AUX", L"NUL", L"CONIN$", L"CONOUT$"}) {
        if (compareNames(base, reserved) == 0) return false;
    }
    return base.size() != 4 || Name(L"123456789¹²³").find(base[3]) == Name::npos ||
           (compareNames(base.substr(0, 3), L"COM") != 0 && compareNames(base.substr(0, 3), L"LPT") != 0);
}

void recordError(OperationResult& result, Name name, Name message) {
    if (result.errors.size() < 6) result.errors.push_back(std::wstring(name) + L" — " + std::wstring(message));
}

struct Move {
    Index source;
    Index targetOffset;
    std::uint16_t targetLength;
    enum class State : std::uint8_t { Pending, Visiting, Done, Failed } state = State::Pending;
    Index dependency = none;
};

} // namespace

void Directory::add(Name value, std::uint64_t size, bool folder) {
    if (value.size() > 0x7fff || names.size() + value.size() > none || entries.size() >= INT_MAX - 1) {
        throw static_cast<DWORD>(ERROR_NOT_ENOUGH_MEMORY);
    }
    const auto dot = value.rfind(L'.');
    const auto stemLength = !folder && dot != Name::npos && dot != 0 ? dot : value.size();
    entries.push_back({folder ? 0 : size, static_cast<Index>(names.size()),
                       static_cast<std::uint16_t>(value.size() | (folder ? 0x8000 : 0)),
                       static_cast<std::uint16_t>(stemLength)});
    names.insert(names.end(), value.begin(), value.end());
    if (!folder) ++fileCount;
}

Name Directory::name(Index index) const {
    const auto& entry = entries[index];
    return {names.data() + entry.offset, entry.length()};
}

Name Directory::stem(Index index) const {
    return name(index).substr(0, entries[index].stemLength);
}

Name Directory::extension(Index index) const {
    const auto value = name(index);
    const auto offset = entries[index].stemLength;
    return offset < value.size() ? value.substr(offset + 1) : Name{};
}

std::wstring absolutePath(Name input, Name base) {
    if (input.size() >= 2 && input.front() == L'"' && input.back() == L'"') {
        input.remove_prefix(1);
        input.remove_suffix(1);
    }
    if (input.empty()) return {};
    std::wstring path(input);
    std::replace(path.begin(), path.end(), L'/', L'\\');
    if (path.starts_with(L"\\\\?\\UNC\\")) path = L"\\\\" + path.substr(8);
    else if (path.starts_with(L"\\\\?\\")) path.erase(0, 4);
    if (path.starts_with(L"\\\\.\\")) return {};
    if (path.size() == 2 && path[1] == L':') path += L'\\';
    if (path.starts_with(L"\\") && !path.starts_with(L"\\\\") && !base.empty()) {
        path = std::wstring(base.substr(0, rootLength(base))) + path;
    } else if (!path.starts_with(L"\\") && !(path.size() >= 2 && path[1] == L':') && !base.empty()) {
        path = childPath(base, path);
    }
    const DWORD required = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    if (!required) return {};
    std::wstring full(required, L'\0');
    const DWORD length = GetFullPathNameW(path.c_str(), required, full.data(), nullptr);
    if (!length || length >= required) return {};
    full.resize(length);
    const auto root = rootLength(full);
    if (!root) return {};
    while (full.size() > root && full.back() == L'\\') full.pop_back();
    return full;
}

std::wstring parentPath(Name path) {
    const auto root = rootLength(path);
    if (!root || path.size() <= root) return {};
    return std::wstring(path.substr(0, std::max(root, path.rfind(L'\\'))));
}

std::wstring childPath(Name path, Name name) {
    std::wstring result(path);
    if (!result.empty() && result.back() != L'\\') result += L'\\';
    result.append(name);
    return result;
}

std::wstring number(std::uint64_t value) {
    const auto digits = std::to_wstring(value);
    std::wstring formatted;
    formatted.reserve(digits.size() + digits.size() / 3);
    for (std::size_t i = 0; i < digits.size(); ++i) {
        if (i && (digits.size() - i) % 3 == 0) formatted += L'\u202f';
        formatted += digits[i];
    }
    return formatted;
}

std::wstring windowsError(DWORD error, Language language) {
    wchar_t buffer[512]{};
    const auto locale = language == Language::Russian ? LANG_RUSSIAN : LANG_ENGLISH;
    const auto count = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                      nullptr, error, MAKELANGID(locale, SUBLANG_DEFAULT),
                                      buffer, static_cast<DWORD>(std::size(buffer)), nullptr);
    std::wstring message(buffer, count);
    while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n' || message.back() == L' ')) {
        message.pop_back();
    }
    return message.empty() ? translated(language, L"Windows error ", L"Ошибка Windows ") + std::to_wstring(error) : message;
}

std::optional<std::uint64_t> parseNumber(Name text) {
    if (text.empty()) return std::nullopt;
    std::uint64_t value = 0;
    for (wchar_t ch : text) {
        if (ch < L'0' || ch > L'9') return std::nullopt;
        const auto digit = static_cast<unsigned>(ch - L'0');
        if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) return std::nullopt;
        value = value * 10 + digit;
    }
    return value;
}

std::optional<std::int64_t> parseCounter(Name text) {
    const bool negative = text.starts_with(L"-");
    if (negative || text.starts_with(L"+")) text.remove_prefix(1);
    const auto magnitude = parseNumber(text);
    if (!magnitude || *magnitude > signBit - !negative) return std::nullopt;
    if (*magnitude == signBit) return std::numeric_limits<std::int64_t>::min();
    return negative ? -static_cast<std::int64_t>(*magnitude) : static_cast<std::int64_t>(*magnitude);
}

int compareNames(Name a, Name b) {
    const auto count = std::min(a.size(), b.size());
    for (std::size_t i = 0; i < count; ++i) {
        const wchar_t left = a[i];
        const wchar_t right = b[i];
        if (left == right) continue;
        if (left > 127 || right > 127) {
            const auto order = CompareStringOrdinal(a.data(), static_cast<int>(a.size()),
                                                    b.data(), static_cast<int>(b.size()), TRUE);
            return order - CSTR_EQUAL;
        }
        const auto x = foldAscii(left);
        const auto y = foldAscii(right);
        if (x != y) return x < y ? -1 : 1;
    }
    return (a.size() > b.size()) - (a.size() < b.size());
}

std::vector<Index> sortDirectory(const Directory& dir, Sort sort, Progress& progress) {
    progress.phase = Phase::Sort;
    std::vector<Index> order(dir.entries.size());
    std::iota(order.begin(), order.end(), Index{0});
    unsigned checks = 0;
    std::sort(order.begin(), order.end(), [&](Index left, Index right) {
        if ((++checks & 8191) == 0 && progress.cancel) throw Cancelled{};
        const auto& a = dir.entries[left];
        const auto& b = dir.entries[right];
        if (a.folder() != b.folder()) return a.folder();
        int cmp = 0;
        if (sort.column == Column::Type) cmp = compareNames(dir.extension(left), dir.extension(right));
        if (sort.column == Column::Size) cmp = (a.size > b.size) - (a.size < b.size);
        if (sort.column != Column::Name && cmp) return sort.descending ? cmp > 0 : cmp < 0;
        cmp = compareNames(dir.name(left), dir.name(right));
        if (!cmp) cmp = dir.name(left).compare(dir.name(right));
        return sort.column == Column::Name && sort.descending ? cmp > 0 : cmp < 0;
    });
    return order;
}

Listing readDirectory(std::wstring path, Sort sort, Progress& progress) {
    auto dir = std::make_shared<Directory>();
    dir->path = std::move(path);
    if (!readBuffered(*dir, progress)) readWithFind(*dir, progress);
    if (progress.cancel) throw Cancelled{};
    return {dir, sortDirectory(*dir, sort, progress)};
}

std::wstring renameOptionsError(const RenameOptions& options, std::size_t count, Language language) {
    if (options.stem.empty() || !validMask(options.stem) || !validMask(options.extension)) {
        return translated(language, L"Masks support [N] (name), [E] (extension), and [C] (counter).", L"В масках доступны [N] (имя), [E] (тип) и [C] (счётчик).");
    }
    if (!options.step || !options.digits || options.digits > RenameOptions::maxDigits) {
        return translated(language, L"Use a nonzero step and a counter width of 1 to 15 digits.", L"Шаг должен быть ненулевым. Ширина счётчика от 1 до 15.");
    }
    const auto biasedStart = static_cast<std::uint64_t>(options.start) ^ signBit;
    const auto step = static_cast<std::uint64_t>(options.step);
    const auto maxSteps = options.step > 0 ? (~biasedStart / step) : (biasedStart / (0 - step));
    if (count && count - 1 > maxSteps) return translated(language, L"The final counter value is out of range.", L"Последнее значение счётчика вне допустимого диапазона.");
    return {};
}

std::wstring targetName(const Directory& dir, Index index, const RenameOptions& options, std::size_t position) {
    const auto bits = static_cast<std::uint64_t>(options.start) + position * static_cast<std::uint64_t>(options.step);
    auto counter = std::to_wstring(std::bit_cast<std::int64_t>(bits));
    const std::size_t sign = counter.starts_with(L"-") ? 1 : 0;
    if (counter.size() - sign < options.digits) counter.insert(sign, options.digits - (counter.size() - sign), L'0');
    std::wstring result;
    expand(result, options.stem, dir.stem(index), dir.extension(index), counter);
    const auto stemEnd = result.size();
    if (!stemEnd) return {};
    result += L'.';
    expand(result, options.extension, dir.stem(index), dir.extension(index), counter);
    if (result.size() == stemEnd + 1) result.resize(stemEnd);
    return result;
}

OperationResult renameFiles(const Directory& dir, const std::vector<Index>& selected,
                            const RenameOptions& options, Progress& progress, Language language) {
    OperationResult result;
    progress.phase = Phase::Validate;
    progress.total = selected.size();
    result.rejected = renameOptionsError(options, selected.size(), language);
    if (!result.rejected.empty()) return result;

    std::vector<Move> moves;
    moves.reserve(selected.size());
    std::vector<wchar_t> targets;
    std::vector<Index> bySource(dir.entries.size(), none);
    std::bitset<256> lengths;
    for (std::size_t i = 0; i < selected.size(); ++i) {
        if (progress.cancel) { result.cancelled = true; return result; }
        const auto index = selected[i];
        if ((i & 1023) == 0) progress.done = i;
        if (dir.entries[index].folder()) {
            result.rejected = translated(language, L"Batch rename is available for files only.", L"Групповое переименование доступно для файлов.");
            return result;
        }
        auto name = targetName(dir, index, options, i);
        if (name == dir.name(index)) continue;
        if (!validTarget(name)) {
            result.rejected = translated(language, L"Invalid new name for \"", L"Недопустимое новое имя для «") +
                std::wstring(dir.name(index)) + translated(language, L"\"\n", L"»\n") + name;
            return result;
        }
        if (targets.size() + name.size() > none) throw static_cast<DWORD>(ERROR_NOT_ENOUGH_MEMORY);
        bySource[index] = static_cast<Index>(moves.size());
        moves.push_back({index, static_cast<Index>(targets.size()), static_cast<std::uint16_t>(name.size())});
        targets.insert(targets.end(), name.begin(), name.end());
        lengths.set(name.size());
    }

    if (moves.empty()) return result;
    auto target = [&](Index index) -> Name {
        const auto& move = moves[index];
        return {targets.data() + move.targetOffset, move.targetLength};
    };

    std::vector<Index> byTarget(moves.size());
    std::iota(byTarget.begin(), byTarget.end(), Index{0});
    try {
        unsigned checks = 0;
        auto less = [&](Index a, Index b) {
            if ((++checks & 8191) == 0 && progress.cancel) throw Cancelled{};
            return compareNames(target(a), target(b)) < 0;
        };
        if (!std::is_sorted(byTarget.begin(), byTarget.end(), less)) {
            std::sort(byTarget.begin(), byTarget.end(), less);
        }
    } catch (const Cancelled&) {
        result.cancelled = true;
        return result;
    }

    for (std::size_t i = 1; i < byTarget.size(); ++i) {
        if ((i & 1023) == 0 && progress.cancel) { result.cancelled = true; return result; }
        if (!compareNames(target(byTarget[i - 1]), target(byTarget[i]))) {
            result.rejected = translated(language, L"Multiple files would have the same name: ", L"Одинаковое новое имя у нескольких файлов: ") + std::wstring(target(byTarget[i]));
            return result;
        }
    }

    for (Index i = 0; i < dir.entries.size(); ++i) {
        if ((i & 1023) == 0 && progress.cancel) { result.cancelled = true; return result; }
        const auto current = dir.name(i);
        if (current.size() >= lengths.size() || !lengths.test(current.size())) continue;
        const auto match = std::lower_bound(byTarget.begin(), byTarget.end(), current,
            [&](Index index, Name value) { return compareNames(target(index), value) < 0; });
        if (match == byTarget.end() || compareNames(target(*match), current) != 0) continue;
        if (moves[*match].source == i) continue;
        if (bySource[i] == none) {
            result.rejected = translated(language, L"This name already exists: ", L"Имя уже занято: ") + std::wstring(current);
            return result;
        }
        moves[*match].dependency = bySource[i];
    }

    progress.done = 0;
    progress.total = moves.size();
    progress.phase = Phase::Rename;
    FilePath from(dir.path);
    FilePath to(dir.path);
    auto moveFile = [&](Name source, Name destination) -> DWORD {
        if (MoveFileExW(from.set(source), to.set(destination), 0)) return ERROR_SUCCESS;
        return GetLastError();
    };
    auto original = [&](Index index) { return dir.name(moves[index].source); };
    auto fail = [&](Index index, Name reason) {
        moves[index].state = Move::State::Failed;
        ++result.failed;
        recordError(result, original(index), reason);
    };

    std::vector<Index> chain;
    for (Index first = 0; first < moves.size(); ++first) {
        if (progress.cancel) { result.cancelled = true; break; }
        if (moves[first].state != Move::State::Pending) continue;
        chain.clear();
        Index next = first;
        while (next != none && moves[next].state == Move::State::Pending) {
            if (progress.cancel) { result.cancelled = true; return result; }
            moves[next].state = Move::State::Visiting;
            chain.push_back(next);
            next = moves[next].dependency;
        }

        if (next != none && moves[next].state == Move::State::Visiting) {
            // Unique destinations imply that a cycle has no incoming chain.
            GUID guid{};
            wchar_t token[40]{};
            DWORD error = ERROR_GEN_FAILURE;
            std::wstring temp;
            if (SUCCEEDED(CoCreateGuid(&guid)) && StringFromGUID2(guid, token, static_cast<int>(std::size(token)))) {
                temp = L".notfar-" + std::wstring(token) + L".tmp";
                error = moveFile(original(first), temp);
            }
            const bool staged = error == ERROR_SUCCESS;
            std::size_t remaining = chain.size();
            while (!error && remaining > 1) {
                const Index index = chain[remaining - 1];
                error = moveFile(original(index), target(index));
                if (!error) --remaining;
            }
            if (!error) error = moveFile(temp, target(first));
            if (error) {
                for (std::size_t i = remaining; i < chain.size(); ++i) {
                    const Index index = chain[i];
                    const DWORD rollback = moveFile(target(index), original(index));
                    if (rollback) recordError(result, target(index), windowsError(rollback, language));
                }
                if (staged) {
                    const DWORD rollback = moveFile(temp, original(first));
                    if (rollback) recordError(result, temp, windowsError(rollback, language));
                }
                const auto reason = windowsError(error, language);
                for (Index index : chain) fail(index, reason);
            } else {
                for (Index index : chain) moves[index].state = Move::State::Done;
                result.changed += chain.size();
            }
        } else {
            for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
                if (progress.cancel) { result.cancelled = true; return result; }
                const Index index = *it;
                const Index dependency = moves[index].dependency;
                if (dependency != none && moves[dependency].state != Move::State::Done) {
                    fail(index, translated(language, L"Could not free the destination name.", L"Не удалось освободить новое имя."));
                } else if (const DWORD error = moveFile(original(index), target(index))) {
                    fail(index, result.errors.size() < 6 ? windowsError(error, language) : std::wstring{});
                } else {
                    moves[index].state = Move::State::Done;
                    ++result.changed;
                }
                if (((result.changed + result.failed) & 127) == 0) progress.done = result.changed + result.failed;
            }
        }
        if (((result.changed + result.failed) & 127) == 0 || chain.size() > 1) {
            progress.done = result.changed + result.failed;
        }
    }
    return result;
}

DWORD renameFile(const Directory& dir, Index file, Name target) {
    if (dir.entries[file].folder() || !validTarget(target)) return ERROR_INVALID_NAME;
    if (dir.name(file) == target) return ERROR_SUCCESS;
    FilePath from(dir.path);
    FilePath to(dir.path);
    if (MoveFileExW(from.set(dir.name(file)), to.set(target), 0)) return ERROR_SUCCESS;
    return GetLastError();
}

namespace {

bool retryableDeleteError(DWORD error) {
    return error == ERROR_ACCESS_DENIED || error == ERROR_PRIVILEGE_NOT_HELD ||
           error == ERROR_ELEVATION_REQUIRED;
}

DWORD deleteReadOnlyFile(const wchar_t* path) {
    const DWORD attributes = GetFileAttributesW(path);
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) ||
        !(attributes & FILE_ATTRIBUTE_READONLY)) return ERROR_ACCESS_DENIED;

    // Keep the original attributes on other hard links and on files left after an error.
    FileHandle handle{CreateFileW(path, DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                 nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
    if (handle.value == INVALID_HANDLE_VALUE) return GetLastError();

    FILE_DISPOSITION_INFO_EX disposition{FILE_DISPOSITION_FLAG_DELETE | FILE_DISPOSITION_FLAG_IGNORE_READONLY_ATTRIBUTE};
    if (SetFileInformationByHandle(handle.value, FileDispositionInfoEx, &disposition, sizeof(disposition))) {
        return ERROR_SUCCESS;
    }
    const DWORD error = GetLastError();
    if (error == ERROR_INVALID_PARAMETER || error == ERROR_NOT_SUPPORTED || error == ERROR_INVALID_FUNCTION ||
        error == ERROR_CALL_NOT_IMPLEMENTED) return ERROR_ACCESS_DENIED;
    return error;
}

bool cancelledShellOperation(HRESULT status) {
    return status == HRESULT_FROM_WIN32(ERROR_CANCELLED) ||
           status == COPYENGINE_E_USER_CANCELLED || status == COPYENGINE_E_CANCELLED;
}

struct DeleteRetry {
    Index index;
    DWORD error;
    bool queued = false;
};

template<class Interface>
struct ReleaseInterface {
    void operator()(Interface* object) const { object->Release(); }
};

template<class Interface>
using InterfacePtr = std::unique_ptr<Interface, ReleaseInterface<Interface>>;

class DeleteProgress final : public IFileOperationProgressSink {
public:
    explicit DeleteProgress(Progress& progress) : progress(progress) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override {
        *out = nullptr;
        if (id != IID_IUnknown && id != __uuidof(IFileOperationProgressSink)) return E_NOINTERFACE;
        *out = static_cast<IFileOperationProgressSink*>(this);
        AddRef();
        return S_OK;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override {
        const auto left = --refs;
        if (!left) delete this;
        return left;
    }

    HRESULT STDMETHODCALLTYPE PreDeleteItem(DWORD, IShellItem* item) override {
        if (progress.cancel) return HRESULT_FROM_WIN32(ERROR_CANCELLED);
        SFGAOF attributes = 0;
        if (SUCCEEDED(item->GetAttributes(SFGAO_FOLDER, &attributes)) && (attributes & SFGAO_FOLDER)) {
            return HRESULT_FROM_WIN32(ERROR_DIRECTORY);
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE PostDeleteItem(DWORD, IShellItem*, HRESULT status, IShellItem*) override {
        cancelled = cancelled || cancelledShellOperation(status);
        ++progress.done;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE UpdateProgress(UINT, UINT) override {
        return progress.cancel ? HRESULT_FROM_WIN32(ERROR_CANCELLED) : S_OK;
    }

    HRESULT STDMETHODCALLTYPE StartOperations() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE FinishOperations(HRESULT status) override {
        cancelled = cancelled || cancelledShellOperation(status);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE PreRenameItem(DWORD, IShellItem*, LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PostRenameItem(DWORD, IShellItem*, LPCWSTR, HRESULT, IShellItem*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PreMoveItem(DWORD, IShellItem*, IShellItem*, LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PostMoveItem(DWORD, IShellItem*, IShellItem*, LPCWSTR, HRESULT, IShellItem*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PreCopyItem(DWORD, IShellItem*, IShellItem*, LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PostCopyItem(DWORD, IShellItem*, IShellItem*, LPCWSTR, HRESULT, IShellItem*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PreNewItem(DWORD, IShellItem*, LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PostNewItem(DWORD, IShellItem*, LPCWSTR, LPCWSTR, DWORD, HRESULT, IShellItem*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE ResetTimer() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PauseTimer() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE ResumeTimer() override { return S_OK; }

    bool cancelled = false;

private:
    ~DeleteProgress() = default;
    Progress& progress;
    std::atomic<ULONG> refs{1};
};

void retryDeletion(const Directory& dir, std::vector<DeleteRetry>& retries, OperationResult& result,
                   Progress& progress, HWND owner) {
    // The original delete was already confirmed. Windows handles any additional UAC consent.
    const struct Apartment {
        HRESULT status = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        ~Apartment() { if (SUCCEEDED(status)) CoUninitialize(); }
    } apartment;
    if (FAILED(apartment.status)) return;

    IFileOperation* rawOperation = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&rawOperation)))) return;
    const InterfacePtr<IFileOperation> operation(rawOperation);
    if (owner) operation->SetOwnerWindow(owner);
    constexpr DWORD flags = FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_NORECURSION |
                            FOF_FILESONLY | FOF_NO_CONNECTED_ELEMENTS | FOFX_SHOWELEVATIONPROMPT;
    if (FAILED(operation->SetOperationFlags(flags))) return;

    const InterfacePtr<DeleteProgress> sink(new DeleteProgress(progress));
    DWORD cookie = 0;
    if (FAILED(operation->Advise(sink.get(), &cookie))) return;

    progress.done = 0;
    progress.total = retries.size();
    progress.phase = Phase::DeleteWithPermissions;
    bool hasWork = false;
    for (auto& retry : retries) {
        if (progress.cancel) break;
        const auto path = childPath(dir.path, dir.name(retry.index));
        IShellItem* rawItem = nullptr;
        if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&rawItem)))) continue;
        const InterfacePtr<IShellItem> item(rawItem);
        PWSTR parsedPath = nullptr;
        if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &parsedPath))) continue;
        const Name parsed(parsedPath);
        const bool sameFile = compareNames(parsed, path) == 0 && parsed.ends_with(dir.name(retry.index));
        CoTaskMemFree(parsedPath);
        if (!sameFile) continue;

        SFGAOF attributes = 0;
        if (SUCCEEDED(item->GetAttributes(SFGAO_FOLDER, &attributes)) && (attributes & SFGAO_FOLDER)) {
            retry.error = ERROR_DIRECTORY;
            continue;
        }
        if (SUCCEEDED(operation->DeleteItem(item.get(), nullptr))) {
            retry.queued = true;
            hasWork = true;
        }
    }

    HRESULT status = S_OK;
    const bool performed = hasWork && !progress.cancel;
    if (performed) status = operation->PerformOperations();
    operation->Unadvise(cookie);
    if (progress.cancel || sink->cancelled || cancelledShellOperation(status)) {
        result.cancelled = true;
    }
    if (!performed) return;

    FilePath path(dir.path);
    for (auto& retry : retries) {
        if (!retry.queued) continue;
        if (GetFileAttributesW(path.set(dir.name(retry.index))) != INVALID_FILE_ATTRIBUTES) continue;
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
            retry.error = ERROR_SUCCESS;
            ++result.changed;
        }
    }
}

} // namespace

OperationResult deleteFiles(const Directory& dir, const std::vector<Index>& selected,
                            Progress& progress, Language language, HWND owner) {
    OperationResult result;
    progress.phase = Phase::Delete;
    progress.total = selected.size();
    FilePath path(dir.path);
    std::vector<DeleteRetry> retries;
    std::size_t processed = 0;

    auto fail = [&](Index index, DWORD error) {
        ++result.failed;
        if (result.errors.size() < 6) recordError(result, dir.name(index), windowsError(error, language));
    };
    for (Index index : selected) {
        if (progress.cancel) { result.cancelled = true; break; }
        if (dir.entries[index].folder()) {
            ++result.failed;
            recordError(result, dir.name(index), translated(language, L"Only files can be deleted.", L"Удаление доступно для файлов."));
        } else {
            const auto full = path.set(dir.name(index));
            DWORD error = DeleteFileW(full) ? ERROR_SUCCESS : GetLastError();
            if (error == ERROR_ACCESS_DENIED) error = deleteReadOnlyFile(full);
            if (!error) {
                ++result.changed;
            } else if (owner && retryableDeleteError(error)) {
                retries.push_back({index, error});
            } else {
                fail(index, error);
            }
        }
        if ((++processed & 127) == 0) progress.done = processed;
    }
    if (!retries.empty() && !progress.cancel) retryDeletion(dir, retries, result, progress, owner);
    for (const auto& retry : retries) if (retry.error) fail(retry.index, retry.error);
    if (progress.cancel) result.cancelled = true;
    return result;
}

} // namespace nf
