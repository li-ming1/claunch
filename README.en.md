[简体中文](README.md) | English

# claunch

A single-file, zero third-party-dependency, zero C-runtime Windows desktop grid launcher (x64 / Windows 10, 11).

One 4,889-line `claunch.c` plus a 115-line assembly stub compiles to roughly 70 KB,
with only 7 system DLLs in the import table. It does one thing — a desktop grid launcher —
and adds no runtime dependency beyond that.

## Features

- **Dense grid**: square slot model. Drag-and-drop swaps with the target cell, clearing a cell
  never shifts the others; cells can be inserted/removed, the grid re-flows to fill the window,
  and the edge is resizable.
- **Independent groups**: a default "Frequent" group, add/remove/rename as needed; drag rows to
  reorder, which also sets the mouse-wheel switching order; each group remembers its own scroll
  position.
- **Drop and go**: exe / .lnk / folders, plus shell virtual items such as Recycle Bin and
  This PC (OLE drag-and-drop). Virtual item icons are stateful (Recycle Bin full/empty, synced
  with the desktop on wake). Single click to launch with result feedback in the title bar;
  stale items are pruned automatically at startup.
- **Four wake paths**: tray icon, global hotkey (default Ctrl+Alt+K, changeable from the tray
  menu; if taken, the original key is kept), double-click on empty desktop, or holding the left
  mouse button and dragging the cursor into the panel.
- **Full keyboard support**: arrow keys to move the hover, Enter/number keys to launch, Del to
  clear a cell, Home/End/PageUp/PageDown to page, Ctrl+wheel zoom (40–160, remembered), and
  type-to-search across groups (prefix > word-start > substring > pinyin initials, `jsb` → Notepad),
  Ctrl+C/V to copy/paste the hovered cell.
- **Silent start**: goes straight to the tray; stale-item cleanup is deferred to the first wake;
  run-at-startup writes to HKCU Run.

## Memory and footprint

This is what the project actually optimizes for. All figures are measured:

| State | Modules | Total WS | Private commit |
|---|---|---|---|
| Tray (silent) | 23 | 0.37 MB | 1.75 MB |
| Panel visible | 31–33 | ~11 MB | 2.5 MB |

Image size: 71,680 bytes; `dumpbin /dependents` shows no CRT DLL.

> Note: `Total WS` includes shared image pages (the shared pages of system DLLs pulled in on
> wake); Task Manager's "Memory" column shows the **private working set**, which excludes them.
> When comparing memory, use the private-working-set figure.

The following design constraints make this possible (details in the header comments of
[claunch.c](claunch.c)):

- No `SHGetFileInfo` for icons (it drags in the whole shell icon pipeline); `.lnk` files are
  minimally parsed directly per MS-SHLLINK.
- Regular launches use `CreateProcessW`, not `ShellExecuteW` (the latter pulls in a WinRT stack
  that stays resident).
- The main window disconnects its IME association to keep input methods out of the process.
- OLE initialization and the icon pipeline are deferred to the first wake: silent tray residency
  pays no COM initialization cost.
- Working set is trimmed and the offscreen buffer freed when the panel hides.
- **No libcmt/ucrt**: wide-char primitives, the formatting core, config I/O, the entry point,
  the `/GS` cookie, and `mem*` are all implemented in-tree.

## Building

Requires Visual Studio 2022 (with the C++ toolset) and the Windows SDK. From an
*x64 Native Tools Command Prompt*, run:

```bat
ml64 /nologo /c /Fo claunch_chkstk.obj claunch.asm
cl /nologo /std:c11 /W4 /O1 /GS /GL /utf-8 /D CLAUNCH_NOCRT claunch.c ^
    /link /NODEFAULTLIB /ENTRY:wWinMainCRTStartup /SUBSYSTEM:WINDOWS ^
    /LTCG /OPT:REF /OPT:ICF /MANIFEST:EMBED /MANIFESTINPUT:claunch.manifest /OUT:claunch.exe ^
    claunch_chkstk.obj kernel32.lib user32.lib shell32.lib gdi32.lib imm32.lib advapi32.lib ole32.lib
```

CI ([.github/workflows/build.yml](.github/workflows/build.yml)) runs the same commands and
additionally asserts the import table contains no CRT dependency.

> The `ml64` object name must be `/Fo claunch_chkstk.obj` — with a **space**, not `/Fo:…`.
> The colon form silently produces no file yet returns 0.

## Releases

Official releases live on the [Releases](../../releases) page. Pushing a `vX.Y.Z` tag makes CI
build from the clean source and attach `claunch.exe` to the matching release. Assets:

| Asset | Contents |
|---|---|
| `claunch.exe` | x64 executable (~70 KB, no CRT dependency), built by tag-triggered CI from clean sources |
| Source code (zip / tar.gz) | Source archives attached automatically by GitHub |

Everyday builds (pushes / PRs without a tag) are available as the `claunch-x64` artifact of the
matching run on [Actions](../../actions). To verify the binary: `dumpbin /dependents claunch.exe`
should list only the 7 system DLLs.

## Usage

1. Double-click `claunch.exe`; it goes silently to the tray.
2. Press `Ctrl+Alt+K` (or double-click empty desktop) to wake the panel; it pops up at the cursor.
3. Drag exes / shortcuts / folders / Recycle Bin into cells.
4. Click to launch; `Del` clears a cell; right-click the tray icon for the menu
   (show panel / change hotkey / run at startup / exit).

## Configuration

`claunch.ini` (UTF-8) lives next to the exe. The program writes it atomically (writes
`claunch.tmp` first, then renames it over the original, so a power cut mid-write cannot tear
the config). Keys:

| Key | Meaning |
|---|---|
| `Rect=x y w h` | Window rectangle (physical pixels; restored on DPI change) |
| `Pinned=0/1` | Always on top (stays visible, no auto-hide) |
| `Hotkey=<mods> <vk>` | Wake hotkey; mod bits: 1=Alt 2=Ctrl 4=Shift, addable; vk is a virtual-key code |
| `Group=<n>` | Group shown on open (0-based index) |
| `Cell=<40..160>` | Base cell size (the value remembered by Ctrl+wheel) |
| `[Groups] G0…` | Group names; indices must be contiguous from 0 |
| `[Items] I0…` | Items, five fields: `name\|path\|group\|args\|tooltip` (last three optional) |

## License

MIT, see [LICENSE](LICENSE).
