#pragma once

#include "files.hpp"

namespace nf {

struct TextFile {
    std::wstring text;
    std::wstring path;
    FILETIME modified{};
    std::uint64_t size = 0;
    UINT codePage = CP_UTF8;
    bool bom = false;
    bool unixLines = false;
    bool binary = false;
    bool truncated = false;
    bool exists = false;
};

std::wstring decodeBytes(std::string_view bytes, UINT page = CP_UTF8);
std::string encodeText(Name text, UINT page = CP_UTF8);
TextFile loadTextFile(Name path, bool allowMissing = false, UINT legacyPage = CP_ACP);
DWORD saveTextFile(const TextFile& file, Name text);
std::wstring temporarySibling(Name path);

}
