# BetterStartMenu

A personal Start menu for Windows 11 that opens in place of Windows' own, when you press the Win key or click the
Start button. Written in plain C++ with Direct2D and Windows.UI.Composition: no .NET, no XAML, no installer, about
30 MB of memory.

The interface is in Turkish.

<!-- Screenshots: add images to a docs/ folder and link them here, e.g. ![Menu](docs/menu.png) -->

## Features

- **Pinned apps in groups.** Drag apps to reorder them or to start a new group; drag a group by its handle to move
  it. Groups can be named, outlined or underlined, and laid out in up to four columns.
- **Folders column** beside the apps, with the folders' own icons; add any folder or shell location (This PC,
  Network, Control Panel).
- **All apps list**, A–Z with letter headings or most recently installed first; click an app to pin or unpin it.
- **Search** over installed apps, from the box in the bottom bar.
- **Acrylic background** with adjustable transparency and a choice of tint colours, including Windows' accent colour.
- **Adjustable layout:** menu width and height, tile size and spacing, icon sizes, app names on or off.
- Keyboard navigation with the arrow keys, and power and account menus like Windows' own.

All settings are in the settings panel ("Menü ayarları" in the account menu), and saved to
`%AppData%\MyStartMenu\config.json`.

## Install

1. Download `MyStartMenu.exe` from [Releases](../../releases).
2. Put it in a folder of its own where it will stay, for example `C:\Programs\MyStartMenu\`. (It writes its error
   log next to itself, so pick a folder you can write to.)
3. Run it. The menu opens, and from then on it starts with Windows.

Requires Windows 11, x64. Nothing else to install.

### Good to know

- **Moved the exe?** The start-with-Windows task remembers where the exe was. Right-click the tray icon and turn
  "Başlangıçta çalıştır" off and on again.
- **SmartScreen warning.** The exe is not signed, so Windows may warn about an unrecognised app the first time.
  Choose "More info", then "Run anyway".
- **Antivirus.** To catch the Win key and the Start button the menu uses a system-wide keyboard and mouse hook,
  which some antivirus programs look at twice.
- **First run.** The menu copies your current Windows Start pins once; after that its pins are its own and are not
  synced with Windows'.
- **Turn it off for a while** from the tray icon's menu ("Win tuşu ve Başlat düğmesi bu menüyü açsın"); Windows' own
  Start menu then works as usual.

### Known limitations

- While a menu of a program running as administrator is open (for example some tray icons' right-click menus),
  clicking the Start button opens Windows' own menu: Windows does not let a normal program see those clicks. The
  Win key still works.
- The shell's own panels (search, notification centre, quick settings, jump lists) are closed when the menu opens;
  in rare cases with several of them open at once, one may stay open.

## Build

Needs the Visual Studio 2026 Build Tools (MSVC, C++ workload) and the Windows SDK 10.0.26100.

```
build.cmd
```

builds `bin\MyStartMenu.exe`. There is no project file; `build.cmd` calls `cl` directly, after `vcvars64.bat` from
the Build Tools' default install folder (edit the path in `build.cmd` if yours is elsewhere). The C++ runtime is
linked statically, so the exe runs on its own.

`MyStartMenu.exe --render <folder>` draws the menu's views to PNG files without showing anything, which is handy for
checking layout changes.

## License

[MIT](LICENSE)
