#pragma once

#include "files.hpp"

#include <span>

namespace nf {

struct SelectionInfo {
    std::uint64_t bytes = 0;
    std::size_t files = 0;
    std::size_t folders = 0;
    std::size_t links = 0;
};

enum class LinkKind { Hard, Symbolic, Junction };
enum class TimeKind { Modified, Created, Accessed, Changed };

Listing findSmallFiles(std::wstring path, std::uint64_t limit, Sort sort, Progress& progress);
OperationResult deleteFiles(const Directory& dir, const std::vector<Index>& selected, Progress& progress,
                            Language language = Language::English, bool wipe = false);
OperationResult transferItems(const Directory& dir, std::span<const Index> selected, Name destination,
                              bool move, Progress& progress, Language language, bool copyPermissions = false);
OperationResult pasteItems(std::span<const std::wstring> paths, Name destination, bool move,
                           Progress& progress, Language language, bool copyPermissions = false);
OperationResult createLinks(const Directory& dir, std::span<const Index> selected, Name destination,
                            LinkKind kind, Progress& progress, Language language);
OperationResult changeAttributes(const Directory& dir, std::span<const Index> selected, DWORD set, DWORD clear,
                                 Progress& progress, Language language);
OperationResult changeTime(const Directory& dir, std::span<const Index> selected, FILETIME time, TimeKind kind,
                           Progress& progress, Language language);
OperationResult applyCommand(const Directory& dir, std::span<const Index> selected, Name command,
                             Progress& progress, Language language);
OperationResult describeItems(const Directory& dir, std::span<const Index> selected, Name description,
                              Progress& progress, Language language);
OperationResult createArchive(const Directory& dir, std::span<const Index> selected, Name destination,
                              Progress& progress, Language language);
OperationResult processArchives(const Directory& dir, std::span<const Index> selected, Name destination, bool extract,
                                Progress& progress, Language language);
std::wstring listArchive(Name path, Language language);
SelectionInfo inspectItems(const Directory& dir, std::span<const Index> selected, Progress& progress);
bool matchesMask(Name pattern, Name name);
std::optional<FILETIME> parseLocalTime(Name value);

}
