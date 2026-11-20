#include "operations.hpp"
#include "text_files.hpp"
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#include "../third_party/miniz/miniz.h"

#include <winioctl.h>
#include <aclapi.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <cwctype>
#include <limits>
#include <unordered_map>

namespace nf {
namespace {

struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    ~Handle() { if (value != INVALID_HANDLE_VALUE && value) CloseHandle(value); }
};

struct CloseHandleDeleter {
    void operator()(void* handle) const { CloseHandle(handle); }
};

struct FindHandle {
    void operator()(void* handle) const { FindClose(handle); }
};

struct Node {
    DWORD attributes;
    std::uint64_t size;

    bool folder() const { return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0; }
    bool link() const { return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0; }
};

enum class Visit { Entry, EndFolder };

std::uint64_t fileSize(DWORD high, DWORD low) {
    return (static_cast<std::uint64_t>(high) << 32) | low;
}

DWORD openTreeFolder(const std::wstring& path, std::unique_ptr<void, CloseHandleDeleter>& pin) {
    const auto directory = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (directory == INVALID_HANDLE_VALUE) return GetLastError();
    pin.reset(directory);
    FILE_ATTRIBUTE_TAG_INFO info{};
    if (!GetFileInformationByHandleEx(directory, FileAttributeTagInfo, &info, sizeof(info))) return GetLastError();
    if (!(info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        return ERROR_DIRECTORY;
    }
    return ERROR_SUCCESS;
}

template<class Action>
DWORD walkTree(const std::wstring& root, Progress& progress, Action action, std::size_t* skipped = nullptr) {
    if (progress.cancel) return ERROR_CANCELLED;
    WIN32_FILE_ATTRIBUTE_DATA first{};
    if (!GetFileAttributesExW(root.c_str(), GetFileExInfoStandard, &first)) return GetLastError();
    struct Frame {
        std::wstring path;
        Node node;
        std::unique_ptr<void, FindHandle> search;
        std::unique_ptr<void, CloseHandleDeleter> pin;
    };
    std::vector<Frame> stack;
    std::wstring path = root;
    Node node{first.dwFileAttributes, fileSize(first.nFileSizeHigh, first.nFileSizeLow)};
    WIN32_FIND_DATAW found{};
    auto skip = [&](DWORD error, const std::wstring& folder) {
        if (!skipped || folder == root) return false;
        switch (error) {
            case ERROR_ACCESS_DENIED: case ERROR_SHARING_VIOLATION:
            case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: case ERROR_DIRECTORY:
                ++*skipped;
                return true;
            default: return false;
        }
    };

    for (;;) {
        if (progress.cancel) return ERROR_CANCELLED;
        if (const auto error = action(path, node, Visit::Entry)) return error;
        if (node.folder() && !node.link()) {
            // Keep the directory name in place while walking its children.
            std::unique_ptr<void, CloseHandleDeleter> opened;
            auto error = openTreeFolder(path, opened);
            HANDLE find = INVALID_HANDLE_VALUE;
            bool empty = false;
            if (!error) {
                const auto searchPattern = childPath(path, L"*");
                find = FindFirstFileExW(searchPattern.c_str(), FindExInfoBasic, &found,
                                        FindExSearchNameMatch, nullptr, skipped ? FIND_FIRST_EX_LARGE_FETCH : 0);
                if (find == INVALID_HANDLE_VALUE) {
                    error = GetLastError();
                    empty = error == ERROR_FILE_NOT_FOUND;
                }
            }
            if (find == INVALID_HANDLE_VALUE) {
                if (!empty && !skip(error, path)) return error;
                opened.reset();
                if (const auto endError = action(path, node, Visit::EndFolder)) return endError;
            } else {
                stack.push_back({std::move(path), node, std::unique_ptr<void, FindHandle>(find), std::move(opened)});
                if (Name(found.cFileName) != L"." && Name(found.cFileName) != L"..") {
                    path = childPath(stack.back().path, found.cFileName);
                    node = {found.dwFileAttributes, fileSize(found.nFileSizeHigh, found.nFileSizeLow)};
                    continue;
                }
            }
        }

        bool next = false;
        while (!stack.empty()) {
            auto& folder = stack.back();
            while (FindNextFileW(folder.search.get(), &found)) {
                if (progress.cancel) return ERROR_CANCELLED;
                if (Name(found.cFileName) == L"." || Name(found.cFileName) == L"..") continue;
                path = childPath(folder.path, found.cFileName);
                node = {found.dwFileAttributes, fileSize(found.nFileSizeHigh, found.nFileSizeLow)};
                next = true;
                break;
            }
            if (next) break;
            const auto error = GetLastError();
            if (error != ERROR_NO_MORE_FILES && !skip(error, folder.path)) return error;
            folder.search.reset();
            folder.pin.reset();
            if (const auto endError = action(folder.path, folder.node, Visit::EndFolder)) return endError;
            stack.pop_back();
        }
        if (!next) return ERROR_SUCCESS;
    }
}

template<class Action>
OperationResult runItems(const Directory& dir, std::span<const Index> selected, Progress& progress,
                         Language language, Phase phase, Action action) {
    OperationResult result;
    progress.phase = phase;
    progress.total = selected.size();
    progress.done = 0;
    std::size_t processed = 0;
    SearchParents parents(dir);
    for (Index index : selected) {
        if (progress.cancel) break;
        const auto name = dir.name(index);
        const auto path = nativePath(childPath(dir.path, name));
        auto error = parents.lock(name);
        if (!error) error = action(index, path);
        if (error == ERROR_CANCELLED) { result.cancelled = true; break; }
        if (error) {
            ++result.failed;
            if (result.errors.size() < 6) {
                result.errors.push_back(std::wstring(name) + L" — " + windowsError(error, language));
            }
        } else {
            ++result.changed;
        }
        ++processed;
        if ((processed & 127) == 0) progress.done = processed;
    }
    progress.done = processed;
    result.cancelled = result.cancelled || progress.cancel;
    return result;
}

DWORD markForDeletion(HANDLE handle) {
    FILE_DISPOSITION_INFO_EX disposition{FILE_DISPOSITION_FLAG_DELETE | FILE_DISPOSITION_FLAG_IGNORE_READONLY_ATTRIBUTE};
    if (SetFileInformationByHandle(handle, FileDispositionInfoEx, &disposition, sizeof(disposition))) return ERROR_SUCCESS;
    const auto error = GetLastError();
    if (error != ERROR_INVALID_PARAMETER && error != ERROR_NOT_SUPPORTED && error != ERROR_INVALID_FUNCTION &&
        error != ERROR_CALL_NOT_IMPLEMENTED) return error;
    FILE_DISPOSITION_INFO fallback{TRUE};
    return SetFileInformationByHandle(handle, FileDispositionInfo, &fallback, sizeof(fallback)) ? ERROR_SUCCESS : GetLastError();
}

DWORD removeFile(const std::wstring& path) {
    if (DeleteFileW(path.c_str())) return ERROR_SUCCESS;
    const auto error = GetLastError();
    if (error != ERROR_ACCESS_DENIED) return error;
    const auto attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) ||
        !(attributes & FILE_ATTRIBUTE_READONLY)) return error;
    const Handle handle{CreateFileW(path.c_str(), DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
    return handle.value != INVALID_HANDLE_VALUE ? markForDeletion(handle.value) : GetLastError();
}

DWORD wipeFile(const std::wstring& path, Progress& progress, std::span<const std::byte> zeros) {
    const Handle handle{CreateFileW(path.c_str(), GENERIC_WRITE | FILE_READ_ATTRIBUTES | DELETE, 0, nullptr,
                                    OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
    if (handle.value == INVALID_HANDLE_VALUE) return GetLastError();
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(handle.value, &info)) return GetLastError();
    if (info.nNumberOfLinks > 1 || (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))) {
        return ERROR_NOT_SUPPORTED;
    }
    auto remaining = fileSize(info.nFileSizeHigh, info.nFileSizeLow);
    while (remaining) {
        if (progress.cancel) return ERROR_CANCELLED;
        const auto count = static_cast<DWORD>(std::min<std::uint64_t>(remaining, zeros.size()));
        DWORD written = 0;
        if (!WriteFile(handle.value, zeros.data(), count, &written, nullptr)) return GetLastError();
        if (!written) return ERROR_WRITE_FAULT;
        remaining -= written;
    }
    if (!FlushFileBuffers(handle.value)) return GetLastError();
    LARGE_INTEGER start{};
    if (!SetFilePointerEx(handle.value, start, nullptr, FILE_BEGIN) || !SetEndOfFile(handle.value) ||
        !FlushFileBuffers(handle.value)) return GetLastError();
    return markForDeletion(handle.value);
}

DWORD removeTree(const std::wstring& root, bool wipe, Progress& progress, std::span<const std::byte> zeros) {
    return walkTree(root, progress, [&](const std::wstring& path, Node node, Visit phase) -> DWORD {
        if (node.folder()) {
            if (!node.link() && phase == Visit::Entry) return ERROR_SUCCESS;
            return RemoveDirectoryW(path.c_str()) ? ERROR_SUCCESS : GetLastError();
        }
        return wipe && !node.link() ? wipeFile(path, progress, zeros) : removeFile(path);
    });
}

std::wstring finalPath(const std::wstring& path) {
    const Handle handle{CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS, nullptr)};
    if (handle.value == INVALID_HANDLE_VALUE) return {};
    const auto needed = GetFinalPathNameByHandleW(handle.value, nullptr, 0, FILE_NAME_NORMALIZED);
    if (!needed) return {};
    std::wstring resolved(needed, L'\0');
    const auto length = GetFinalPathNameByHandleW(handle.value, resolved.data(), needed, FILE_NAME_NORMALIZED);
    if (!length || length >= needed) return {};
    resolved.resize(length);
    return resolved;
}

DWORD CALLBACK copyProgress(LARGE_INTEGER, LARGE_INTEGER, LARGE_INTEGER, LARGE_INTEGER,
                            DWORD, DWORD, HANDLE, HANDLE, LPVOID context) {
    return static_cast<Progress*>(context)->cancel ? PROGRESS_CANCEL : PROGRESS_CONTINUE;
}

DWORD copyDacl(const std::wstring& source, const std::wstring& target) {
    constexpr DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    constexpr DWORD flags = FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT;
    const Handle from{CreateFileW(source.c_str(), READ_CONTROL, share, nullptr, OPEN_EXISTING, flags, nullptr)};
    if (from.value == INVALID_HANDLE_VALUE) return GetLastError();
    const Handle to{CreateFileW(target.c_str(), WRITE_DAC, share, nullptr, OPEN_EXISTING, flags, nullptr)};
    if (to.value == INVALID_HANDLE_VALUE) return GetLastError();
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const auto error = GetSecurityInfo(from.value, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                                       nullptr, nullptr, &dacl, nullptr, &descriptor);
    if (error) return error;
    std::unique_ptr<void, decltype(&LocalFree)> security(descriptor, LocalFree);
    // Preserve inherited source rules as explicit rules on the protected copy.
    for (DWORD i = 0; dacl && i < dacl->AceCount; ++i) {
        void* ace = nullptr;
        if (!GetAce(dacl, i, &ace)) return GetLastError();
        static_cast<ACE_HEADER*>(ace)->AceFlags &= static_cast<BYTE>(~INHERITED_ACE);
    }
    SECURITY_DESCRIPTOR targetSecurity{};
    if (!InitializeSecurityDescriptor(&targetSecurity, SECURITY_DESCRIPTOR_REVISION) ||
        !SetSecurityDescriptorDacl(&targetSecurity, TRUE, dacl, FALSE) ||
        !SetSecurityDescriptorControl(&targetSecurity, SE_DACL_PROTECTED, SE_DACL_PROTECTED) ||
        !SetKernelObjectSecurity(to.value, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                                 &targetSecurity)) {
        return GetLastError();
    }
    return ERROR_SUCCESS;
}

DWORD transferTree(const std::wstring& root, const std::wstring& destination, bool move, Progress& progress,
                   bool copyPermissions) {
    return walkTree(root, progress, [&](const std::wstring& source, Node node, Visit phase) -> DWORD {
        auto path = destination;
        path.append(source, root.size());
        if (phase == Visit::EndFolder) {
            if (move && !RemoveDirectoryW(source.c_str())) return GetLastError();
            return copyPermissions ? copyDacl(source, path) : ERROR_SUCCESS;
        }
        if (node.folder()) {
            if (node.link()) return ERROR_NOT_SUPPORTED;
            return CreateDirectoryW(path.c_str(), nullptr) ? ERROR_SUCCESS : GetLastError();
        }
        if (move) {
            if (!MoveFileWithProgressW(source.c_str(), path.c_str(), copyProgress, &progress, MOVEFILE_COPY_ALLOWED)) {
                return progress.cancel ? ERROR_CANCELLED : GetLastError();
            }
            if (GetFileAttributesW(source.c_str()) != INVALID_FILE_ATTRIBUTES) return ERROR_ACCESS_DENIED;
            const auto error = GetLastError();
            return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? ERROR_SUCCESS : error;
        }
        constexpr DWORD flags = COPY_FILE_FAIL_IF_EXISTS | COPY_FILE_COPY_SYMLINK;
        if (!CopyFileExW(source.c_str(), path.c_str(), copyProgress, &progress, nullptr, flags)) {
            return progress.cancel ? ERROR_CANCELLED : GetLastError();
        }
        return copyPermissions ? copyDacl(source, path) : ERROR_SUCCESS;
    });
}

DWORD createJunction(const std::wstring& destination, Name source) {
    // A mount-point record stores two names after its fixed header.
    struct Header {
        DWORD tag;
        WORD length, reserved, substituteOffset, substituteLength, printOffset, printLength;
    };
    constexpr DWORD maxBytes = MAXIMUM_REPARSE_DATA_BUFFER_SIZE;
    const auto substitution = L"\\??\\" + std::wstring(source);
    const auto size = sizeof(Header) + (substitution.size() + source.size() + 2) * sizeof(wchar_t);
    if (size > maxBytes) return ERROR_FILENAME_EXCED_RANGE;
    std::vector<std::byte> data(size);
    const Header header{IO_REPARSE_TAG_MOUNT_POINT, static_cast<WORD>(size - 8), 0, 0,
                        static_cast<WORD>(substitution.size() * sizeof(wchar_t)),
                        static_cast<WORD>((substitution.size() + 1) * sizeof(wchar_t)),
                        static_cast<WORD>(source.size() * sizeof(wchar_t))};
    std::memcpy(data.data(), &header, sizeof(header));
    std::memcpy(data.data() + sizeof(header), substitution.c_str(), (substitution.size() + 1) * sizeof(wchar_t));
    std::memcpy(data.data() + sizeof(header) + header.printOffset, source.data(), source.size() * sizeof(wchar_t));
    if (!CreateDirectoryW(destination.c_str(), nullptr)) return GetLastError();
    DWORD error = ERROR_SUCCESS;
    {
        const Handle handle{CreateFileW(destination.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
        DWORD returned = 0;
        if (handle.value == INVALID_HANDLE_VALUE ||
            !DeviceIoControl(handle.value, FSCTL_SET_REPARSE_POINT, data.data(), static_cast<DWORD>(data.size()),
                             nullptr, 0, &returned, nullptr)) error = GetLastError();
    }
    if (error) RemoveDirectoryW(destination.c_str());
    return error;
}

std::wstring quoteArgument(Name input) {
    std::wstring result(1, L'"');
    std::size_t slashes = 0;
    for (const wchar_t ch : input) {
        if (ch == L'\\') {
            ++slashes;
            continue;
        }
        result.append(ch == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        result += ch;
        slashes = 0;
    }
    result.append(slashes * 2, L'\\');
    result += L'"';
    return result;
}

} // namespace

Listing findSmallFiles(std::wstring path, std::uint64_t limit, Sort sort, Progress& progress) {
    progress.phase = Phase::Search;
    auto dir = std::make_shared<Directory>();
    dir->path = std::move(path);
    dir->recursive = true;
    const auto root = nativePath(dir->path);
    const auto prefixLength = childPath(root, {}).size();
    std::size_t visited = 0;
    if (limit) {
        const auto error = walkTree(root, progress, [&](const std::wstring& item, Node node, Visit phase) -> DWORD {
            if (phase != Visit::Entry) return ERROR_SUCCESS;
            if (item == root && (!node.folder() || node.link())) return ERROR_DIRECTORY;
            if (node.folder() && node.link()) ++dir->skippedFolders;
            if (!node.folder() && !node.link() && node.size < limit) {
                dir->add(Name(item).substr(prefixLength), node.size, false);
            }
            if ((++visited & 1023) == 0) progress.done = visited;
            return ERROR_SUCCESS;
        }, &dir->skippedFolders);
        if (error) throw error;
    }
    return {dir, sortDirectory(*dir, sort, progress)};
}

OperationResult deleteFiles(const Directory& dir, const std::vector<Index>& selected, Progress& progress,
                            Language language, bool wipe) {
    std::vector<std::byte> zeros;
    if (wipe) zeros.resize(1024 * 1024);
    return runItems(dir, selected, progress, language, wipe ? Phase::Wipe : Phase::Delete,
                    [&](Index index, const std::wstring& path) -> DWORD {
        if (!dir.entries[index].folder() && !wipe) return removeFile(path);
        if (dir.entries[index].folder()) {
            const auto attributes = GetFileAttributesW(path.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES) return GetLastError();
            if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)) return ERROR_DIRECTORY;
        } else {
            const auto attributes = GetFileAttributesW(path.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES) return GetLastError();
            if (attributes & FILE_ATTRIBUTE_DIRECTORY) return ERROR_DIRECTORY;
        }
        return removeTree(path, wipe, progress, zeros);
    });
}

OperationResult transferItems(const Directory& dir, std::span<const Index> selected, Name destination,
                              bool move, Progress& progress, Language language, bool copyPermissions) {
    const auto destinationPath = nativePath(destination);
    const auto attributes = GetFileAttributesW(destinationPath.c_str());
    DWORD destinationError = ERROR_SUCCESS;
    if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
        destinationError = attributes == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_DIRECTORY;
    } else if (copyPermissions && !move) {
        const Handle folder{CreateFileW(destinationPath.c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS, nullptr)};
        DWORD flags = 0;
        if (folder.value == INVALID_HANDLE_VALUE ||
            !GetVolumeInformationByHandleW(folder.value, nullptr, 0, nullptr, nullptr, &flags, nullptr, 0)) {
            destinationError = GetLastError();
        } else if (!(flags & FILE_PERSISTENT_ACLS)) {
            destinationError = ERROR_NOT_SUPPORTED;
        }
    }
    if (destinationError) {
        OperationResult result;
        result.failed = selected.size();
        result.errors.push_back(std::wstring(destination) + L" — " + windowsError(destinationError, language));
        return result;
    }
    std::wstring resolvedDestination;
    return runItems(dir, selected, progress, language, move ? Phase::Move : Phase::Copy,
        [&](Index index, const std::wstring& source) -> DWORD {
            const auto target = childPath(destinationPath, dir.leaf(index));
            const auto current = GetFileAttributesW(source.c_str());
            if (current == INVALID_FILE_ATTRIBUTES) return GetLastError();
            const bool folder = (current & FILE_ATTRIBUTE_DIRECTORY) != 0;
            if (folder != dir.entries[index].folder()) return ERROR_DIRECTORY;
            if (folder && !(current & FILE_ATTRIBUTE_REPARSE_POINT)) {
                if (resolvedDestination.empty()) resolvedDestination = finalPath(destinationPath);
                const auto resolvedSource = finalPath(source);
                if (resolvedSource.empty() || resolvedDestination.empty()) return ERROR_ACCESS_DENIED;
                const auto sourceWithSlash = childPath(resolvedSource, {});
                if (compareNames(resolvedDestination, resolvedSource) == 0 ||
                    (resolvedDestination.size() >= sourceWithSlash.size() &&
                     compareNames(Name(resolvedDestination).substr(0, sourceWithSlash.size()), sourceWithSlash) == 0)) {
                    return ERROR_INVALID_PARAMETER;
                }
            }
            if (move) {
                if (MoveFileExW(source.c_str(), target.c_str(), 0)) return ERROR_SUCCESS;
                const auto error = GetLastError();
                if (error != ERROR_NOT_SAME_DEVICE) return error;
            }
            return transferTree(source, target, move, progress, copyPermissions && !move);
        });
}

OperationResult pasteItems(std::span<const std::wstring> paths, Name destination, bool move,
                           Progress& progress, Language language, bool copyPermissions) {
    OperationResult result;
    std::unordered_map<std::wstring, Directory> folders;
    for (const auto& input : paths) {
        if (progress.cancel) break;
        const auto path = absolutePath(input);
        const auto parent = parentPath(path);
        const auto separator = path.find_last_of(L'\\');
        const Name name = separator == Name::npos ? Name{} : Name(path).substr(separator + 1);
        const auto attributes = parent.empty() || !validFileName(name) ? INVALID_FILE_ATTRIBUTES :
            GetFileAttributesW(nativePath(path).c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            ++result.failed;
            if (result.errors.size() < 6) result.errors.push_back(input + L" — " + windowsError(ERROR_FILE_NOT_FOUND, language));
            continue;
        }
        auto& group = folders[parent];
        group.path = parent;
        group.add(name, 0, (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
    }
    for (auto& entry : folders) {
        if (progress.cancel) break;
        const auto& group = entry.second;
        std::vector<Index> selected(group.entries.size());
        for (Index i = 0; i < selected.size(); ++i) selected[i] = i;
        auto batch = transferItems(group, selected, destination, move, progress, language, copyPermissions);
        result.changed += batch.changed;
        result.failed += batch.failed;
        for (auto& error : batch.errors) {
            if (result.errors.size() == 6) break;
            result.errors.push_back(std::move(error));
        }
        if (batch.cancelled) { result.cancelled = true; break; }
    }
    result.cancelled = result.cancelled || progress.cancel;
    return result;
}

OperationResult createLinks(const Directory& dir, std::span<const Index> selected, Name destination,
                            LinkKind kind, Progress& progress, Language language) {
    const auto dest = nativePath(destination);
    return runItems(dir, selected, progress, language, Phase::Change, [&](Index index, const std::wstring& source) -> DWORD {
        const auto name = dir.name(index);
        const auto target = childPath(dest, dir.leaf(index));
        const bool folder = dir.entries[index].folder();
        if (kind == LinkKind::Junction) {
            if (!folder || dir.path.starts_with(L"\\\\") ||
                GetDriveTypeW(dir.path.substr(0, 3).c_str()) == DRIVE_REMOTE) return ERROR_NOT_SUPPORTED;
            return createJunction(target, childPath(dir.path, name));
        }
        if (kind == LinkKind::Hard) {
            if (folder) return ERROR_NOT_SUPPORTED;
            return CreateHardLinkW(target.c_str(), source.c_str(), nullptr) ? ERROR_SUCCESS : GetLastError();
        }
        const auto full = childPath(dir.path, name);
        const DWORD flags = folder ? SYMBOLIC_LINK_FLAG_DIRECTORY : 0;
        if (CreateSymbolicLinkW(target.c_str(), full.c_str(), flags | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)) {
            return ERROR_SUCCESS;
        }
        const auto error = GetLastError();
        if (error != ERROR_INVALID_PARAMETER) return error;
        return CreateSymbolicLinkW(target.c_str(), full.c_str(), flags) ? ERROR_SUCCESS : GetLastError();
    });
}

OperationResult changeAttributes(const Directory& dir, std::span<const Index> selected, DWORD set, DWORD clear,
                                 Progress& progress, Language language) {
    constexpr DWORD supported = FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM |
        FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_TEMPORARY | FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED;
    return runItems(dir, selected, progress, language, Phase::Change, [&](Index, const std::wstring& path) -> DWORD {
        const auto current = GetFileAttributesW(path.c_str());
        if (current == INVALID_FILE_ATTRIBUTES) return GetLastError();
        const auto next = ((current | set) & ~clear) & supported;
        if (next == (current & supported)) return ERROR_SUCCESS;
        return SetFileAttributesW(path.c_str(), next ? next : FILE_ATTRIBUTE_NORMAL) ? ERROR_SUCCESS : GetLastError();
    });
}

OperationResult changeTime(const Directory& dir, std::span<const Index> selected, FILETIME time, TimeKind kind,
                           Progress& progress, Language language) {
    return runItems(dir, selected, progress, language, Phase::Change, [&](Index, const std::wstring& path) -> DWORD {
        const Handle handle{CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
        if (handle.value == INVALID_HANDLE_VALUE) return GetLastError();
        if (kind == TimeKind::Changed) {
            FILE_BASIC_INFO changed{};
            changed.ChangeTime.LowPart = time.dwLowDateTime;
            changed.ChangeTime.HighPart = static_cast<LONG>(time.dwHighDateTime);
            return SetFileInformationByHandle(handle.value, FileBasicInfo, &changed, sizeof(changed)) ?
                ERROR_SUCCESS : GetLastError();
        }
        return SetFileTime(handle.value, kind == TimeKind::Created ? &time : nullptr,
            kind == TimeKind::Accessed ? &time : nullptr, kind == TimeKind::Modified ? &time : nullptr) ?
            ERROR_SUCCESS : GetLastError();
    });
}

OperationResult applyCommand(const Directory& dir, std::span<const Index> selected, Name command,
                             Progress& progress, Language language) {
    return runItems(dir, selected, progress, language, Phase::Run, [&](Index index, const std::wstring&) -> DWORD {
        auto line = std::wstring(command);
        const auto argument = quoteArgument(childPath(dir.path, dir.name(index)));
        bool hasToken = false;
        for (std::size_t pos = 0; (pos = line.find(L"{}", pos)) != std::wstring::npos; pos += argument.size()) {
            line.replace(pos, 2, argument);
            hasToken = true;
        }
        if (!hasToken) line += L" " + argument;
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION info{};
        if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, CREATE_NEW_CONSOLE,
                            nullptr, dir.path.c_str(), &startup, &info)) return GetLastError();
        const Handle process{info.hProcess};
        CloseHandle(info.hThread);
        while (!progress.cancel) {
            const auto status = WaitForSingleObject(process.value, 100);
            if (status == WAIT_TIMEOUT) continue;
            if (status != WAIT_OBJECT_0) return GetLastError();
            DWORD code = 0;
            if (!GetExitCodeProcess(process.value, &code)) return GetLastError();
            return code ? ERROR_GEN_FAILURE : ERROR_SUCCESS;
        }
        return ERROR_CANCELLED;
    });
}

SelectionInfo inspectItems(const Directory& dir, std::span<const Index> selected, Progress& progress) {
    SelectionInfo result;
    progress.total = selected.size();
    progress.done = 0;
    for (Index index : selected) {
        if (progress.cancel) break;
        const auto path = nativePath(childPath(dir.path, dir.name(index)));
        const auto error = walkTree(path, progress, [&](const std::wstring&, Node node, Visit phase) -> DWORD {
            if (phase == Visit::EndFolder) return ERROR_SUCCESS;
            if (node.link()) ++result.links;
            if (node.folder()) ++result.folders;
            else {
                ++result.files;
                result.bytes += node.size;
            }
            return ERROR_SUCCESS;
        });
        if (error) throw error;
        ++progress.done;
    }
    return result;
}

bool matchesMask(Name pattern, Name name) {
    if (pattern == L"*.*") return true;
    std::size_t p = 0, n = 0, star = Name::npos, retry = 0;
    while (n < name.size()) {
        if (p < pattern.size() && pattern[p] == L'*') {
            star = p++;
            retry = n;
        } else if (p < pattern.size() && (pattern[p] == L'?' || compareNames(pattern.substr(p, 1), name.substr(n, 1)) == 0)) {
            ++p;
            ++n;
        } else if (star != Name::npos) {
            p = star + 1;
            n = ++retry;
        } else return false;
    }
    while (p < pattern.size() && pattern[p] == L'*') ++p;
    return p == pattern.size();
}

std::optional<FILETIME> parseLocalTime(Name value) {
    if (value.size() != 19 || value[4] != L'-' || value[7] != L'-' || value[10] != L' ' ||
        value[13] != L':' || value[16] != L':') return std::nullopt;
    SYSTEMTIME local{};
    const std::array<WORD*, 6> fields{&local.wYear, &local.wMonth, &local.wDay, &local.wHour, &local.wMinute, &local.wSecond};
    constexpr std::array<std::size_t, 6> offsets{0, 5, 8, 11, 14, 17};
    for (std::size_t i = 0; i < fields.size(); ++i) {
        const auto part = parseNumber(value.substr(offsets[i], i ? 2 : 4));
        if (!part) return std::nullopt;
        *fields[i] = static_cast<WORD>(*part);
    }
    SYSTEMTIME utc{};
    FILETIME time{};
    if (!TzSpecificLocalTimeToSystemTime(nullptr, &local, &utc) || !SystemTimeToFileTime(&utc, &time)) return std::nullopt;
    return time;
}

namespace {

struct ZipStream {
    HANDLE file;
    Progress* progress;
    DWORD error = ERROR_SUCCESS;
};

bool seekZip(ZipStream& stream, mz_uint64 offset) {
    if (stream.progress && stream.progress->cancel) {
        stream.error = ERROR_CANCELLED;
        return false;
    }
    if (offset > static_cast<mz_uint64>(std::numeric_limits<LONGLONG>::max())) {
        stream.error = ERROR_FILE_TOO_LARGE;
        return false;
    }
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(offset);
    if (SetFilePointerEx(stream.file, position, nullptr, FILE_BEGIN)) return true;
    stream.error = GetLastError();
    return false;
}

size_t zipRead(void* context, mz_uint64 offset, void* buffer, size_t count) {
    auto& stream = *static_cast<ZipStream*>(context);
    if (!seekZip(stream, offset)) return 0;
    DWORD bytes = 0;
    if (ReadFile(stream.file, buffer, static_cast<DWORD>(std::min<std::size_t>(count, MAXDWORD)), &bytes, nullptr)) {
        return bytes;
    }
    stream.error = GetLastError();
    return 0;
}

size_t zipWrite(void* context, mz_uint64 offset, const void* buffer, size_t count) {
    auto& stream = *static_cast<ZipStream*>(context);
    if (!seekZip(stream, offset)) return 0;
    DWORD bytes = 0;
    if (WriteFile(stream.file, buffer, static_cast<DWORD>(std::min<std::size_t>(count, MAXDWORD)), &bytes, nullptr)) {
        if (bytes < count) stream.error = ERROR_WRITE_FAULT;
        return bytes;
    }
    stream.error = GetLastError();
    return 0;
}

size_t testZipBytes(void* context, mz_uint64, const void*, size_t count) {
    return static_cast<Progress*>(context)->cancel ? 0 : count;
}

struct Zip {
    mz_zip_archive value{};

    ~Zip() { mz_zip_end(&value); }
};

std::wstring archiveMember(mz_zip_archive& zip, mz_uint index, const mz_zip_archive_file_stat& stat) {
    const auto length = mz_zip_reader_get_filename(&zip, index, nullptr, 0);
    if (length < 2) throw static_cast<DWORD>(ERROR_INVALID_NAME);
    std::string bytes(length, '\0');
    if (mz_zip_reader_get_filename(&zip, index, bytes.data(), length) != length) {
        throw static_cast<DWORD>(ERROR_INVALID_DATA);
    }
    bytes.pop_back();
    try {
        return decodeBytes(bytes);
    } catch (DWORD error) {
        if (error != ERROR_NO_UNICODE_TRANSLATION || (stat.m_bit_flag & 0x0800)) throw;
        return decodeBytes(bytes, 437);
    }
}

bool safeMember(Name name, const mz_zip_archive_file_stat& stat) {
    if (((stat.m_external_attr >> 16) & 0170000u) == 0120000u ||
        (stat.m_external_attr & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
    if (stat.m_is_directory && (name.ends_with(L"/") || name.ends_with(L"\\"))) name.remove_suffix(1);
    if (name.empty()) return false;
    while (!name.empty()) {
        const auto separator = name.find_first_of(L"/\\");
        if (!validFileName(name.substr(0, separator))) return false;
        if (separator == Name::npos) return true;
        name.remove_prefix(separator + 1);
    }
    return false;
}

DWORD extractMember(mz_zip_archive& zip, mz_uint index, Name root, Name name,
                    const mz_zip_archive_file_stat& stat, Progress& progress) {
    auto path = std::wstring(root);
    if (stat.m_is_directory && (name.ends_with(L"/") || name.ends_with(L"\\"))) name.remove_suffix(1);
    for (;;) {
        const auto separator = name.find_first_of(L"/\\");
        path = childPath(path, name.substr(0, separator));
        if (separator == Name::npos && !stat.m_is_directory) break;
        if (!CreateDirectoryW(path.c_str(), nullptr)) {
            const auto error = GetLastError();
            if (error != ERROR_ALREADY_EXISTS) return error;
            const auto attrs = GetFileAttributesW(path.c_str());
            if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY) ||
                (attrs & FILE_ATTRIBUTE_REPARSE_POINT)) return ERROR_DIRECTORY;
        }
        if (separator == Name::npos) return ERROR_SUCCESS;
        name.remove_prefix(separator + 1);
    }
    DWORD error = ERROR_SUCCESS;
    {
        const Handle output{CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                         FILE_ATTRIBUTE_NORMAL, nullptr)};
        if (output.value == INVALID_HANDLE_VALUE) return GetLastError();
        ZipStream write{output.value, &progress};
        if (!mz_zip_reader_extract_to_callback(&zip, index, zipWrite, &write, 0)) {
            error = write.error ? write.error : progress.cancel ? ERROR_CANCELLED : ERROR_CRC;
        }
    }
    if (error) DeleteFileW(path.c_str());
    return error;
}

DWORD openZip(Zip& zip, ZipStream& stream) {
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(stream.file, &size)) return GetLastError();
    zip.value.m_pRead = zipRead;
    zip.value.m_pIO_opaque = &stream;
    if (mz_zip_reader_init(&zip.value, static_cast<mz_uint64>(size.QuadPart), 0)) return ERROR_SUCCESS;
    return stream.error ? stream.error : ERROR_BAD_FORMAT;
}

} // namespace

OperationResult createArchive(const Directory& dir, std::span<const Index> selected, Name destination,
                              Progress& progress, Language language) {
    OperationResult result;
    progress.phase = Phase::Archive;
    progress.total = selected.size();
    const auto parent = finalPath(nativePath(parentPath(destination)));
    const auto target = nativePath(destination);
    if (parent.empty()) throw static_cast<DWORD>(ERROR_PATH_NOT_FOUND);
    if (GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES) throw static_cast<DWORD>(ERROR_FILE_EXISTS);
    for (Index index : selected) {
        if (!dir.entries[index].folder()) continue;
        const auto source = finalPath(nativePath(childPath(dir.path, dir.name(index))));
        const auto prefix = childPath(source, {});
        if (source.empty() || compareNames(parent, source) == 0 ||
            (parent.size() >= prefix.size() && compareNames(Name(parent).substr(0, prefix.size()), prefix) == 0)) {
            throw static_cast<DWORD>(ERROR_INVALID_PARAMETER);
        }
    }

    struct Stage {
        std::wstring path;
        ~Stage() { if (!path.empty()) DeleteFileW(path.c_str()); }
    } stage{nativePath(temporarySibling(destination))};

    DWORD error = ERROR_SUCCESS;
    {
        const Handle output{CreateFileW(stage.path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                         CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr)};
        if (output.value == INVALID_HANDLE_VALUE) {
            stage.path.clear();
            throw GetLastError();
        }
        ZipStream write{output.value, &progress};
        Zip zip;
        zip.value.m_pWrite = zipWrite;
        zip.value.m_pIO_opaque = &write;
        if (!mz_zip_writer_init_v2(&zip.value, 0, MZ_ZIP_FLAG_WRITE_ZIP64)) {
            throw static_cast<DWORD>(ERROR_NOT_ENOUGH_MEMORY);
        }

        for (Index index : selected) {
            const auto source = nativePath(childPath(dir.path, dir.name(index)));
            error = walkTree(source, progress, [&](const std::wstring& path, Node node, Visit phase) -> DWORD {
                if (phase == Visit::EndFolder) return ERROR_SUCCESS;
                if (node.link()) return ERROR_NOT_SUPPORTED;
                auto member = std::wstring(dir.name(index));
                member.append(path, source.size());
                std::replace(member.begin(), member.end(), L'\\', L'/');
                if (node.folder()) member += L'/';
                const auto name = encodeText(member);
                if (node.folder()) {
                    if (mz_zip_writer_add_mem(&zip.value, name.c_str(), nullptr, 0, 0)) return ERROR_SUCCESS;
                    return write.error ? write.error : ERROR_WRITE_FAULT;
                }
                const Handle input{CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                    OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
                if (input.value == INVALID_HANDLE_VALUE) return GetLastError();
                FILE_ATTRIBUTE_TAG_INFO info{};
                if (!GetFileInformationByHandleEx(input.value, FileAttributeTagInfo, &info, sizeof(info))) return GetLastError();
                if (info.FileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) return ERROR_NOT_SUPPORTED;
                LARGE_INTEGER length{};
                if (!GetFileSizeEx(input.value, &length)) return GetLastError();
                ZipStream read{input.value, &progress};
                if (mz_zip_writer_add_read_buf_callback(&zip.value, name.c_str(), zipRead, &read,
                    static_cast<mz_uint64>(length.QuadPart), nullptr, nullptr, 0,
                    MZ_BEST_SPEED, nullptr, 0, nullptr, 0)) return ERROR_SUCCESS;
                return read.error ? read.error : write.error ? write.error : ERROR_WRITE_FAULT;
            });
            if (error) {
                if (error != ERROR_CANCELLED) result.errors.push_back(std::wstring(dir.name(index)) + L" — " + windowsError(error, language));
                break;
            }
            ++progress.done;
        }
        if (!error && progress.cancel) error = ERROR_CANCELLED;
        if (!error && !mz_zip_writer_finalize_archive(&zip.value)) error = write.error ? write.error : ERROR_WRITE_FAULT;
        if (!error && !FlushFileBuffers(output.value)) error = GetLastError();
    }
    if (!error && !MoveFileExW(stage.path.c_str(), target.c_str(), 0)) error = GetLastError();
    if (error == ERROR_CANCELLED) result.cancelled = true;
    else if (error) {
        result.failed = 1;
        if (result.errors.empty()) result.errors.push_back(std::wstring(destination) + L" — " + windowsError(error, language));
    } else {
        stage.path.clear();
        result.changed = selected.size();
    }
    return result;
}

OperationResult processArchives(const Directory& dir, std::span<const Index> selected, Name destination, bool extract,
                                Progress& progress, Language language) {
    return runItems(dir, selected, progress, language, Phase::Archive, [&](Index index, const std::wstring& path) -> DWORD {
        if (dir.entries[index].folder()) return ERROR_DIRECTORY;
        const Handle input{CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                        OPEN_EXISTING, FILE_FLAG_RANDOM_ACCESS, nullptr)};
        if (input.value == INVALID_HANDLE_VALUE) return GetLastError();
        ZipStream read{input.value, &progress};
        Zip zip;
        if (const auto error = openZip(zip, read)) return error;
        const auto count = mz_zip_reader_get_num_files(&zip.value);
        std::vector<std::wstring> members;
        if (extract) members.reserve(count);
        for (mz_uint i = 0; i < count; ++i) {
            if (progress.cancel) return ERROR_CANCELLED;
            mz_zip_archive_file_stat stat{};
            if (!mz_zip_reader_file_stat(&zip.value, i, &stat) || !stat.m_is_supported) return ERROR_BAD_FORMAT;
            if (extract) {
                try {
                    auto name = archiveMember(zip.value, i, stat);
                    if (!safeMember(name, stat)) return ERROR_INVALID_NAME;
                    members.push_back(std::move(name));
                } catch (DWORD error) { return error; }
            } else if (!stat.m_is_directory &&
                       !mz_zip_reader_extract_to_callback(&zip.value, i, testZipBytes, &progress, 0)) {
                return progress.cancel ? ERROR_CANCELLED : ERROR_CRC;
            }
        }
        if (!extract) return ERROR_SUCCESS;
        auto base = std::wstring(dir.stem(index)) + L".extracted";
        if (!validFileName(base)) return ERROR_INVALID_NAME;
        const auto root = nativePath(childPath(destination, base));
        if (!CreateDirectoryW(root.c_str(), nullptr)) return GetLastError();
        for (mz_uint i = 0; i < count; ++i) {
            if (progress.cancel) return ERROR_CANCELLED;
            mz_zip_archive_file_stat stat{};
            if (!mz_zip_reader_file_stat(&zip.value, i, &stat)) return ERROR_BAD_FORMAT;
            if (const auto error = extractMember(zip.value, i, root, members[i], stat, progress)) return error;
        }
        return ERROR_SUCCESS;
    });
}

std::wstring listArchive(Name path, Language language) {
    const auto full = nativePath(path);
    const Handle input{CreateFileW(full.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                    OPEN_EXISTING, FILE_FLAG_RANDOM_ACCESS, nullptr)};
    if (input.value == INVALID_HANDLE_VALUE) throw GetLastError();
    ZipStream read{input.value, nullptr};
    Zip zip;
    if (const auto error = openZip(zip, read)) throw error;
    const auto count = mz_zip_reader_get_num_files(&zip.value);
    auto result = translated(language, L"Entries ", L"Записей ") + number(count) + L"\r\n\r\n";
    constexpr mz_uint limit = 2000;
    for (mz_uint i = 0; i < std::min(count, limit); ++i) {
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&zip.value, i, &stat)) throw static_cast<DWORD>(ERROR_BAD_FORMAT);
        result += archiveMember(zip.value, i, stat);
        if (!stat.m_is_directory) result += L"   " + number(stat.m_uncomp_size);
        result += L"\r\n";
    }
    if (count > limit) result += translated(language, L"Showing the first 2,000 entries.", L"Показаны первые 2 000 записей.");
    return result;
}

} // namespace nf
