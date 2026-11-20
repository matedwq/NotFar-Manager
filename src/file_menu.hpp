#pragma once

#include "files.hpp"

#include <shlobj.h>
#include <span>

namespace nf {

enum FileMenuCommand : UINT {
    RenameFile = 1, BatchRenameFiles, DeleteFiles, CutFiles, CopyFiles, FileProperties,
    FirstShellCommand = 0x100
};

class FileMenu {
public:
    FileMenu(HWND owner, const Directory& directory, std::optional<Index> single, Language language);
    ~FileMenu();
    FileMenu(const FileMenu&) = delete;
    FileMenu& operator=(const FileMenu&) = delete;

    UINT track(HWND owner, POINT point) const;
    std::optional<LRESULT> handleMessage(UINT message, WPARAM wParam, LPARAM lParam) const;
    HRESULT invoke(HWND owner, UINT command, POINT point, bool& refresh) const;

private:
    HMENU popup = nullptr;
    IContextMenu* shellMenu = nullptr;
    IContextMenu2* shellMenu2 = nullptr;
    IContextMenu3* shellMenu3 = nullptr;
};

DWORD putFilesOnClipboard(HWND owner, const Directory& directory, std::span<const Index> files, bool cut);
bool showFileProperties(HWND owner, const Directory& directory, std::span<const Index> files);

}
