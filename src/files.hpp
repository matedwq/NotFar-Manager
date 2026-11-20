#pragma once

#include "language.hpp"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace nf {

using Index = std::uint32_t;
using Name = std::wstring_view;

struct Entry {
    std::uint64_t size;
    Index offset;
    std::uint16_t lengthAndKind;
    std::uint16_t stemLength;

    bool folder() const { return (lengthAndKind & 0x8000) != 0; }
    std::uint16_t length() const { return lengthAndKind & 0x7fff; }
};

struct Directory {
    std::wstring path;
    std::vector<Entry> entries;
    std::vector<wchar_t> names;
    std::size_t fileCount = 0;

    void add(Name name, std::uint64_t size, bool folder);
    Name name(Index entry) const;
    Name stem(Index entry) const;
    Name extension(Index entry) const;
};

enum class Column { Name, Type, Size };
struct Sort {
    Column column = Column::Name;
    bool descending = false;
};

enum class Phase { Read, Sort, Validate, Rename, Delete };
struct Progress {
    std::atomic<bool> cancel{false};
    std::atomic<Phase> phase{Phase::Read};
    std::atomic<std::size_t> done{0};
    std::atomic<std::size_t> total{0};
};

struct Listing {
    std::shared_ptr<const Directory> directory;
    std::vector<Index> order;
};

struct RenameOptions {
    std::wstring stem = L"[C]";
    std::wstring extension = L"[E]";
    std::int64_t start = 1;
    std::int64_t step = 1;
    unsigned digits = 8;
};

struct OperationResult {
    std::size_t changed = 0;
    std::size_t failed = 0;
    bool cancelled = false;
    std::wstring rejected;
    std::vector<std::wstring> errors;
};

std::wstring absolutePath(Name input, Name base = {});
std::wstring parentPath(Name path);
std::wstring childPath(Name path, Name name);
std::wstring number(std::uint64_t value);
std::wstring windowsError(DWORD error, Language language = Language::English);
std::optional<std::uint64_t> parseNumber(Name value);
std::optional<std::int64_t> parseCounter(Name value);
int compareNames(Name a, Name b);
Listing readDirectory(std::wstring path, Sort sort, Progress& progress);
std::vector<Index> sortDirectory(const Directory& dir, Sort sort, Progress& progress);
std::wstring renameOptionsError(const RenameOptions& options, std::size_t count,
                                Language language = Language::English);
DWORD renameFile(const Directory& dir, Index file, Name target);
std::wstring targetName(const Directory& dir, Index entry, const RenameOptions& options, std::size_t position);
OperationResult renameFiles(const Directory& dir, const std::vector<Index>& selected,
                            const RenameOptions& options, Progress& progress, Language language = Language::English);
OperationResult deleteFiles(const Directory& dir, const std::vector<Index>& selected,
                            Progress& progress, Language language = Language::English);

}
