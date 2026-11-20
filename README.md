# NotFar Manager

NotFar Manager is a portable dual-pane file manager for Windows 10 and 11 x64, built for large directories. Download and run `NotFar-Manager.exe`. [Latest release](https://github.com/matedwq/NotFar-Manager/releases/latest)

The interface starts in English. Use the dropdown in the upper-right corner to switch between **English** and **Русский**. Changes take effect immediately, and the app remembers your choice.

## Working with files

1. Choose a drive or type a folder path into either pane and press **Enter**. Double-click a folder to open it. Press **Backspace** or click the up arrow to go up a level. Press **Tab** to switch panes.
2. Click **Name**, **Type**, or **Size** to sort. Click the same heading again to reverse the order. Click **Refresh** or press **F5** to reread the folder in the active pane.
3. Left-click a file to select it and clear the previous selection. **Shift+click** selects the range from the first file to the clicked file, including both endpoints. **Ctrl+click** adds or removes individual files. **Insert** and **Space** also toggle individual files.
4. Click **Select all** or press **Ctrl+A** to select all files in the active pane. Selected rows appear in red.
5. **Select files smaller than** replaces the selection with files strictly below the byte threshold, which starts at **2048**. **Clear selection** removes all marks. **Delete selected** permanently removes the marked files after confirmation.

Select at least one file, then click **Batch rename** or press **Ctrl+M** to preview the changes. Use **[C]** for the counter, **[N]** for the original name, and **[E]** for the original extension. Masks can also include literal text. The counter follows the order of the files in the pane. With a start of 1, a step of 1, and 8 digits, the first name could be `00000001.png`. Choose **Digits** from 1 to 15. This is the minimum width, including leading zeros. Use a negative step to count down.

Click **Run** to check the new names and apply the changes. The dialog closes when the operation finishes, and the pane refreshes after changes. Existing destination files are protected from overwrites. Stopping a batch keeps the changes already made, and the app reports how many operations succeeded or failed.

Confirmed permanent deletion uses a fast filesystem pass. Read-only files are handled when the volume supports it. If Windows denies access, NotFar retries the affected files through the Windows Shell, which can show a UAC prompt when elevation is available. Files that Windows still refuses to remove remain in place and are reported.

## File context menus

Right-click an unselected file to select it and show its Windows context menu. It includes Shell actions such as **Open**, **Copy**, and **Properties**, along with NotFar's **Rename** and **Batch rename** commands. The Shell supplies the wording and display language for its own commands.

Right-click an already-selected file to keep the selection. With multiple files selected, the menu offers **Cut**, **Copy**, **Delete**, **Batch rename**, and **Properties**. Cut and copy use the Windows clipboard, so you can paste the files in Explorer. Group deletion uses the same permanent-delete confirmation as the toolbar.

You can pass one or two starting folders on the command line, for example `NotFar-Manager.exe "D:\Data" "C:\Windows"`.

## Building and testing

Run `build.cmd` on Windows with Visual Studio Build Tools 2022 or later and the **Desktop development with C++** workload. The executable is written to `build\NotFar-Manager.exe`.

- `build.cmd test` builds the app and runs filesystem checks in temporary folders.
- `build\files-test.exe --scan "D:\Test data" 200000` measures reading and sorting an existing folder with the specified number of files.
- `build\files-test.exe --benchmark 200000` creates a temporary dataset, runs the same measurements, and removes the test files.

NotFar is written in C++20 and uses Win32. It reads batches of directory entries through `GetFileInformationByHandleEx`, with a fallback to `FindFirstFileExW`. Each directory entry supplies the name, size, and file or folder type. Reading, sorting, and file operations run on background threads. Virtual lists request text for visible rows. The batch enumeration approach is inspired by [Far Manager](https://github.com/FarGroup/FarManager).
