#include "Shell.h"
#include "Json.h"

#include <windows.h>
#include <shellapi.h>
#include <shlobj_core.h>
#include <shobjidl_core.h>
#include <propsys.h>
#include <sddl.h>
#include <powrprof.h>
#include <wincodec.h>
#include <taskschd.h>
#include <userenv.h>
#define SECURITY_WIN32
#include <security.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <thread>

using Microsoft::WRL::ComPtr;

namespace
{
constexpr wchar_t AppsFolder[] = L"shell:AppsFolder\\";

// "C:\..." or "\\server\...": a file or folder itself, not an app inside shell:AppsFolder.
// A path, or a shell location with none ("::{GUID}", This PC for one), as the folder picker gives them.
bool IsPath(std::wstring_view id)
{
    return (id.size() > 2 && id[1] == L':' && id[2] == L'\\') || id.starts_with(L"\\\\") || id.starts_with(L"::{");
}

// What to hand the shell for an app id.
std::wstring ShellName(std::wstring const& id) { return IsPath(id) ? id : AppsFolder + id; }

std::string ToUtf8(std::wstring_view text)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring ReadUtf8File(std::wstring const& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    std::string bytes((std::istreambuf_iterator<char>(file)), {});
    if (bytes.starts_with("\xEF\xBB\xBF")) bytes.erase(0, 3);
    int n = MultiByteToWideChar(CP_UTF8, 0, bytes.data(), int(bytes.size()), nullptr, 0);
    std::wstring text(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, bytes.data(), int(bytes.size()), text.data(), n);
    return text;
}

std::wstring ConfigPath()
{
    wchar_t overridePath[MAX_PATH];
    if (GetEnvironmentVariableW(L"MYSTARTMENU_CONFIG", overridePath, MAX_PATH)) return overridePath;
    PWSTR appData{};
    SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData);
    std::wstring path = std::wstring(appData ? appData : L"") + L"\\MyStartMenu\\config.json";
    CoTaskMemFree(appData);
    return path;
}

constexpr wchar_t RunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t RunValue[] = L"MyStartMenu";
}

Config LoadConfig()
{
    Config config;
    JsonValue root;
    if (!JsonParser(ReadUtf8File(ConfigPath())).Parse(root)) return config;

    config.enabled = root.Flag(L"enabled", config.enabled);
    config.transparency = std::clamp(root.Num(L"transparency", config.transparency), 0.0, 0.9);
    config.border = root.Flag(L"border", config.border);
    config.fixedBar = root.Flag(L"fixedBar", config.fixedBar);
    config.folders = root.Flag(L"folders", config.folders);
    config.appNames = root.Flag(L"appNames", config.appNames);
    // A choice of three; before, a switch for the box (highlightSections).
    if (root.Find(L"sectionStyle")) config.sectionStyle = std::clamp(int(root.Num(L"sectionStyle", 0)), 0, 2);
    else config.sectionStyle = root.Flag(L"highlightSections", false) ? 1 : 0;
    config.sectionColumns = std::clamp(int(root.Num(L"sectionColumns", config.sectionColumns)), 1, 4);
    {
        // First it was a choice of four (0 to 3: whole, 3/4, 2/3, 1/2), then percent in 5s.
        int scale = int(root.Num(L"sectionScale", config.sectionScale));
        constexpr int Choices[] = {100, 75, 65, 50};
        if (scale >= 0 && scale <= 3)
            scale = Choices[scale];
        config.sectionScale = std::clamp((scale + 2) / 5 * 5, 50, 100);
    }
    config.sectionNames = root.Flag(L"sectionNames", config.sectionNames);
    config.centerUngrouped = root.Flag(L"centerUngrouped", config.centerUngrouped);
    // A switch for the accent colour first (accentTint), then a choice of colours.
    config.tint = root.Find(L"tint") ? std::wstring(root.Str(L"tint")) : root.Flag(L"accentTint", false) ? L"accent" : L"";
    config.highlightTiles = root.Flag(L"highlightTiles", config.highlightTiles);
    config.iconSize = std::clamp(int(root.Num(L"iconSize", config.iconSize)), IconSizeMin, IconSizeMax);
    config.folderIconSize = std::clamp(int(root.Num(L"folderIconSize", config.folderIconSize)), FolderIconMin, FolderIconMax);
    {
        int size = std::clamp(int(root.Num(L"tileSize", config.tileSize)), TileSizeMin, TileSizeMax);
        config.tileSize = TileSizeMin + (size - TileSizeMin + TileSizeStep / 2) / TileSizeStep * TileSizeStep;
    }
    config.tileGap = TileGaps[Nearest(TileGaps, int(root.Num(L"tileGap", config.tileGap)))];
    config.folderWidth = int(root.Num(L"folderWidth", config.folderWidth));
    // Before sizes in DIPs the size was the grid's columns x rows at Windows' 96 x 84 cell.
    if (root.Find(L"menuColumns")) config.menuWidth = 258 + int(root.Num(L"menuColumns", 6)) * 96;
    if (root.Find(L"menuRows")) config.menuHeight = 194 + int(root.Num(L"menuRows", 8)) * 84;
    config.menuWidth = std::max(int(root.Num(L"menuWidth", config.menuWidth)), MenuWidthMin);
    config.menuHeight = std::max(int(root.Num(L"menuHeight", config.menuHeight)), MenuHeightMin);
    if (root.Find(L"menuWidthStep")) config.menuWidthStep = std::clamp(int(root.Num(L"menuWidthStep", 0)), 0, 3);
    // The height as one of four (lower, low, Windows' own, the screen's); before, one of three
    // without the lowest (menuHeightStep).
    if (root.Find(L"menuHeightSize")) config.menuHeightStep = std::clamp(int(root.Num(L"menuHeightSize", 2)), 0, 3);
    else if (root.Find(L"menuHeightStep")) config.menuHeightStep = std::clamp(int(root.Num(L"menuHeightStep", 1)), 0, 2) + 1;
    config.appsSort = int(root.Num(L"appsSort", config.appsSort)) == 1 ? 1 : 0;  // 2 was "by letter", now A-Z itself
    if (auto favorites = root.Find(L"favorites"); favorites && favorites->type == JsonValue::Type::Array)
        for (auto const& app : favorites->items)
            if (!app.Str(L"id").empty()) config.favorites.push_back({ std::wstring(app.Str(L"id")), std::wstring(app.Str(L"name")) });
    if (auto pins = root.Find(L"pins"); pins && pins->type == JsonValue::Type::Array)
    {
        config.hasPins = true;
        for (auto const& pin : pins->items)
            if (!pin.Str(L"id").empty())
                config.pins.push_back({ std::wstring(pin.Str(L"id")), std::wstring(pin.Str(L"name")), pin.Flag(L"folder", false), 0,
                                        int(pin.Num(L"group", 0)) });
    }
    if (auto groups = root.Find(L"pinGroups"); groups && groups->type == JsonValue::Type::Array)
        for (auto const& group : groups->items)
            if (int id = int(group.Num(L"id", 0)); id > 0)
            {
                // A group's name as it started, never changed, under the old word for a group.
                std::wstring name(group.Str(L"name"));
                if (name == L"Yeni bölüm") name = L"Yeni grup";
                config.pinGroups.push_back({ id, name, int(group.Num(L"column", -1)) });
            }
    return config;
}

void SaveSettings(Config const& config)
{
    std::wstring path = ConfigPath();
    JsonValue root;
    if (!JsonParser(ReadUtf8File(path)).Parse(root) || root.type != JsonValue::Type::Object)
    {
        root = {};
        root.Set(L"favorites", JsonValue::EmptyArray());
        root.Set(L"groups", JsonValue::EmptyArray());
    }
    root.Set(L"enabled", JsonValue::Bool(config.enabled));
    root.Set(L"transparency", JsonValue::Number(config.transparency));
    root.Set(L"border", JsonValue::Bool(config.border));
    root.Set(L"fixedBar", JsonValue::Bool(config.fixedBar));
    root.Set(L"folders", JsonValue::Bool(config.folders));
    root.Remove(L"fixedSearch");  // the search box is always centred over the apps side now
    root.Set(L"appNames", JsonValue::Bool(config.appNames));
    root.Remove(L"highlightSections");
    root.Set(L"sectionStyle", JsonValue::Number(config.sectionStyle));
    root.Set(L"sectionColumns", JsonValue::Number(config.sectionColumns));
    root.Set(L"sectionScale", JsonValue::Number(config.sectionScale));
    root.Set(L"sectionNames", JsonValue::Bool(config.sectionNames));
    root.Set(L"centerUngrouped", JsonValue::Bool(config.centerUngrouped));
    root.Remove(L"accentTint");
    root.Set(L"tint", JsonValue::String(config.tint));
    root.Set(L"highlightTiles", JsonValue::Bool(config.highlightTiles));
    root.Set(L"iconSize", JsonValue::Number(config.iconSize));
    root.Set(L"folderIconSize", JsonValue::Number(config.folderIconSize));
    root.Remove(L"material");  // the glass, tried and taken out (16.10.2026)
    root.Remove(L"iconGap");  // the room around each icon, before the tiles had a size of their own
    root.Set(L"tileSize", JsonValue::Number(config.tileSize));
    root.Set(L"tileGap", JsonValue::Number(config.tileGap));
    root.Set(L"folderWidth", JsonValue::Number(config.folderWidth));
    root.Remove(L"menuColumns");  // the size as columns x rows, before menuWidth and menuHeight
    root.Remove(L"menuRows");
    // The size in DIPs, before it was a choice of a few; read once to pick the nearest.
    root.Remove(L"menuWidth");
    root.Remove(L"menuHeight");
    if (config.menuWidthStep >= 0) root.Set(L"menuWidthStep", JsonValue::Number(config.menuWidthStep));
    root.Remove(L"menuHeightStep");
    if (config.menuHeightStep >= 0) root.Set(L"menuHeightSize", JsonValue::Number(config.menuHeightStep));
    root.Set(L"appsSort", JsonValue::Number(config.appsSort));
    if (config.hasPins)
    {
        JsonValue pins = JsonValue::EmptyArray();
        for (auto const& pin : config.pins)
        {
            JsonValue item;
            item.Set(L"id", JsonValue::String(pin.id));
            item.Set(L"name", JsonValue::String(pin.name));
            if (pin.folder) item.Set(L"folder", JsonValue::Bool(true));
            if (pin.group) item.Set(L"group", JsonValue::Number(pin.group));
            pins.items.push_back(std::move(item));
        }
        root.Set(L"pins", std::move(pins));
        // Only the groups (the grid.s sections) some pin is in: an emptied one goes.
        JsonValue groups = JsonValue::EmptyArray();
        for (auto const& group : config.pinGroups)
            if (std::any_of(config.pins.begin(), config.pins.end(), [&](AppRef const& pin) { return pin.group == group.id; }))
            {
                JsonValue item;
                item.Set(L"id", JsonValue::Number(group.id));
                item.Set(L"name", JsonValue::String(group.name));
                if (group.column >= 0) item.Set(L"column", JsonValue::Number(group.column));
                groups.items.push_back(std::move(item));
            }
        root.Set(L"pinGroups", std::move(groups));
    }

    std::wstring text;
    WriteJson(root, text);
    std::string bytes = ToUtf8(text);

    // Write beside it and swap in, so a crash never leaves a half-written config.
    CreateDirectoryW(path.substr(0, path.find_last_of(L'\\')).c_str(), nullptr);
    std::wstring temp = path + L".tmp";
    {
        std::ofstream file(temp, std::ios::binary | std::ios::trunc);
        if (!file.write(bytes.data(), std::streamsize(bytes.size()))) { Log(L"config write failed"); return; }
    }
    if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) Log(L"config replace failed: " + std::to_wstring(GetLastError()));
}

uint64_t ConfigStamp()
{
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(ConfigPath().c_str(), GetFileExInfoStandard, &data)) return 0;
    return (uint64_t(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime;
}

// Starting at sign-in is a scheduled task with a logon trigger, not a Run value: Explorer runs the
// Run values one after another, the machine's first, waiting for each, and at a cold sign-in the
// menu came ~7 s after the desktop (measured in Shell-Core's log, 16.10.2026). The task starts as
// soon as the user is signed in, alongside the rest. A user can make one for themselves without
// being an administrator. Its defaults would stop it after 3 days, and on battery, and run it at
// below normal priority; the XML sets all three.
namespace
{
constexpr wchar_t TaskName[] = L"MyStartMenu";

ComPtr<ITaskFolder> TaskRoot()
{
    ComPtr<ITaskService> service;
    if (FAILED(CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&service)))) return nullptr;
    if (FAILED(service->Connect({}, {}, {}, {}))) return nullptr;
    ComPtr<ITaskFolder> root;
    BSTR path = SysAllocString(L"\\");
    service->GetFolder(path, &root);
    SysFreeString(path);
    return root;
}

std::wstring XmlEscaped(std::wstring_view text)
{
    std::wstring out;
    for (wchar_t c : text)
        out += c == L'&' ? L"&amp;" : c == L'<' ? L"&lt;" : c == L'>' ? L"&gt;" : std::wstring(1, c);
    return out;
}
}

bool AutostartEnabled()
{
    auto root = TaskRoot();
    if (!root) return false;
    ComPtr<IRegisteredTask> task;
    BSTR name = SysAllocString(TaskName);
    bool found = SUCCEEDED(root->GetTask(name, &task));
    SysFreeString(name);
    VARIANT_BOOL enabled = VARIANT_FALSE;
    return found && SUCCEEDED(task->get_Enabled(&enabled)) && enabled;
}

void SetAutostart(bool enabled)
{
    RegDeleteKeyValueW(HKEY_CURRENT_USER, RunKey, RunValue);  // how older versions started
    auto root = TaskRoot();
    if (!root) { Log(L"autostart: no task scheduler"); return; }
    BSTR name = SysAllocString(TaskName);
    if (!enabled)
    {
        root->DeleteTask(name, 0);
        SysFreeString(name);
        return;
    }
    wchar_t exe[MAX_PATH], user[256];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    ULONG userLength = ULONG(std::size(user));
    if (!GetUserNameExW(NameSamCompatible, user, &userLength)) user[0] = 0;
    std::wstring xml = LR"(<?xml version="1.0" encoding="UTF-16"?>
<Task version="1.2" xmlns="http://schemas.microsoft.com/windows/2004/02/mit/task">
  <RegistrationInfo><Description>Başlat Menüm: starts hidden at sign-in, waiting for the Win key.</Description></RegistrationInfo>
  <Triggers><LogonTrigger><Enabled>true</Enabled><UserId>)" + XmlEscaped(user) + LR"(</UserId></LogonTrigger></Triggers>
  <Principals><Principal id="Author"><UserId>)" + XmlEscaped(user) + LR"(</UserId><LogonType>InteractiveToken</LogonType><RunLevel>LeastPrivilege</RunLevel></Principal></Principals>
  <Settings>
    <MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>
    <DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>
    <StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>
    <ExecutionTimeLimit>PT0S</ExecutionTimeLimit>
    <Priority>4</Priority>
    <AllowStartOnDemand>true</AllowStartOnDemand>
    <Enabled>true</Enabled>
  </Settings>
  <Actions Context="Author"><Exec><Command>)" + XmlEscaped(exe) + LR"(</Command><Arguments>--background</Arguments></Exec></Actions>
</Task>)";
    BSTR text = SysAllocString(xml.c_str());
    ComPtr<IRegisteredTask> task;
    HRESULT hr = root->RegisterTask(name, text, TASK_CREATE_OR_UPDATE, {}, {}, TASK_LOGON_INTERACTIVE_TOKEN, {}, &task);
    if (FAILED(hr)) Log(L"autostart: task not registered: " + std::to_wstring(hr));
    SysFreeString(text);
    SysFreeString(name);
}

bool AutostartIsOld()
{
    return RegGetValueW(HKEY_CURRENT_USER, RunKey, RunValue, RRF_RT_REG_SZ, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
}

void Log(std::wstring_view message)
{
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring path = exe;
    path = path.substr(0, path.find_last_of(L'\\') + 1) + L"error.log";
    std::ofstream(path, std::ios::app | std::ios::binary) << ToUtf8(message) << "\r\n";
}

namespace
{
std::wstring StringProperty(IShellItem2* item, wchar_t const* name)
{
    PROPERTYKEY key;
    PWSTR text{};
    if (FAILED(PSGetPropertyKeyFromName(name, &key)) || FAILED(item->GetString(key, &text))) return {};
    std::wstring result = text;
    CoTaskMemFree(text);
    return result;
}

uint64_t CreationTime(std::wstring const& path)
{
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (path.empty() || !GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return 0;
    return uint64_t(data.ftCreationTime.dwHighDateTime) << 32 | data.ftCreationTime.dwLowDateTime;
}

// When an app was installed, as near as the disk tells; 0 for what is not a program at all (a
// folder, a document, a web link), which "newest first" leaves out.
//  - packaged app: its package folder's creation (a store update makes a new one, so counts as new);
//  - program file (.exe and the like): the folder it is in, made by its installer. Its Start menu
//    shortcut can be newer (made again, or by hand), and Windows' own tools fall back to their
//    install date with System32;
//  - a link into another program (steam://rungameid/...): its Start menu shortcut, which the
//    program writes as it installs the game.
uint64_t AddedTime(IShellItem* item)
{
    ComPtr<IShellItem2> item2;
    if (FAILED(item->QueryInterface(IID_PPV_ARGS(&item2)))) return 0;
    if (auto package = StringProperty(item2.Get(), L"System.AppUserModel.PackageInstallPath"); !package.empty())
        return CreationTime(package);

    std::wstring target = StringProperty(item2.Get(), L"System.Link.TargetParsingPath");
    if (size_t colon = target.find(L':'); colon > 1 && colon != std::wstring::npos)  // a URL, not a drive letter
    {
        std::wstring scheme = target.substr(0, colon);
        if (_wcsicmp(scheme.c_str(), L"http") == 0 || _wcsicmp(scheme.c_str(), L"https") == 0) return 0;
        PROPERTYKEY key;
        PROPVARIANT value;
        PropVariantInit(&value);
        wchar_t shortcut[MAX_PATH];
        uint64_t time = 0;
        // The shortcut as an item id list, in a byte vector.
        if (SUCCEEDED(PSGetPropertyKeyFromName(L"System.AppUserModel.BestShortcut", &key)) &&
            SUCCEEDED(item2->GetProperty(key, &value)) && value.vt == (VT_VECTOR | VT_UI1) && value.caub.cElems &&
            SHGetPathFromIDListW(reinterpret_cast<PCIDLIST_ABSOLUTE>(value.caub.pElems), shortcut))
            time = CreationTime(shortcut);
        PropVariantClear(&value);
        return time;
    }

    constexpr wchar_t const* programs[] = { L".exe", L".msc", L".cpl", L".com", L".bat", L".cmd" };
    size_t dot = target.find_last_of(L".\\");
    if (dot == std::wstring::npos || target[dot] != L'.') return 0;
    if (std::none_of(std::begin(programs), std::end(programs), [&](wchar_t const* ext) { return _wcsicmp(target.c_str() + dot, ext) == 0; }))
        return 0;
    return CreationTime(target.substr(0, target.find_last_of(L'\\')));
}
}

std::vector<AppRef> EnumerateApps()
{
    std::vector<AppRef> apps;
    ComPtr<IShellItem> folder;
    if (FAILED(SHGetKnownFolderItem(FOLDERID_AppsFolder, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(&folder)))) return apps;
    ComPtr<IEnumShellItems> items;
    if (FAILED(folder->BindToHandler(nullptr, BHID_EnumItems, IID_PPV_ARGS(&items)))) return apps;

    ComPtr<IShellItem> item;
    while (items->Next(1, &item, nullptr) == S_OK)
    {
        PWSTR name{}, id{};
        if (SUCCEEDED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &name)) &&
            SUCCEEDED(item->GetDisplayName(SIGDN_PARENTRELATIVEPARSING, &id)) && *name && *id)
            apps.push_back({ id, name, false, AddedTime(item.Get()) });
        CoTaskMemFree(name);
        CoTaskMemFree(id);
    }

    std::sort(apps.begin(), apps.end(), [](AppRef const& a, AppRef const& b) {
        return CompareStringEx(LOCALE_NAME_USER_DEFAULT, LINGUISTIC_IGNORECASE, a.name.c_str(), int(a.name.size()),
                               b.name.c_str(), int(b.name.size()), nullptr, nullptr, 0) == CSTR_LESS_THAN;
    });
    return apps;
}

namespace
{
bool BitmapPixels(HBITMAP hbitmap, std::vector<uint8_t>& pixels, int& width, int& height);

// The Recycle Bin's icon when full or empty (RecycleBinIcon), whichever it is now: as Explorer
// draws it, from its DefaultIcon ("path,-id"; the user's own, if set, under HKCU).
bool LoadRecycleBinIcon(bool full, int size, std::vector<uint8_t>& pixels, int& width, int& height)
{
    constexpr wchar_t key[] = L"\\CLSID\\{645FF040-5081-101B-9F08-00AA002F954E}\\DefaultIcon";
    wchar_t source[MAX_PATH + 16];
    DWORD bytes = sizeof source;
    if (RegGetValueW(HKEY_CURRENT_USER, (std::wstring(L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer") + key).c_str(),
                     full ? L"Full" : L"Empty", RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr, source, &bytes) != ERROR_SUCCESS)
    {
        bytes = sizeof source;
        if (RegGetValueW(HKEY_CLASSES_ROOT, key + 1, full ? L"Full" : L"Empty", RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ,
                         nullptr, source, &bytes) != ERROR_SUCCESS)
            return false;
    }
    std::wstring path = source;
    int index = 0;
    if (size_t comma = path.rfind(L','); comma != std::wstring::npos)
    {
        index = _wtoi(path.c_str() + comma + 1);
        path.resize(comma);
    }
    HICON icon{};
    if (SHDefExtractIconW(path.c_str(), index, 0, &icon, nullptr, MAKELONG(size, 0)) != S_OK || !icon) return false;
    ICONINFO info{};
    bool loaded = GetIconInfo(icon, &info) && BitmapPixels(info.hbmColor, pixels, width, height);
    if (info.hbmMask) DeleteObject(info.hbmMask);
    DestroyIcon(icon);  // hbmColor is deleted by BitmapPixels
    return loaded;
}
}

// Copies the pixels out by hand and returns them premultiplied, whichever way the shell gave them.
bool LoadIconPixels(std::wstring const& id, int size, std::vector<uint8_t>& pixels, int& width, int& height)
{
    if (id == RecycleBinIcon(true) || id == RecycleBinIcon(false))
        return LoadRecycleBinIcon(id == RecycleBinIcon(true), size, pixels, width, height);
    ComPtr<IShellItemImageFactory> factory;
    if (FAILED(SHCreateItemFromParsingName(ShellName(id).c_str(), nullptr, IID_PPV_ARGS(&factory)))) return false;
    HBITMAP hbitmap{};
    if (FAILED(factory->GetImage({ size, size }, SIIGBF_ICONONLY, &hbitmap)) || !hbitmap) return false;
    return BitmapPixels(hbitmap, pixels, width, height);
}

namespace
{
// Takes the bitmap (and deletes it).
bool BitmapPixels(HBITMAP hbitmap, std::vector<uint8_t>& pixels, int& width, int& height)
{
    if (!hbitmap) return false;
    BITMAP bm{};
    GetObjectW(hbitmap, sizeof bm, &bm);
    width = bm.bmWidth;
    height = bm.bmHeight;
    BITMAPINFO info{};
    info.bmiHeader = { sizeof(BITMAPINFOHEADER), width, -height, 1, 32, BI_RGB };
    pixels.assign(size_t(width) * height * 4, 0);

    HDC dc = GetDC(nullptr);
    int lines = GetDIBits(dc, hbitmap, 0, height, pixels.data(), &info, DIB_RGB_COLORS);
    ReleaseDC(nullptr, dc);
    DeleteObject(hbitmap);
    if (!lines) return false;

    // Icons without an alpha channel come back fully transparent; make them opaque.
    bool hasAlpha = false;
    for (size_t i = 3; i < pixels.size() && !hasAlpha; i += 4) hasAlpha = pixels[i] != 0;
    if (!hasAlpha)
        for (size_t i = 3; i < pixels.size(); i += 4) pixels[i] = 255;

    // Despite appearances the shell mostly hands back straight alpha: edge pixels such as white at
    // 3% opacity. Taken as premultiplied, those draw almost fully white and outline the icon in a
    // light fringe. A colour above its alpha gives straight alpha away; then premultiply here.
    bool straight = false;
    for (size_t i = 0; i < pixels.size() && !straight; i += 4)
        straight = std::max({ pixels[i], pixels[i + 1], pixels[i + 2] }) > pixels[i + 3];
    if (straight)
        for (size_t i = 0; i < pixels.size(); i += 4)
            for (int c = 0; c < 3; c++) pixels[i + c] = uint8_t((pixels[i + c] * pixels[i + 3] + 127) / 255);
    return true;
}
}

std::wstring RecycleBinIcon(bool full) { return RecycleBinId + std::wstring(full ? L"|full" : L"|empty"); }

bool RecycleBinFull()
{
    SHQUERYRBINFO info{ sizeof info };
    return SUCCEEDED(SHQueryRecycleBinW(nullptr, &info)) && info.i64NumItems > 0;
}

int RunEmptyRecycleBinHelper()
{
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    // Windows' own confirmation, progress and sound, as Explorer's "Empty Recycle Bin".
    SHEmptyRecycleBinW(nullptr, nullptr, 0);
    return 0;
}

void UseUserEnvironment()
{
    HANDLE token{};
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE, &token)) return;
    void *block{};
    bool ok = CreateEnvironmentBlock(&block, token, FALSE);
    CloseHandle(token);
    if (!ok) return;

    // Our own variables (MYSTARTMENU_CONFIG for tests) and the hidden "=C:" current directories stay.
    auto kept = [](std::wstring_view name)
    { return name.empty() || name.starts_with(L"MYSTARTMENU_"); };
    auto names = [](wchar_t const *strings)
    {
        std::vector<std::wstring> out;
        for (wchar_t const *s = strings; *s; s += wcslen(s) + 1)
            if (wchar_t const *eq = wcschr(s + 1, L'='))
                out.emplace_back(s, eq);
        return out;
    };
    if (wchar_t *current = GetEnvironmentStringsW())
    {
        for (auto const& name : names(current))
            if (!kept(name)) SetEnvironmentVariableW(name.c_str(), nullptr);
        FreeEnvironmentStringsW(current);
    }
    for (wchar_t const *s = static_cast<wchar_t const*>(block); *s; s += wcslen(s) + 1)
        if (wchar_t const *eq = wcschr(s + 1, L'='); eq && !kept(std::wstring_view(s, eq - s)))
            SetEnvironmentVariableW(std::wstring(s, eq).c_str(), eq + 1);
    DestroyEnvironmentBlock(block);
}

namespace
{
// ShellExecuteW returns only once the program has started (Blender's launcher: 200 ms, measured),
// and the menu began closing only after it. On its own thread the menu closes at once; it is still
// the foreground process meanwhile, so the program may still come to the front.
void ShellOpenAsync(std::function<void()> open)
{
    std::thread([open = std::move(open)]
                {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        open();
        CoUninitialize(); }).detach();
}
}

void LaunchApp(std::wstring const& id)
{
    ShellOpenAsync([target = ShellName(id)]
                   {
        if (reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) <= 32)
            ShellExecuteW(nullptr, L"open", L"explorer.exe", (L"\"" + target + L"\"").c_str(), nullptr, SW_SHOWNORMAL); });
}

void OpenTarget(std::wstring const& target)
{
    wchar_t expanded[MAX_PATH];
    ExpandEnvironmentStringsW(target.c_str(), expanded, MAX_PATH);
    ShellOpenAsync([target = std::wstring(expanded)]
                   { ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL); });
}

namespace
{
std::wstring UserSid()
{
    HANDLE token{};
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return {};
    std::vector<uint8_t> buffer(256);
    DWORD size = 0;
    bool ok = GetTokenInformation(token, TokenUser, buffer.data(), DWORD(buffer.size()), &size);
    CloseHandle(token);
    PWSTR text{};
    if (!ok || !ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &text)) return {};
    std::wstring sid = text;
    LocalFree(text);
    return sid;
}

// Decodes an image file, shrunk with a high-quality filter so its shorter side is `size` pixels
// when it is larger.
bool DecodeImage(std::wstring const& path, int size, std::vector<uint8_t>& pixels, int& width, int& height)
{
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICBitmapScaler> scaler;
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) ||
        FAILED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame)))
        return false;
    ComPtr<IWICBitmapSource> source = frame;
    UINT fw = 0, fh = 0;
    frame->GetSize(&fw, &fh);
    if (UINT shorter = std::min(fw, fh); size > 0 && shorter > UINT(size))
    {
        UINT sw = std::max(1u, UINT(uint64_t(fw) * size / shorter)), sh = std::max(1u, UINT(uint64_t(fh) * size / shorter));
        if (SUCCEEDED(wic->CreateBitmapScaler(&scaler)) &&
            SUCCEEDED(scaler->Initialize(frame.Get(), sw, sh, WICBitmapInterpolationModeHighQualityCubic)))
            source = scaler;
    }
    if (FAILED(wic->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(source.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                     WICBitmapPaletteTypeCustom)))
        return false;
    UINT w = 0, h = 0;
    converter->GetSize(&w, &h);
    width = int(w);
    height = int(h);
    pixels.assign(size_t(w) * h * 4, 0);
    return SUCCEEDED(converter->CopyPixels(nullptr, w * 4, UINT(pixels.size()), pixels.data()));
}

// Shutting down or restarting needs this privilege switched on in our own token.
void EnableShutdownPrivilege()
{
    HANDLE token{};
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) return;
    TOKEN_PRIVILEGES privileges{ 1 };
    privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (LookupPrivilegeValueW(nullptr, SE_SHUTDOWN_NAME, &privileges.Privileges[0].Luid))
        AdjustTokenPrivileges(token, FALSE, &privileges, 0, nullptr, nullptr);
    CloseHandle(token);
}
}

bool LoadAccountPicture(int minSize, std::vector<uint8_t>& pixels, int& width, int& height)
{
    // Windows stores the picture in several sizes, one value each (Image32, Image96, ...).
    constexpr int sizes[] = { 32, 40, 48, 64, 96, 192, 208, 240, 424, 448, 1080 };
    std::wstring sid = UserSid();
    if (sid.empty()) return false;
    std::wstring key = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\AccountPicture\\Users\\" + sid;
    // Windows' small sizes are coarse JPEGs (blocky at 40, the user saw it): first one at least four
    // times as big, shrunk here to the exact size; then the big enough ones from the smallest up,
    // then the smaller ones from the largest down.
    std::vector<int> order;
    for (int size : sizes) if (size >= 4 * minSize) order.push_back(size);
    for (int size : sizes) if (size >= minSize && size < 4 * minSize) order.push_back(size);
    for (auto it = std::rbegin(sizes); it != std::rend(sizes); ++it) if (*it < minSize) order.push_back(*it);
    for (int size : order)
    {
        wchar_t path[MAX_PATH];
        DWORD bytes = sizeof path;
        std::wstring value = L"Image" + std::to_wstring(size);
        if (RegGetValueW(HKEY_LOCAL_MACHINE, key.c_str(), value.c_str(), RRF_RT_REG_SZ, nullptr, path, &bytes) == ERROR_SUCCESS &&
            DecodeImage(path, minSize, pixels, width, height))
            return true;
    }
    return false;
}

void RunPowerAction(PowerAction action)
{
    constexpr DWORD reason = SHTDN_REASON_MAJOR_OTHER | SHTDN_REASON_MINOR_OTHER | SHTDN_REASON_FLAG_PLANNED;
    BOOL ok = TRUE;
    switch (action)
    {
    case PowerAction::Lock: ok = LockWorkStation(); break;
    case PowerAction::SignOut: ok = ExitWindowsEx(EWX_LOGOFF, reason); break;
    case PowerAction::Sleep: ok = SetSuspendState(FALSE, FALSE, FALSE); break;
    case PowerAction::Shutdown: EnableShutdownPrivilege(); ok = ExitWindowsEx(EWX_POWEROFF, reason); break;
    case PowerAction::Restart: EnableShutdownPrivilege(); ok = ExitWindowsEx(EWX_REBOOT, reason); break;
    case PowerAction::UpdateAndShutdown:
    case PowerAction::UpdateAndRestart:
    {
        // Installs the pending updates on the way down; with none pending it is a plain shutdown or restart.
        EnableShutdownPrivilege();
        DWORD flags = SHUTDOWN_INSTALL_UPDATES | (action == PowerAction::UpdateAndShutdown ? SHUTDOWN_POWEROFF : SHUTDOWN_RESTART);
        DWORD error = InitiateShutdownW(nullptr, nullptr, 0, flags,
                                        SHTDN_REASON_MAJOR_OPERATINGSYSTEM | SHTDN_REASON_MINOR_HOTFIX | SHTDN_REASON_FLAG_PLANNED);
        ok = error == ERROR_SUCCESS;
        SetLastError(error);
        break;
    }
    }
    if (!ok) Log(L"power action " + std::to_wstring(int(action)) + L" failed: " + std::to_wstring(GetLastError()));
}

bool UpdatesPendingAtShutdown()
{
    // Update Orchestrator sets this for the shell's shutdown menus while updates wait for a
    // shutdown; it is 0 or missing otherwise. Undocumented, but it is what the Windows UI reads.
    DWORD options = 0, size = sizeof options;
    RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\WindowsUpdate\\Orchestrator", L"ShutdownFlyoutOptions",
                 RRF_RT_REG_DWORD, nullptr, &options, &size);
    return options != 0;
}

StartPlaces ReadStartPlaces()
{
    // Settings > Personalization > Start > Folders keeps the places turned on here, one 16-byte GUID
    // each. Undocumented; read on Windows 11 26300. Without the value (never set), what this menu
    // had before: Settings only.
    static constexpr GUID Settings{ 0x52730886, 0x51AA, 0x4243, { 0x9F, 0x7B, 0x27, 0x76, 0x58, 0x46, 0x59, 0xD4 } };
    static constexpr GUID Network{ 0xFE758144, 0x080D, 0x42AE, { 0x8B, 0xDA, 0x34, 0xED, 0x97, 0xB6, 0x63, 0x94 } };
    BYTE places[64 * sizeof(GUID)];
    DWORD size = sizeof places;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Start", L"VisiblePlaces",
                     RRF_RT_REG_BINARY, nullptr, places, &size) != ERROR_SUCCESS)
        return { true, false };
    StartPlaces shown{};
    for (DWORD at = 0; at + sizeof(GUID) <= size; at += sizeof(GUID))
    {
        GUID place;
        memcpy(&place, places + at, sizeof place);
        shown.settings |= place == Settings;
        shown.network |= place == Network;
    }
    return shown;
}

// ---- Helper process ----

namespace
{
std::wstring LocalAppDataFolder()
{
    PWSTR local{};
    SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local);
    std::wstring path = local ? local : L"";
    CoTaskMemFree(local);
    return path;
}

// Writes beside the target and swaps it in, so a reader never sees half a file.
bool WriteFileAtomic(std::wstring const& path, void const* data, size_t size)
{
    std::wstring temp = path + L".tmp" + std::to_wstring(GetCurrentProcessId());
    {
        std::ofstream file(temp, std::ios::binary | std::ios::trunc);
        if (!file.write(static_cast<char const*>(data), std::streamsize(size))) return false;
    }
    if (MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) return true;
    DeleteFileW(temp.c_str());
    return false;
}

std::vector<std::wstring> ReadLines(std::wstring const& file)
{
    std::vector<std::wstring> lines;
    std::wstring text = ReadUtf8File(file);
    size_t start = 0;
    while (start < text.size())
    {
        size_t end = text.find(L'\n', start);
        if (end == std::wstring::npos) end = text.size();
        std::wstring line = text.substr(start, end - start);
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        if (!line.empty()) lines.push_back(std::move(line));
        start = end + 1;
    }
    return lines;
}

// One file per app and size, named by a hash of the app's id (ids can hold any character).
std::wstring IconFile(std::wstring const& id, int size)
{
    uint64_t hash = 14695981039346656037ull;  // FNV-1a
    for (wchar_t c : id) hash = (hash ^ uint64_t(c)) * 1099511628211ull;
    wchar_t name[32];
    swprintf_s(name, L"%016llx.icon", hash);
    std::wstring folder = CacheFolder() + L"\\icons" + std::to_wstring(size);
    CreateDirectoryW(folder.c_str(), nullptr);
    return folder + L"\\" + name;
}

constexpr uint32_t IconMagic = 0x494D534D;  // "MSMI": magic, width, height, then premultiplied BGRA
}

std::wstring CacheFolder()
{
    static std::wstring folder = [] {
        std::wstring path = LocalAppDataFolder() + L"\\MyStartMenu";
        CreateDirectoryW(path.c_str(), nullptr);
        return path;
    }();
    return folder;
}

std::wstring AppListFile() { return CacheFolder() + L"\\apps.txt"; }

uint64_t FileTimeNow()
{
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    return (uint64_t(now.dwHighDateTime) << 32) | now.dwLowDateTime;
}

int RunAppsHelper(std::wstring const& outFile)
{
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    std::wstring text;
    for (auto const& app : EnumerateApps()) text += app.id + L'\t' + app.name + L'\t' + std::to_wstring(app.added) + L'\n';
    std::string bytes = ToUtf8(text);
    return WriteFileAtomic(outFile, bytes.data(), bytes.size()) ? 0 : 1;
}

namespace
{
void CacheIcon(std::wstring const& id, int size)
{
    std::vector<uint8_t> pixels;
    int w = 0, h = 0;
    if (!LoadIconPixels(id, size, pixels, w, h)) return;
    std::vector<uint8_t> file(12 + pixels.size());
    uint32_t header[3] = { IconMagic, uint32_t(w), uint32_t(h) };
    memcpy(file.data(), header, sizeof header);
    memcpy(file.data() + 12, pixels.data(), pixels.size());
    WriteFileAtomic(IconFile(id, size), file.data(), file.size());
}
}

// One icon the shell is slow to give (a network share that is off, a phone unplugged) must not
// hold up the rest of the batch: each icon gets IconTimeout. One that runs over is left to its
// thread, which can't be stopped, and the rest go on in a fresh one; the process ends at the end
// whatever is still stuck. The menu does not ask for the skipped one again until the app list
// changes or it restarts (its g_iconAsked).
int RunIconsHelper(int size, std::wstring const& listFile)
{
    constexpr ULONGLONG IconTimeout = 5000;
    struct Progress { std::atomic<size_t> at; std::atomic<bool> done = false, abandoned = false; };
    auto ids = std::make_shared<std::vector<std::wstring>>(ReadLines(listFile));
    bool stuck = false;
    for (size_t from = 0; from < ids->size();)
    {
        auto progress = std::make_shared<Progress>();
        progress->at = from;
        std::thread([ids, size, progress] {
            CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            for (size_t i = progress->at; i < ids->size() && !progress->abandoned; i++)
            {
                progress->at = i;
                CacheIcon((*ids)[i], size);
            }
            progress->done = true;
        }).detach();

        size_t seen = from;
        ULONGLONG since = GetTickCount64();
        for (;;)
        {
            Sleep(20);
            if (progress->done) { from = ids->size(); break; }
            if (size_t at = progress->at; at != seen) { seen = at; since = GetTickCount64(); }
            else if (GetTickCount64() - since > IconTimeout)
            {
                Log(L"icon timed out, skipped: " + (*ids)[at]);
                progress->abandoned = true;
                stuck = true;
                from = at + 1;
                break;
            }
        }
    }
    DeleteFileW(listFile.c_str());
    if (stuck) TerminateProcess(GetCurrentProcess(), 0);  // a clean exit could wait on the stuck thread
    return 0;
}

namespace
{
// Windows keeps its Start pins to itself (start2.bin, an undocumented format). The one way out is
// the object behind the documented Export-StartLayout cmdlet, which writes them as JSON. The
// interface is not in the SDK; this is it as the cmdlet's own assembly declares it.
struct __declspec(uuid("0BAC4102-61E9-48A5-93DD-D295ABA65369")) IStartLayoutCmdlet : IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE ExportStartLayout(LPCWSTR path) = 0;
    virtual HRESULT STDMETHODCALLTYPE ExportStartLayoutWithDesktopApplicationIDs(LPCWSTR path) = 0;  // not implemented on Windows 11
    virtual HRESULT STDMETHODCALLTYPE ValidateLayoutFile(LPCWSTR path) = 0;
    virtual HRESULT STDMETHODCALLTYPE ExportEdgeAssets(LPCWSTR path) = 0;
};
constexpr CLSID CLSID_StartLayoutCmdlet{ 0x75AB852C, 0x0441, 0x46D4, { 0xA2, 0x05, 0xEF, 0x0A, 0x33, 0xF9, 0x82, 0x55 } };

std::wstring DisplayName(std::wstring const& id)
{
    ComPtr<IShellItem> item;
    PWSTR name{};
    if (FAILED(SHCreateItemFromParsingName(ShellName(id).c_str(), nullptr, IID_PPV_ARGS(&item))) ||
        FAILED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &name)))
        return {};
    std::wstring result = name;
    CoTaskMemFree(name);
    return result;
}

// A pin is a folder when what it opens is one: for a shortcut its target (a folder, or a shell
// location such as Home), otherwise the pinned item itself. File Explorer's shortcut targets a
// shell location that is not a folder, so it stays an app. Zip files are folders to the shell
// too, but they count as files.
bool IsFolder(std::wstring const& id)
{
    ComPtr<IShellItem> item;
    if (id.size() > 4 && _wcsicmp(id.c_str() + id.size() - 4, L".lnk") == 0)
    {
        ComPtr<IShellLinkW> link;
        ComPtr<IPersistFile> file;
        PIDLIST_ABSOLUTE target{};
        if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))) ||
            FAILED(link.As(&file)) || FAILED(file->Load(id.c_str(), STGM_READ)) || link->GetIDList(&target) != S_OK)
            return false;  // installer-advertised shortcuts (Unity's, for one) have no target: apps
        HRESULT hr = SHCreateItemFromIDList(target, IID_PPV_ARGS(&item));
        CoTaskMemFree(target);
        if (FAILED(hr)) return false;
    }
    else if (id.find(L'\\') == std::wstring::npos ||  // a packaged app's id
             FAILED(SHCreateItemFromParsingName(id.c_str(), nullptr, IID_PPV_ARGS(&item))))
        return false;
    SFGAOF attributes = 0;
    item->GetAttributes(SFGAO_FOLDER | SFGAO_STREAM, &attributes);
    return (attributes & SFGAO_FOLDER) && !(attributes & SFGAO_STREAM);
}
}

// The export lists packaged apps by their app id, which is also their id in shell:AppsFolder, and
// everything else by its shortcut in a Start menu Programs folder (pinned folders and files get
// one there too), which the shortcut's full path then stands for.
int RunPinsHelper(std::wstring const& outFile)
{
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ComPtr<IStartLayoutCmdlet> layout;
    if (HRESULT hr = CoCreateInstance(CLSID_StartLayoutCmdlet, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&layout)); FAILED(hr))
    {
        Log(L"start layout unavailable: " + std::to_wstring(hr));
        return 1;
    }
    std::wstring json = CacheFolder() + L"\\start-layout-" + std::to_wstring(GetCurrentProcessId()) + L".json";
    HRESULT hr = layout->ExportStartLayout(json.c_str());
    JsonValue root;
    bool parsed = SUCCEEDED(hr) && JsonParser(ReadUtf8File(json)).Parse(root);
    DeleteFileW(json.c_str());
    if (!parsed)
    {
        Log(L"start layout export failed: " + std::to_wstring(hr));
        return 1;
    }

    std::wstring text;
    if (auto pins = root.Find(L"pinnedList"); pins && pins->type == JsonValue::Type::Array)
        for (auto const& pin : pins->items)
        {
            std::wstring id(pin.Str(L"packagedAppId"));
            if (id.empty())
            {
                wchar_t path[MAX_PATH];
                std::wstring link(pin.Str(L"desktopAppLink"));
                if (link.empty() || !ExpandEnvironmentStringsW(link.c_str(), path, MAX_PATH)) continue;
                id = path;
            }
            std::wstring name = DisplayName(id);
            if (name.empty()) continue;  // a pin whose app is gone is skipped, as Windows does
            text += id + L'\t' + name + (IsFolder(id) ? L"\tfolder" : L"") + L'\n';
        }
    std::string bytes = ToUtf8(text);
    return WriteFileAtomic(outFile, bytes.data(), bytes.size()) ? 0 : 1;
}

// The shell's own folder picker, for a folder to pin; writes "path<tab>name" (the name as Explorer
// shows it, "Belgeler" for Documents) to outFile, or nothing if cancelled. Shell locations with no
// path (This PC, Network, Control Panel) are allowed too, by their parsing name ("::{GUID}").
int RunPickFolderHelper(std::wstring const& outFile)
{
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    DeleteFileW(outFile.c_str());
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return 1;
    FILEOPENDIALOGOPTIONS options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions((options | FOS_PICKFOLDERS | FOS_ALLNONSTORAGEITEMS) & ~FOS_FORCEFILESYSTEM);
    dialog->SetTitle(L"Sabitlenecek klasörü seçin");
    dialog->SetOkButtonLabel(L"Sabitle");
    ComPtr<IShellItem> item;
    if (dialog->Show(nullptr) != S_OK || FAILED(dialog->GetResult(&item))) return 1;
    PWSTR path = nullptr, name = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) &&
        FAILED(item->GetDisplayName(SIGDN_DESKTOPABSOLUTEPARSING, &path)))
        return 1;
    if (FAILED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &name))) name = nullptr;
    std::wstring folder = path, last = folder.substr(folder.find_last_of(L'\\') + 1);
    std::wstring text = folder + L'\t' + (name ? std::wstring(name) : last) + L"\tfolder\n";
    CoTaskMemFree(path);
    CoTaskMemFree(name);
    std::string bytes = ToUtf8(text);
    return WriteFileAtomic(outFile, bytes.data(), bytes.size()) ? 0 : 1;
}

bool StartHelper(std::wstring const& arguments, HWND notify, unsigned message)
{
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring command = L"\"" + std::wstring(exe) + L"\" " + arguments;
    STARTUPINFOW startup{ sizeof startup };
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(exe, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process))
    {
        Log(L"helper failed to start: " + std::to_wstring(GetLastError()));
        return false;
    }
    CloseHandle(process.hThread);
    std::thread([handle = process.hProcess, notify, message] {
        WaitForSingleObject(handle, INFINITE);
        CloseHandle(handle);
        PostMessageW(notify, message, 0, 0);
    }).detach();
    return true;
}

namespace
{
// "id<tab>name" lines, as the helper writes them; pins add "<tab>folder" for a folder, apps
// "<tab>" and when they were installed.
std::vector<AppRef> ReadRefs(std::wstring const& file)
{
    std::vector<AppRef> apps;
    for (auto const& line : ReadLines(file))
        if (size_t tab = line.find(L'\t'); tab != std::wstring::npos && tab > 0)
        {
            size_t end = line.find(L'\t', tab + 1);
            std::wstring extra = end == std::wstring::npos ? L"" : line.substr(end + 1);
            AppRef app{ line.substr(0, tab), line.substr(tab + 1, end == std::wstring::npos ? end : end - tab - 1) };
            app.folder = extra == L"folder";
            if (!app.folder) app.added = wcstoull(extra.c_str(), nullptr, 10);
            apps.push_back(std::move(app));
        }
    return apps;
}
}

std::vector<AppRef> ReadAppList() { return ReadRefs(AppListFile()); }

std::wstring PinListFile() { return CacheFolder() + L"\\pins.txt"; }
std::vector<AppRef> ReadPinList() { return ReadRefs(PinListFile()); }

std::wstring PickedFolderFile() { return CacheFolder() + L"\\picked-folder.txt"; }
std::vector<AppRef> ReadPickedFolder() { return ReadRefs(PickedFolderFile()); }

void WriteIdList(std::wstring const& file, std::vector<std::wstring> const& ids)
{
    std::wstring text;
    for (auto const& id : ids) text += id + L'\n';
    std::string bytes = ToUtf8(text);
    WriteFileAtomic(file, bytes.data(), bytes.size());
}

bool ReadCachedIcon(std::wstring const& id, int size, std::vector<uint8_t>& pixels, int& width, int& height, uint64_t& written)
{
    std::wstring path = IconFile(id, size);
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info)) return false;
    written = (uint64_t(info.ftLastWriteTime.dwHighDateTime) << 32) | info.ftLastWriteTime.dwLowDateTime;
    std::ifstream file(path, std::ios::binary);
    uint32_t header[3]{};
    if (!file.read(reinterpret_cast<char*>(header), sizeof header) || header[0] != IconMagic || !header[1] || !header[2] ||
        header[1] > 1024 || header[2] > 1024)
        return false;
    width = int(header[1]);
    height = int(header[2]);
    pixels.resize(size_t(width) * height * 4);
    return bool(file.read(reinterpret_cast<char*>(pixels.data()), std::streamsize(pixels.size())));
}

void WatchAppChanges(HWND notify, unsigned message)
{
    std::thread([notify, message] {
        // Kernel-level notifications only: no shell machinery comes into the process for this.
        std::vector<HANDLE> waits;
        for (auto folder : { &FOLDERID_Programs, &FOLDERID_CommonPrograms })
        {
            PWSTR path{};
            if (SUCCEEDED(SHGetKnownFolderPath(*folder, 0, nullptr, &path)))
            {
                HANDLE change = FindFirstChangeNotificationW(path, TRUE,
                    FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE);
                if (change != INVALID_HANDLE_VALUE) waits.push_back(change);
            }
            CoTaskMemFree(path);
        }
        // Packaged (Store) apps: their per-user registrations live under this key.
        HKEY packages{};
        HANDLE packagesChanged = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        auto armPackages = [&] {
            return packages && RegNotifyChangeKeyValue(packages, TRUE, REG_NOTIFY_CHANGE_NAME | REG_NOTIFY_THREAD_AGNOSTIC,
                                                       packagesChanged, TRUE) == ERROR_SUCCESS;
        };
        if (RegOpenKeyExW(HKEY_CURRENT_USER,
                          L"Software\\Classes\\Local Settings\\Software\\Microsoft\\Windows\\CurrentVersion\\AppModel\\Repository\\Packages",
                          0, KEY_NOTIFY, &packages) == ERROR_SUCCESS && armPackages())
            waits.push_back(packagesChanged);
        if (waits.empty()) return;

        for (;;)
        {
            DWORD signaled = WaitForMultipleObjects(DWORD(waits.size()), waits.data(), FALSE, INFINITE);
            if (signaled >= WAIT_OBJECT_0 + waits.size()) return;
            HANDLE handle = waits[signaled - WAIT_OBJECT_0];
            if (handle == packagesChanged) armPackages();
            else FindNextChangeNotification(handle);
            PostMessageW(notify, message, 0, 0);
        }
    }).detach();
}
