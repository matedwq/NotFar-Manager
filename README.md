# NotFar Manager

NotFar Manager is a portable 64-bit file manager for Windows, based on the dual-pane workflow of [Far Manager](https://github.com/FarGroup/FarManager) and built for large directories.

- **[Download for Windows 11 ARM64](https://github.com/matedwq/NotFar-Manager/releases/latest/download/NotFar-Manager-ARM64.exe)** for ARM processors. This executable runs natively on ARM64.
- [Download for Windows 10 or 11 x64](https://github.com/matedwq/NotFar-Manager/releases/latest/download/NotFar-Manager-x64.exe) for Intel and AMD processors.

Run the downloaded file directly. See the [latest release notes](https://github.com/matedwq/NotFar-Manager/releases/latest) for changes.

The interface starts in English. Use the dropdown in the upper-right corner to switch between **English** and **Русский**. Changes take effect immediately, and the app remembers your choice.

## Working with files

1. Choose a drive or type a folder path into either pane and press **Enter**. Double-click a folder to open it. Press **Backspace** or click the up arrow to go up a level. Press **Tab** to switch panes.
2. Click **Name**, **Type**, or **Size** to sort. Click the same heading again to reverse the order. Click **Refresh** or press **F5** to reread the folder in the active pane.
3. Left-click a file or folder to select it and clear the previous selection. **Shift+click** selects the range from the first file to the clicked file, including both endpoints. **Ctrl+click** adds or removes individual files. **Insert** and **Space** also toggle individual files.
4. Click **Select all** or press **Ctrl+A** to select all files and folders in the active pane. Selected rows appear in red.
5. **Select files smaller than** replaces the selection with files strictly below the byte threshold, which starts at **2048**. Check **Include subfolders** to show and select matches throughout the folder tree. Results show relative paths. The search skips inaccessible folders and does not follow directory links. Press **F5** or click the up arrow to return to the folder view. **Clear selection** removes all marks. **Delete selected** permanently removes the marked files after confirmation.

Select at least one item, then click **Batch rename** or press **Ctrl+M** to preview the changes. Use **[C]** for the counter, **[N]** for the original name, and **[E]** for the original extension. Masks can also include literal text. The counter follows the order of the files in the pane. With a start of 1, a step of 1, and 8 digits, the first name could be `00000001.png`. Choose **Digits** from 1 to 15. This is the minimum width, including leading zeros. Use a negative step to count down.

Click **Run** to check the new names and apply the changes. The dialog closes when the operation finishes, and the pane refreshes after changes. Existing destination files are protected from overwrites. Stopping a batch keeps the changes already made, and the app reports how many operations succeeded or failed.

## File and folder context menu

Right-click a file or folder to open NotFar's built-in menu. Right-click any item in a selection to keep the whole selection. Right-click blank space to access pane-wide commands.

| Far file-menu area | NotFar commands |
| --- | --- |
| View and edit | Open folders, view text and binary previews, edit text, calculate folder sizes, show properties |
| Copy, rename, and move | Copy or move to a folder, rename, batch rename, cut/copy/paste through the file clipboard |
| Links and folders | Create hard links, symbolic links and folder junctions, make a folder |
| Delete and wipe | Delete selected files and folder trees, overwrite ordinary files with zeros before deleting |
| Archives | Create ZIP/ZIP64, list, test, and extract ZIP to a new `.extracted` folder |
| Metadata and commands | Change R/H/S/A/N/T/O attributes and file timestamps, edit `descript.ion`, apply a program to selected items |
| Selection | Select or deselect using `*` and `?`, invert or restore selection |

Copy and move suggest the folder in the opposite pane. In a destination dialog, click **Browse…** to open NotFar's folder picker. Choose a drive, double-click folders to navigate, then click **Select this folder**. You can also use it to choose where to save a new ZIP. The destination field still accepts a typed path. Existing destination files are protected from overwrites. Delete and Wipe operate permanently, including the contents of selected directories. Directory links are removed without descending into their targets. A wipe skips files with multiple hard links. Overwriting operates on logical file data and cannot guarantee erasure from SSDs, snapshots or backups.

Drag a file, folder, or marked group onto the opposite pane to open the copy dialog with that pane's folder filled in. Folder copies include their contents. Copying individual search results places them directly in the destination folder. In the copy dialog, including when pasting files from the clipboard, check **Copy NTFS permissions** to preserve the source access rules (DACL) on files and folders. This option starts unchecked for each copy and requires a destination that supports persistent access control lists and permission to write them. The copy uses its own owner and omits auditing rules.

For **Apply command**, enter an executable and its arguments. The placeholder `{}` inserts the quoted item path. If omitted, the path is appended. Use an explicit command interpreter if your command requires shell syntax.

NotFar uses Win32 directly for file operations. The menu does not load Explorer handlers or depend on Explorer's elevation broker. Windows access rights still apply. ZIP support is compiled into the EXE with [miniz](https://github.com/richgel999/miniz), licensed under the included [MIT notice](third_party/miniz/LICENSE). The menu has no Far plug-in or external archiver dependencies. Encrypted ZIPs, editing existing archives and other archive formats are outside the built-in ZIP commands. The editor works with text files up to 8 MiB.

You can pass one or two starting folders on the command line, for example `NotFar-Manager-x64.exe "D:\Data" "C:\Windows"`.

## Building and testing

Use Visual Studio Build Tools 2022 or later with **Desktop development with C++** and the MSVC toolset for the target architecture.

- `build.cmd arm64` writes `build\arm64\NotFar-Manager-ARM64.exe`.
- `build.cmd x64` writes `build\NotFar-Manager-x64.exe`.
- Add `test` after the architecture to run the filesystem checks on a matching Windows machine.
- `build\files-test.exe --scan "D:\Test data" 200000` measures reading and sorting an existing folder with the specified number of files.
- `build\files-test.exe --benchmark 200000` creates a temporary dataset, runs the same measurements, and removes the test files.

NotFar is written in C++20 and uses Win32. It reads batches of directory entries through `GetFileInformationByHandleEx`, with a fallback to `FindFirstFileExW`. Each directory entry supplies the name, size, and file or folder type. Reading, sorting, and file operations run on background threads. Virtual lists request text for visible rows. The batch enumeration approach is inspired by [Far Manager](https://github.com/FarGroup/FarManager).
