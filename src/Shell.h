#pragma once
// Everything that talks to the Windows shell: installed apps, icons, launching, settings.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "version.h"  // obj\version.h, made by build.cmd from the VERSION file

struct HWND__;  // HWND, without pulling in windows.h

// This build's version, "major.minor.patch" (for the update check).
constexpr wchar_t AppVersion[] = L"" APP_VERSION;

struct AppRef
{
    std::wstring id;    // parsing name inside shell:AppsFolder
    std::wstring name;
    bool folder = false;  // pins only: it opens a folder (a real one or a shell location like Home)
    uint64_t added = 0;   // app list only: when it was installed (FILETIME ticks); 0 if unknown
    int group = 0;        // pinned apps only: the grid section it is in (PinGroup::id); 0 for none
};

// A section of the pinned apps' grid: its apps under its name, in order, after those in no section.
struct PinGroup
{
    int id = 0;
    std::wstring name;
    int column = -1;  // with the sections side by side: which column it stands in (from 0); -1 for none yet
};

// %AppData%\MyStartMenu\config.json, shared with the WPF version (or MYSTARTMENU_CONFIG).
constexpr int IconSizeMin = 24, IconSizeMax = 64, IconSizeStep = 1;
constexpr int FolderIconMin = 16, FolderIconMax = 40;
// The tiles' size, in DIPs: a slider in steps of TileSizeStep (the user's choice, 16.10.2026: four
// sizes to choose from were too few, steps of 1 too fine); a file's value is read to the nearest step.
constexpr int TileSizeMin = 56, TileSizeMax = 160, TileSizeStep = 8;
// The gaps between the tiles to choose from, in DIPs (in place of a slider); a file holding another
// value is read as the nearest.
constexpr int TileGaps[] = { 4, 8, 16, 20 };

template <size_t N>
int Nearest(int const (&sizes)[N], int value)
{
    int best = 0;
    for (int k = 1; k < int(N); k++)
        if ((sizes[k] > value ? sizes[k] - value : value - sizes[k]) < (sizes[best] > value ? sizes[best] - value : value - sizes[best])) best = k;
    return best;
}
// The menu's size, in DIPs (pixels at 100%); the largest is what fits on the screen.
constexpr int MenuWidthMin = 640, MenuHeightMin = 500, MenuSizeStep = 10;

struct Config
{
    bool enabled = true;         // the Win key and Start button open this menu
    double transparency = 0.3;   // 0 is a solid background
    bool border = true;          // thin outline around the menu
    int iconSize = 32;           // pinned and favourite icons, in DIPs (IconSizeMin..IconSizeMax)
    int folderIconSize = 20;     // the folder list's icons, in DIPs (FolderIconMin..FolderIconMax)
    int tileSize = 88;           // a pinned app's tile, its icon in the middle (TileSizeMin..TileSizeMax, by TileSizeStep)
    int tileGap = 8;             // the room between the tiles, across and down alike (one of TileGaps)
    int appsSort = 0;            // the every-apps list: 0 A-Z under letter headings, 1 newest first
    int folderWidth = 9999;    // the menu's folder column, in DIPs; the menu keeps it in its range (default: widest)
    // The menu's size, a choice of a few for each side (the user's wish: the sliders were too free):
    // the width Windows' own (0) up to the screen's (3), the height lower (0), low (1), Windows' own
    // (2), the screen's (3; saved as menuHeightSize); -1 until worked out from the size in DIPs an
    // older version saved (menuWidth x menuHeight).
    int menuWidthStep = -1, menuHeightStep = -1;
    int menuWidth = 834, menuHeight = 866;
    bool fixedBar = false;       // the bottom bar's buttons keep their places in Windows' own 834 wide menu
    bool folders = true;         // the folder column shown; off, the apps take the whole menu
    bool appNames = true;        // the pinned apps' names under their icons; off, the grid is icons alone
    int sectionStyle = 0;  // how the sections show when not dragged: 0 not at all, 1 a faint box, 2 a line under the name
    int sectionColumns = 1;  // the sections side by side in this many columns (1..4); the apps in none above, across
    int sectionScale = 100;  // the sections' tiles, in percent of the rest's (SectionScaleMin..100, in 5s)
    bool sectionNames = true;  // the sections' names over them; off, no name rows at all
    // The body's tint: "" Windows' dark grey, "accent" Windows' accent colour, or "#RRGGBB" one picked;
    // any, a dark shade of it (see AcrylicBrush).
    std::wstring tint;
    bool centerUngrouped = false;  // the apps in no section centred in each row, not from the left
    bool highlightTiles = false;     // each pinned app's tile shown as a faint box
    std::vector<AppRef> favorites;
    // The menu's own pins, apps and folders, in order. They were Windows' Start pins, read once
    // when the file had none (hasPins false); since then the menu keeps its own list.
    std::vector<AppRef> pins;
    bool hasPins = false;
    std::vector<PinGroup> pinGroups;  // the groups the pinned apps are in; only those with apps are saved
};

void Log(std::wstring_view message);

Config LoadConfig();
// Writes the settings and the pins back, keeping favorites, groups and anything else in the file.
void SaveSettings(Config const& config);
// Changes whenever the config file is rewritten, so callers can tell when to reload it.
uint64_t ConfigStamp();

bool AutostartEnabled();
void SetAutostart(bool enabled);
bool AutostartIsOld();  // set up the old way (a Run value), to be moved to the logon task

// Every app in shell:AppsFolder, sorted by name. Takes a few hundred ms; call off the UI thread.
std::vector<AppRef> EnumerateApps();

// The Recycle Bin: its id (as pinned from the folder picker or the app list), whether it holds
// anything (cheap: no shell machinery comes in for it), and the ids its two icons are cached
// under, so the right one shows at once whenever it fills or empties.
inline constexpr wchar_t RecycleBinId[] = L"::{645FF040-5081-101B-9F08-00AA002F954E}";
bool RecycleBinFull();
std::wstring RecycleBinIcon(bool full);

// Premultiplied BGRA pixels of an app's icon.
bool LoadIconPixels(std::wstring const& id, int size, std::vector<uint8_t>& pixels, int& width, int& height);

// ---- Helper process ----
//
// Asking the shell for the app list or an icon loads ~15 MB of its machinery into the asking
// process, for good. So the menu never asks: a short-lived copy of this exe does, leaves the
// results under %LocalAppData%\MyStartMenu, and exits. The two functions below with "Helper"
// in their names are that copy's whole job (MyStartMenu.exe --apps / --icons).

int RunAppsHelper(std::wstring const& outFile);             // writes the app list to outFile
int RunIconsHelper(int size, std::wstring const& listFile);  // caches the icons of the ids in listFile
int RunPinsHelper(std::wstring const& outFile);             // writes Windows' own Start pins to outFile (read once, to start the menu's own list)
int RunPickFolderHelper(std::wstring const& outFile);       // the shell's folder picker; writes the folder chosen, if any, to outFile
int RunEmptyRecycleBinHelper();                             // empties the Recycle Bin, Windows asking first

// Starts the helper with these arguments; when it exits, posts `message` to `notify`.
bool StartHelper(std::wstring const& arguments, HWND__* notify, unsigned message);

std::wstring CacheFolder();   // %LocalAppData%\MyStartMenu, created if missing
std::wstring AppListFile();   // where --apps writes the list
std::vector<AppRef> ReadAppList();
std::wstring PinListFile();   // where --pins writes the pins, in Windows' order
std::vector<AppRef> ReadPinList();
std::wstring PickedFolderFile();  // where --pick-folder writes the folder chosen
std::vector<AppRef> ReadPickedFolder();
void WriteIdList(std::wstring const& file, std::vector<std::wstring> const& ids);

// An icon the helper cached; `written` is when (FILETIME ticks).
bool ReadCachedIcon(std::wstring const& id, int size, std::vector<uint8_t>& pixels, int& width, int& height, uint64_t& written);
uint64_t FileTimeNow();

// Posts `message` to `notify` whenever apps may have been installed, removed or updated: a change
// in either Start menu Programs folder, or in the current user's list of packaged apps.
void WatchAppChanges(HWND__* notify, unsigned message);

// Replaces this process' environment with the user's default one, as Explorer gives programs it
// starts. Whatever started the menu (e.g. a VS Code terminal, whose ELECTRON_RUN_AS_NODE=1 made
// Code.exe run as Node and exit) would otherwise leak into every app the menu launches.
void UseUserEnvironment();

// An app's id is its name inside shell:AppsFolder, or, for pins of shortcuts, the full path of
// the shortcut (or any file or folder).
void LaunchApp(std::wstring const& id);
void OpenTarget(std::wstring const& target);  // a path, URI or shell: location; %VARS% are expanded

// Premultiplied BGRA pixels of the user's account picture, in the smallest stored size that is
// at least minSize pixels wide. False if the user has none.
bool LoadAccountPicture(int minSize, std::vector<uint8_t>& pixels, int& width, int& height);

enum class PowerAction { Lock, SignOut, Sleep, Shutdown, Restart, UpdateAndShutdown, UpdateAndRestart };
void RunPowerAction(PowerAction action);

// Windows Update has updates waiting to install at the next shutdown or restart, so the
// power menu should offer "update and shut down / restart", as the Windows menu does.
bool UpdatesPendingAtShutdown();
// Which of the bottom bar's places Windows' Start settings turn on (Settings > Personalization >
// Start > Folders). Only these two: its folders are left to the folders column.
struct StartPlaces { bool settings, network; };
StartPlaces ReadStartPlaces();
