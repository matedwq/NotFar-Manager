#include "text_files.hpp"
#include "operations.hpp"

#include <objbase.h>

#include <algorithm>
#include <cwchar>
#include <map>
#include <unordered_set>

namespace nf {
namespace {

struct Handle {
    HANDLE value;
    ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};

constexpr std::size_t textLimit = 8 * 1024 * 1024;
constexpr UINT utf16 = 1200;

std::uint64_t lengthOf(const BY_HANDLE_FILE_INFORMATION& info) {
    return (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
}

std::wstring hexPreview(std::string_view bytes) {
    constexpr wchar_t hex[] = L"0123456789ABCDEF";
    const auto count = std::min<std::size_t>(bytes.size(), 16 * 1024);
    std::wstring result;
    result.reserve(count * 5);
    for (std::size_t offset = 0; offset < count; offset += 16) {
        wchar_t address[20]{};
        swprintf(address, std::size(address), L"%08X  ", static_cast<unsigned>(offset));
        result += address;
        const auto line = bytes.substr(offset, std::min<std::size_t>(16, count - offset));
        for (const unsigned char ch : line) {
            result += hex[ch >> 4];
            result += hex[ch & 15];
            result += L' ';
        }
        result.append((16 - line.size()) * 3 + 1, L' ');
        for (const unsigned char ch : line) result += ch >= 32 && ch < 127 ? static_cast<wchar_t>(ch) : L'.';
        result += L"\r\n";
    }
    return result;
}

} // namespace

std::wstring decodeBytes(std::string_view bytes, UINT page) {
    if (bytes.empty()) return {};
    const auto flags = page == CP_UTF8 ? MB_ERR_INVALID_CHARS : 0;
    const auto count = MultiByteToWideChar(page, flags, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    if (!count) throw GetLastError();
    std::wstring text(count, L'\0');
    if (!MultiByteToWideChar(page, flags, bytes.data(), static_cast<int>(bytes.size()), text.data(), count)) {
        throw GetLastError();
    }
    return text;
}

std::string encodeText(Name text, UINT page) {
    if (text.empty()) return {};
    const auto flags = page == CP_UTF8 ? WC_ERR_INVALID_CHARS :
        (page == CP_SYMBOL ? 0 : WC_NO_BEST_FIT_CHARS);
    BOOL replaced = FALSE;
    BOOL* replacedPtr = page == CP_UTF8 ? nullptr : &replaced;
    const auto count = WideCharToMultiByte(page, flags, text.data(), static_cast<int>(text.size()),
                                           nullptr, 0, nullptr, replacedPtr);
    if (!count) throw GetLastError();
    if (replaced) throw static_cast<DWORD>(ERROR_NO_UNICODE_TRANSLATION);
    std::string bytes(count, '\0');
    if (!WideCharToMultiByte(page, flags, text.data(), static_cast<int>(text.size()),
                             bytes.data(), count, nullptr, replacedPtr)) throw GetLastError();
    if (replaced) throw static_cast<DWORD>(ERROR_NO_UNICODE_TRANSLATION);
    return bytes;
}

std::wstring temporarySibling(Name path) {
    GUID guid{};
    wchar_t token[40]{};
    if (FAILED(CoCreateGuid(&guid)) || !StringFromGUID2(guid, token, static_cast<int>(std::size(token)))) {
        throw static_cast<DWORD>(ERROR_GEN_FAILURE);
    }
    return childPath(parentPath(path), L".notfar-" + std::wstring(token) + L".tmp");
}

TextFile loadTextFile(Name path, bool allowMissing, UINT legacyPage) {
    TextFile file;
    file.path = path;
    const auto full = nativePath(path);
    const Handle handle{CreateFileW(full.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr)};
    if (handle.value == INVALID_HANDLE_VALUE) {
        const auto error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND && allowMissing) { file.bom = true; return file; }
        throw error;
    }
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(handle.value, &info)) throw GetLastError();
    file.exists = true;
    file.modified = info.ftLastWriteTime;
    file.size = lengthOf(info);
    file.truncated = file.size > textLimit;
    std::string bytes(static_cast<std::size_t>(std::min<std::uint64_t>(file.size, textLimit)), '\0');
    std::size_t total = 0;
    while (total < bytes.size()) {
        DWORD read = 0;
        if (!ReadFile(handle.value, bytes.data() + total, static_cast<DWORD>(bytes.size() - total), &read, nullptr)) {
            throw GetLastError();
        }
        if (!read) break;
        total += read;
    }
    bytes.resize(total);
    std::string_view input(bytes);
    if (input.starts_with("\xff\xfe")) {
        file.codePage = utf16;
        file.bom = true;
        for (std::size_t i = 2; i + 1 < input.size(); i += 2) {
            file.text += static_cast<wchar_t>(static_cast<unsigned char>(input[i]) |
                                             (static_cast<unsigned char>(input[i + 1]) << 8));
        }
        file.binary = (input.size() % 2) || file.text.find(L'\0') != Name::npos;
    } else {
        file.binary = std::any_of(input.begin(), input.end(), [](unsigned char ch) {
            return ch < 32 && ch != '\r' && ch != '\n' && ch != '\t' && ch != '\f';
        });
        if (!file.binary) {
            if (input.starts_with("\xef\xbb\xbf")) { file.bom = true; input.remove_prefix(3); }
            try {
                file.text = decodeBytes(input);
            } catch (DWORD error) {
                if (error != ERROR_NO_UNICODE_TRANSLATION) throw;
                if (file.bom) file.binary = true;
                else {
                    file.codePage = legacyPage == CP_ACP ? GetACP() : legacyPage == CP_OEMCP ? GetOEMCP() : legacyPage;
                    file.text = decodeBytes(input, file.codePage);
                }
            }
        }
    }

    if (file.binary) {
        file.text = hexPreview(bytes);
        file.truncated = file.truncated || bytes.size() > 16 * 1024;
    } else {
        file.unixLines = file.text.find(L'\n') != Name::npos && file.text.find(L"\r\n") == Name::npos;
        if (file.unixLines) {
            std::wstring windowsLines;
            windowsLines.reserve(file.text.size());
            for (wchar_t ch : file.text) {
                if (ch == L'\n') windowsLines += L'\r';
                windowsLines += ch;
            }
            file.text = std::move(windowsLines);
        }
    }
    return file;
}

DWORD saveTextFile(const TextFile& file, Name text) {
    if (file.binary || file.truncated) return ERROR_NOT_SUPPORTED;
    std::wstring lineEndings;
    if (file.unixLines) {
        lineEndings.reserve(text.size());
        for (std::size_t i = 0; i < text.size(); ++i) {
            if (text[i] != L'\r' || i + 1 == text.size() || text[i + 1] != L'\n') lineEndings += text[i];
        }
        text = lineEndings;
    }
    std::string bytes;
    if (file.codePage == utf16) {
        if (file.bom) bytes = "\xff\xfe";
        bytes.append(reinterpret_cast<const char*>(text.data()), text.size() * sizeof(wchar_t));
    } else {
        if (file.bom) bytes = "\xef\xbb\xbf";
        bytes += encodeText(text, file.codePage);
    }

    const auto path = nativePath(file.path);
    const auto stage = nativePath(temporarySibling(file.path));
    DWORD error = ERROR_SUCCESS;
    {
        const Handle out{CreateFileW(stage.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr)};
        if (out.value == INVALID_HANDLE_VALUE) return GetLastError();
        DWORD written = 0;
        if (!WriteFile(out.value, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) ||
            !FlushFileBuffers(out.value)) error = GetLastError();
        else if (written != bytes.size()) error = ERROR_WRITE_FAULT;
    }
    if (!error && file.exists) {
        WIN32_FILE_ATTRIBUTE_DATA info{};
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info)) error = GetLastError();
        else if (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) error = ERROR_NOT_SUPPORTED;
        else if (CompareFileTime(&file.modified, &info.ftLastWriteTime) != 0 ||
                 file.size != ((static_cast<std::uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow)) {
            error = ERROR_FILE_INVALID;
        } else if (!ReplaceFileW(path.c_str(), stage.c_str(), nullptr, 0, nullptr, nullptr)) error = GetLastError();
    } else if (!error && !MoveFileExW(stage.c_str(), path.c_str(), 0)) {
        error = GetLastError();
    }
    if (error) DeleteFileW(stage.c_str());
    return error;
}

OperationResult describeItems(const Directory& dir, std::span<const Index> selected, Name description,
                              Progress& progress, Language language) {
    OperationResult result;
    if (dir.recursive) {
        std::map<std::wstring, std::vector<Index>> folders;
        for (Index index : selected) {
            if (progress.cancel) { result.cancelled = true; return result; }
            const auto relative = dir.name(index);
            const auto prefix = relative.substr(0, relative.size() - dir.leaf(index).size());
            folders[std::wstring(prefix)].push_back(index);
        }
        SearchParents parents(dir);
        std::size_t processed = 0;
        for (const auto& [prefix, indices] : folders) {
            if (progress.cancel) break;
            Directory folder;
            folder.path = prefix.empty() ? dir.path : childPath(dir.path, Name(prefix).substr(0, prefix.size() - 1));
            try {
                if (const auto error = parents.lock(dir.name(indices.front()))) throw error;
                std::vector<Index> items;
                items.reserve(indices.size());
                for (Index index : indices) {
                    items.push_back(static_cast<Index>(folder.entries.size()));
                    folder.add(dir.leaf(index), 0, false);
                }
                const auto batch = describeItems(folder, items, description, progress, language);
                result.changed += batch.changed;
                result.failed += batch.failed;
                if (!batch.errors.empty() && result.errors.size() < 6) {
                    result.errors.push_back(prefix + batch.errors.front());
                }
            } catch (DWORD error) {
                result.failed += indices.size();
                if (result.errors.size() < 6) {
                    result.errors.push_back(prefix + L"descript.ion — " + windowsError(error, language));
                }
            }
            processed += indices.size();
            progress.total = selected.size();
            progress.done = processed;
        }
        result.cancelled = progress.cancel;
        return result;
    }
    progress.phase = Phase::Change;
    progress.total = selected.size();
    auto file = loadTextFile(childPath(dir.path, L"descript.ion"), true, CP_OEMCP);
    if (file.binary || file.truncated) throw static_cast<DWORD>(ERROR_NOT_SUPPORTED);
    std::unordered_set<Name> names;
    names.reserve(selected.size());
    for (Index index : selected) names.insert(dir.name(index));
    std::wstring text;
    for (Name remaining = file.text; !remaining.empty();) {
        const auto end = remaining.find(L'\n');
        Name line = remaining.substr(0, end);
        remaining = end == Name::npos ? Name{} : remaining.substr(end + 1);
        if (line.ends_with(L"\r")) line.remove_suffix(1);
        Name name;
        if (line.starts_with(L"\"")) {
            const auto quote = line.find(L'"', 1);
            if (quote != Name::npos) name = line.substr(1, quote - 1);
        } else {
            name = line.substr(0, line.find_first_of(L" \t"));
        }
        if (!names.contains(name)) { text += line; text += L"\r\n"; }
    }
    if (!description.empty()) {
        for (Index index : selected) {
            if (progress.cancel) { result.cancelled = true; return result; }
            text += L'"';
            text += dir.name(index);
            text += L"\" ";
            text += description;
            text += L"\r\n";
        }
    }
    if (progress.cancel) { result.cancelled = true; return result; }
    if (const auto error = saveTextFile(file, text)) {
        result.failed = selected.size();
        result.errors.push_back(L"descript.ion — " + windowsError(error, language));
    } else {
        result.changed = selected.size();
    }
    progress.done = selected.size();
    return result;
}

}
