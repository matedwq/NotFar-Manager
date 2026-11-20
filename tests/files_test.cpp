#include "../src/files.hpp"

#include <array>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace {
using namespace nf;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

void check(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

struct Fixture {
    fs::path path = fs::temp_directory_path() /
        (L"notfar-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));

    Fixture() { fs::create_directory(path); }
    ~Fixture() {
        std::error_code ignored;
        fs::remove_all(L"\\\\?\\" + path.wstring(), ignored);
    }

    void file(Name name, DWORD bytes, char contents = 'A') {
        const auto full = L"\\\\?\\" + childPath(path.wstring(), name);
        HANDLE file = CreateFileW(full.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        check(file != INVALID_HANDLE_VALUE, "create fixture file");
        std::string data(bytes, contents);
        DWORD written = 0;
        const bool ok = WriteFile(file, data.data(), bytes, &written, nullptr) && written == bytes;
        CloseHandle(file);
        check(ok, "write fixture file");
    }

    Listing read(Sort sort = {}) const {
        Progress progress;
        return readDirectory(path.wstring(), sort, progress);
    }

    bool exists(Name name) const { return fs::exists(path / name); }

    char firstByte(Name name) const {
        const auto full = childPath(path.wstring(), name);
        HANDLE file = CreateFileW(full.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        check(file != INVALID_HANDLE_VALUE, "open renamed file");
        char byte = 0;
        DWORD count = 0;
        const bool ok = ReadFile(file, &byte, 1, &count, nullptr) && count == 1;
        CloseHandle(file);
        check(ok, "read renamed file");
        return byte;
    }
};

Index find(const Directory& dir, Name name) {
    for (Index i = 0; i < dir.entries.size(); ++i) if (dir.name(i) == name) return i;
    throw std::runtime_error("fixture file missing from listing");
}

void listingAndDelete() {
    Fixture f;
    fs::create_directory(f.path / L"folder");
    f.file(L"альфа.PNG", 2047);
    f.file(L"Beta.png", 2048);
    f.file(L"a.bin", 0);
    f.file(L".env", 17);
    const auto bySize = f.read({Column::Size, false});
    const auto& dir = *bySize.directory;
    check(dir.fileCount == 4 && dir.entries.size() == 5, "enumerate files and folders");
    check(dir.entries[bySize.order.front()].folder(), "folders first");
    check(dir.name(bySize.order[1]) == L"a.bin" && dir.name(bySize.order.back()) == L"Beta.png", "size sort");
    check(dir.extension(find(dir, L".env")).empty(), "leading dot without extension");
    check(dir.extension(find(dir, L"альфа.PNG")) == L"PNG", "original extension");
    const auto byType = f.read({Column::Type, true});
    check(byType.directory->name(byType.order[1]) == L"Beta.png", "type sort and stable tie breaker");

    std::vector<Index> belowLimit;
    for (const auto index : bySize.order) {
        if (!dir.entries[index].folder() && dir.entries[index].size < 2048) belowLimit.push_back(index);
    }
    Progress progress;
    auto result = deleteFiles(dir, belowLimit, progress);
    check(result.changed == 3 && !result.failed && f.exists(L"Beta.png") && f.exists(L"folder"), "strict size cutoff and file-only deletion");
    progress.cancel = true;
    result = deleteFiles(dir, {find(dir, L"Beta.png")}, progress);
    check(!result.changed && result.cancelled && f.exists(L"Beta.png"), "delete cancellation");
}

void readOnlyDelete() {
    Fixture f;
    f.file(L"readonly.txt", 1);
    const auto file = f.path / L"readonly.txt";
    check(SetFileAttributesW(file.c_str(), FILE_ATTRIBUTE_READONLY), "set read-only attribute");
    const auto listing = f.read();

    HANDLE locked = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    check(locked != INVALID_HANDLE_VALUE, "open read-only file without sharing deletion");
    Progress progress;
    const auto blocked = deleteFiles(*listing.directory, listing.order, progress);
    CloseHandle(locked);
    check(blocked.changed == 0 && blocked.failed == 1 && f.exists(L"readonly.txt") &&
          (GetFileAttributesW(file.c_str()) & FILE_ATTRIBUTE_READONLY), "blocked deletion preserves file and attributes");

    const auto removed = deleteFiles(*listing.directory, listing.order, progress);
    const bool exists = f.exists(L"readonly.txt");
    if (exists) SetFileAttributesW(file.c_str(), FILE_ATTRIBUTE_NORMAL);
    check(removed.changed == 1 && removed.failed == 0 && !exists, "delete a read-only file");
}

void normalRename() {
    Fixture f;
    f.file(L"z.PNG", 12, 'Z');
    f.file(L"a.jpg", 10, 'A');
    f.file(L".env", 7, 'E');
    const auto listing = f.read({Column::Size, true});
    const auto& dir = *listing.directory;
    RenameOptions options;
    options.start = 7;
    options.step = 3;
    Progress progress;
    const auto result = renameFiles(dir, listing.order, options, progress);
    check(result.changed == 3 && !result.failed, "rename in display order");
    check(f.firstByte(L"00000007.PNG") == 'Z' && f.firstByte(L"00000010.jpg") == 'A' &&
          f.firstByte(L"00000013") == 'E', "width, start, step and extensions");
    options.start = 1;
    options.step = -2;
    options.digits = 4;
    check(targetName(dir, listing.order[1], options, 1) == L"-0001.jpg", "negative counter");
    options.digits = 15;
    check(renameOptionsError(options, 2).empty() &&
          targetName(dir, listing.order[1], options, 1) == L"-000000000000001.jpg", "15-digit counter padding");
    options.digits = 16;
    check(!renameOptionsError(options, 2).empty(), "counter width above the dropdown range is rejected");
    options.digits = 4;
    options.start = INT64_MIN;
    check(!renameOptionsError(options, 2).empty(), "negative counter overflow");
    options.start = INT64_MAX;
    options.step = 1;
    check(!renameOptionsError(options, 2).empty(), "positive counter overflow");
    check(parseCounter(L"-9223372036854775808") == INT64_MIN && !parseCounter(L"9223372036854775808"), "parse counter limits");
}

void singleRename() {
    Fixture f;
    f.file(L"original.txt", 1, 'A');
    f.file(L"existing.txt", 1, 'B');
    const auto listing = f.read();
    const auto& dir = *listing.directory;
    const auto source = find(dir, L"original.txt");

    check(renameFile(dir, source, L"..\\outside.txt") == ERROR_INVALID_NAME, "rename takes a filename");
    check(renameFile(dir, source, L"existing.txt") != ERROR_SUCCESS &&
          f.firstByte(L"original.txt") == 'A' && f.firstByte(L"existing.txt") == 'B',
          "single rename protects existing files");
    check(renameFile(dir, source, L"[C] отчёт.txt") == ERROR_SUCCESS &&
          !f.exists(L"original.txt") && f.firstByte(L"[C] отчёт.txt") == 'A',
          "single rename treats brackets and Unicode as literal text");
}

void conflictsAndCycles() {
    Fixture f;
    f.file(L"1.txt", 1, 'A');
    f.file(L"2.txt", 1, 'B');
    const auto listing = f.read();
    const auto& dir = *listing.directory;
    const auto one = find(dir, L"1.txt");
    const auto two = find(dir, L"2.txt");
    Progress progress;
    RenameOptions options;
    options.digits = 1;
    options.start = 2;
    auto result = renameFiles(dir, {one}, options, progress);
    check(result.rejected.starts_with(L"This name already exists:") && f.firstByte(L"2.txt") == 'B',
          "existing target is protected with an English diagnostic by default");
    result = renameFiles(dir, {one}, options, progress, Language::Russian);
    check(result.rejected.starts_with(L"Имя уже занято:") && f.firstByte(L"2.txt") == 'B',
          "existing target is protected with a Russian diagnostic");

    options.stem = L"same";
    result = renameFiles(dir, {one, two}, options, progress);
    check(!result.rejected.empty() && !f.exists(L"same.txt"), "duplicate target rejected");

    options.stem = L"[C]";
    HANDLE locked = CreateFileW((f.path / L"2.txt").c_str(), GENERIC_READ, FILE_SHARE_READ,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    check(locked != INVALID_HANDLE_VALUE, "hold file open without FILE_SHARE_DELETE");
    options.step = -1;
    result = renameFiles(dir, {one, two}, options, progress);
    CloseHandle(locked);
    check(result.failed == 2 && !result.changed && f.read().directory->fileCount == 2 &&
          f.firstByte(L"1.txt") == 'A' && f.firstByte(L"2.txt") == 'B', "cycle rollback on I/O failure");

    result = renameFiles(dir, {one, two}, options, progress);
    check(result.changed == 2 && !result.failed && f.read().directory->fileCount == 2 &&
          f.firstByte(L"1.txt") == 'B' && f.firstByte(L"2.txt") == 'A', "swap without overwriting");

    options.step = 1;
    result = renameFiles(dir, {one, two}, options, progress);
    check(result.changed == 2 && f.firstByte(L"2.txt") == 'B' && f.firstByte(L"3.txt") == 'A', "rename dependency chain");

    const auto newer = f.read();
    f.file(L"4.txt", 1, 'C');
    options.start = 4;
    result = renameFiles(*newer.directory, {find(*newer.directory, L"3.txt")}, options, progress);
    check(result.failed == 1 && f.firstByte(L"4.txt") == 'C' && f.firstByte(L"3.txt") == 'A', "late conflict does not overwrite");
}

void longPaths() {
    Fixture f;
    const auto level = std::wstring(118, L'я');
    auto path = f.path.wstring();
    for (int i = 0; i < 3; ++i) {
        path = childPath(path, level);
        check(CreateDirectoryW((L"\\\\?\\" + path).c_str(), nullptr), "create deep test path");
    }
    const auto full = L"\\\\?\\" + childPath(path, L"Юникод.txt");
    HANDLE handle = CreateFileW(full.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    check(handle != INVALID_HANDLE_VALUE, "long Unicode filename");
    CloseHandle(handle);
    Progress progress;
    check(absolutePath(path) == path && parentPath(path).size() < path.size(), "normalize long path");
    const auto listing = readDirectory(path, {}, progress);
    check(listing.directory->fileCount == 1 && listing.directory->name(0) == L"Юникод.txt", "read long Unicode path");
    const auto renamed = renameFiles(*listing.directory, listing.order, {}, progress);
    check(renamed.changed == 1, "rename long Unicode path");
    const auto fresh = readDirectory(path, {}, progress);
    check(deleteFiles(*fresh.directory, fresh.order, progress).changed == 1, "delete long Unicode path");
}

void scan(Name path, std::size_t expected) {
    const auto from = Clock::now();
    Progress progress;
    const auto listing = readDirectory(absolutePath(path), {}, progress);
    const auto listed = Clock::now();
    const auto order = sortDirectory(*listing.directory, {Column::Size, true}, progress);
    const auto sorted = Clock::now();
    check(listing.directory->fileCount == expected && order.size() == expected, "large directory count");
    using Ms = std::chrono::milliseconds;
    std::cout << "Files: " << expected << "\nRead + name sort: "
              << std::chrono::duration_cast<Ms>(listed - from).count() << " ms\nSize sort: "
              << std::chrono::duration_cast<Ms>(sorted - listed).count() << " ms\n";
}
void dataset(std::size_t count) {
    Fixture fixture;
    const auto base = fixture.path.wstring();
    const auto from = Clock::now();
    for (std::size_t i = 0; i < count; ++i) {
        const auto name = childPath(base, L"export_frame_" + std::to_wstring(10000000 + i) + L".png");
        HANDLE handle = CreateFileW(name.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        check(handle != INVALID_HANDLE_VALUE, "create large directory fixture");
        CloseHandle(handle);
    }
    std::cout << "Fixture creation: "
              << std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - from).count() << " ms\n";
    scan(base, count);
}

void syntheticSort(std::size_t count) {
    Directory dir;
    dir.entries.reserve(count);
    dir.names.reserve(count * 24);
    for (std::size_t i = 0; i < count; ++i) {
        const auto name = L"export_frame_" + std::to_wstring(10000000 + (i * 65537 % count)) + L".png";
        dir.add(name, 1000 + (i % 3000), false);
    }
    Progress progress;
    const auto from = Clock::now();
    const auto order = sortDirectory(dir, {Column::Size, false}, progress);
    check(order.size() == count, "synthetic count");
    for (std::size_t i = 1; i < order.size(); ++i) {
        check(dir.entries[order[i - 1]].size <= dir.entries[order[i]].size, "synthetic size order");
    }
    std::cout << "Synthetic size sort (" << count << " entries): "
              << std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - from).count() << " ms\n";
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        if (argc == 4 && Name(argv[1]) == L"--scan") {
            const auto count = parseNumber(argv[3]);
            check(count.has_value(), "expected count");
            scan(argv[2], static_cast<std::size_t>(*count));
        } else if (argc == 3 && (Name(argv[1]) == L"--benchmark" || Name(argv[1]) == L"--synthetic")) {
            const auto count = parseNumber(argv[2]);
            check(count && *count > 0 && *count < 10000000, "benchmark count");
            if (Name(argv[1]) == L"--benchmark") dataset(static_cast<std::size_t>(*count));
            else syntheticSort(static_cast<std::size_t>(*count));
        } else {
            listingAndDelete();
            readOnlyDelete();
            normalRename();
            singleRename();
            conflictsAndCycles();
            longPaths();
            std::cout << "Filesystem checks passed\n";
        }
        return 0;
    } catch (DWORD code) {
        std::wcerr << L"Windows error " << code << L": " << windowsError(code) << L'\n';
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
    }
    return 1;
}
