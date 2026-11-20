#pragma once

#include "files.hpp"

#include <span>

namespace nf {

enum FileMenuCommand : UINT {
    OpenFolder = 1, ViewItems, EditFile, CopyItems, MoveItems, RenameFile, BatchRenameFiles,
    HardLinkItems, SymbolicLinkItems, JunctionItems, MakeFolder, DeleteFiles, WipeFiles,
    PackZip, ExtractZip, ListZip, TestZip, FileAttributes, ModifiedTime, CreatedTime, AccessedTime, ChangedTime, ApplyCommand,
    DescribeItems, SelectMask, DeselectMask, InvertSelection, RestoreSelection,
    CutFiles, CopyFiles, PasteFiles, FileProperties
};

UINT showFileMenu(HWND owner, POINT point, bool selected, std::optional<bool> singleFolder,
                  bool canRestore, Language language);

struct ClipboardFiles {
    std::vector<std::wstring> paths;
    bool cut = false;
    DWORD sequence = 0;
};

DWORD putFilesOnClipboard(HWND owner, const Directory& directory, std::span<const Index> files, bool cut);
ClipboardFiles filesFromClipboard(HWND owner);
void clearFileClipboard(HWND owner, DWORD sequence);

}
