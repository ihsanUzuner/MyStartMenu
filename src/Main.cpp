// Başlat Menüm: a personal start menu that opens instead of Windows' own.
//
// Everything is drawn by us: Direct2D for shapes and images, DirectWrite for text, on the
// Windows.UI.Composition visual layer (what XAML itself draws on, without loading XAML).
// The menu is a tree of composition visuals: the blurred backdrop, the tinted background, the
// hover highlights and the content (text, icons). Hover fades and the open/close slide are
// composition animations, so the system compositor plays them at the display's refresh rate
// while this process draws nothing. (Drawing the whole menu per frame on the CPU took ~16 ms,
// which dropped frames at 60 Hz.)
//
// Usage: MyStartMenu.exe                shows the menu (or asks the running copy to show it)
//        MyStartMenu.exe --background   starts hidden and waits for the Win key (used at sign-in)
//        MyStartMenu.exe --render DIR   draws home, hover and search views offscreen into DIR\*.png

#include <unknwn.h>
#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <d3d11.h>
#include <d2d1_1.h>
#include <d2d1_1helper.h>
#include <d2d1effects.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <DispatcherQueue.h>
#include <windows.ui.composition.interop.h>
#include <windows.graphics.effects.interop.h>
#undef GetCurrentTime // clashes with a composition method

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.Numerics.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.Effects.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.UI.Composition.Desktop.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <tuple>
#include <vector>

#include "Input.h"
#include "Shell.h"
#include "Tray.h"

using Microsoft::WRL::ComPtr;
namespace wuc = winrt::Windows::UI::Composition;
using namespace std::chrono_literals;

namespace
{
    // ---- Layout, in DIPs ----
    //
    // Measured off a screenshot of the Windows 11 Start menu at 100% (14.10.2026), so the two match:
    // its size and place, the bands, the search box, the grid and the bottom bar.

    constexpr float ScreenGap = 12;
    // No top band (user's choice, 16.10.2026): the search box is in the bottom bar and the content
    // starts at the menu's top. The bar's separator line is on its inner edge.
    constexpr float BarH = 65;
    constexpr float ContentTop = 0;
    constexpr float TitleY = ContentTop + 29;
    constexpr float GridX = 33, GridY = TitleY + 32;
    // Windows' cell is 96 wide and 84 high at its 32 DIP icon. Ours are tiles of a size set (config
    // `tileSize`, see "Layout"); the icon's size changes only the icon, never the tile.
    constexpr float CellW = 96;                         // Windows' cell, which the widest folder list is measured against
    constexpr float RowH = 40, ListY = ContentTop + 20; // search results start at the scrolling view's top (SplitTop)
    // The home view is two columns: the pinned apps' grid on the left, the pinned folders' list on the
    // right, a line between them (FolderSplit). Each scrolls on its own. While the settings panel is
    // open the line can be dragged right, narrowing the folder list (config `folderWidth`): its names
    // shorten, then go, leaving the icons. The grid takes as many whole tiles as fit left of the line
    // and is centred in that room (Columns, GridLeft).
    constexpr float SplitGap = 32; // from the grid's right edge to the line
    // The widest folder list: what Windows' 834 wide menu leaves beside a grid of 6 columns.
    constexpr float FolderMaxW = (834 - 20) - (GridX + 6 * CellW + SplitGap + 12);

    // The menu's size is a setting: four widths and four heights in proportion to the screen (config
    // `menuWidthStep`, `menuHeightSize`; MenuWidthOf, MenuHeightOf), Windows' own 834 x 866 the
    // standard; no larger than the screen has room for. The grid takes as many columns as fit beside
    // the folder column, and the rows scroll. Set by LayoutMenu.
    float MenuW = 834, MenuH = 866;
    float ContentBottom = MenuH - BarH;
    // The line between the columns runs down the content area, short of its ends. What scrolls (both
    // columns, the search results, the every-apps list) shows only along it, the user's wish: it
    // touched the bands' lines before.
    constexpr float SplitTop = ContentTop + 20;
    float SplitBottom = ContentBottom - 20;
    constexpr float WindowsMenuW = 834;
    float g_barShift = 0; // DIPs from the menu's left to where Windows' own menu would start (BarSpanLeft)
    float FolderRight = MenuW - 20;
    // The search box is in the bottom bar, between the account picture and the buttons on the right,
    // always centred over the apps side (the user's wish), following the column line. Set by
    // PlaceSearchBox.
    D2D1_RECT_F SearchBox{33, 0, 513, 32};
    constexpr float SearchBoxH = 32, SearchBoxMaxW = 480, SearchBarGap = 12; // SearchBarGap: to the picture and the buttons
    float SearchTextLeft = SearchBox.left + 44, SearchTextRight = SearchBox.right - 14;
    constexpr float SplitHandleW = 32, SplitHandleH = 22, SplitGrab = 6; // SplitGrab: either side of the line
    constexpr float BarButton = 40, BarSmallInset = 4, BarGap = 6, BarLeft = 57, BarRight = 53;
    constexpr int BarCount = 3, PowerButton = 2;
    // Room around the menu and the flyout for their shadows. The shadows are drawn in our visual tree,
    // not by the window manager, so they slide and fade with the content; the window's own shadow
    // stayed put while the content slid away and vanished only when the window hid.
    constexpr float ShadowMargin = 24;
    constexpr float Avatar = 40, AccountH = Avatar + 8; // Windows' is 32; larger, the user's wish

    // ---- Motion; the same timings as the WPF menu ----

    constexpr float SlideDistance = 90;
    constexpr auto OpenTime = 300ms, FadeInTime = 110ms, CloseTime = 130ms, HoverTime = 150ms;

    enum : UINT
    {
        WM_APPS_READY = WM_APP + 1, // the helper wrote a fresh app list
        WM_TOGGLE,                  // Win key or Start button
        WM_SHOW_MENU,               // a second copy of the app was started
        WM_TRAY,                    // notification-area icon events
        WM_APPS_CHANGED,            // apps may have been installed, removed or updated
        WM_ICONS_READY,             // the helper cached a batch of icons
        WM_PINS_READY,              // the helper read Windows' Start pins (once, for the menu's first list)
        WM_FOLDER_PICKED,           // the folder picker closed, a folder chosen or not
        WM_START_MOVED,             // the taskbar's Start button moved while the menu is open
        WM_PRESSED,                 // a mouse button went down anywhere while the menu is open
    };

    enum : UINT_PTR
    {
        TimerAppsChanged = 2,
        TimerDragScroll,
        TimerTakeForeground,
        TimerHideScrollbar
    }; // the last, one per scrollbar (+ bar)

    // Hit-test keys. Ranges let one number say both what was hit and which one.
    enum : int
    {
        KeyNone = -1,
        KeySearch = 1,
        KeyPinned = 1000,
        KeyFolder = 1500,
        KeyFolderAdd = 1999,
        KeyBar = 2000,
        KeySectionGrip = 2500,
        KeySearchBack = 2980,
        KeyScrollbar = 2990,
        KeyFolderScrollbar = 2991,
        KeySort = 2993,
        KeyAllApps = 2996,
        KeySplitter = 2997,
        KeyAccount = 2999,
        KeyResult = 3000
    };

    struct BarAction
    {
        const wchar_t *glyph;
        const wchar_t *target;
    };
    // Folder buttons (Downloads, the user's folder, File Explorer) were dropped: the folders column
    // does that, with the folders' own icons (user's choice). Settings and Network show as Windows' Start
    // settings say (ReadStartPlaces, on every open); the power button always.
    const BarAction BarActions[BarCount] = {
        {L"\xEC27", L"shell:::{F02C1A0D-BE21-4350-88B0-7367FC96EF3C}"}, // Network
        {L"\xE713", L"ms-settings:"},
        {L"\xE7E8", nullptr}, // PowerButton: opens the power menu
    };
    bool g_barShown[BarCount] = {false, true, true};

    void ReadBarShown()
    {
        StartPlaces places = ReadStartPlaces();
        g_barShown[0] = places.network;
        g_barShown[1] = places.settings;
    }

    struct Tile
    {
        std::wstring id, name;
        ComPtr<ID2D1Bitmap1> icon;
        int iconPixels = 0; // the size the icon was loaded at (or tried to be); 0 = not yet
        int iconOf = 0;     // the size `icon` itself was asked at; another than wanted while a new size loads
        uint64_t added = 0; // app list only: when installed (FILETIME ticks), for "newest first"
        int group = 0;      // pinned apps only: the group it is in (see "Groups"); 0 for none
    };

    // ---- State ----

    HWND g_hwnd{};
    UINT g_dpi = 96;
    bool g_dirty = true;

    Config g_config;
    uint64_t g_configStamp = 0;
    // The menu's own pins (config `pins`, not synced with Windows'), in their order: the apps
    // (g_pinned) and the folders (g_folders) apart.
    std::vector<Tile> g_pinned, g_folders, g_apps;
    std::vector<int> g_results; // indices into g_apps
    ULONGLONG g_appsFetchedAt = 0;
    bool g_fetchingApps = false, g_fetchAgain = false;
    int g_appsGeneration = 0;     // bumps when apps may have changed, so icons get checked again
    uint64_t g_appsChangedAt = 0; // FILETIME of the last such change; icons cached before it are refreshed

    // Icons the helper is asked for, as "size|id" -> the apps generation they were asked in, so each
    // is asked for once per change. The queue waits for the helper; the batch is what it is doing.
    std::unordered_map<std::wstring, int> g_iconAsked;
    std::vector<std::pair<int, std::wstring>> g_iconQueue, g_iconBatch;

    // A line of text being edited: the search box, and a section's name while it is renamed. One
    // editor serves both (caret, selection, the keys, the clipboard, the drawing); each field says
    // where its text shows and what follows an edit.
    struct TextField
    {
        std::wstring text;
        size_t caret = 0, anchor = 0;
        float scroll = 0;            // how far the text is scrolled left in its area
        float caretX = 0;            // the caret's x in the text, as last drawn
        D2D1_RECT_F area{};          // where the text shows; set by place()
        void (*place)() = nullptr;   // puts `area` where the field is now
        void (*changed)() = nullptr; // after an edit
        IDWriteTextFormat *(*format)() = nullptr;
    };
    TextField g_search, g_nameField;
    std::wstring &g_query = g_search.text;
    int g_renaming = 0;       // the section whose name is being edited; 0: none, and typing searches
    bool g_selecting = false; // dragging out a selection in the focused field with the mouse

    TextField &Focused() { return g_renaming ? g_nameField : g_search; }

    float g_scroll = 0;           // the apps grid, or the search results
    float g_folderScroll = 0;     // the folder list, which scrolls on its own
    bool g_splitDragging = false; // the line between the columns is being dragged
    float g_splitGrab = 0;        // where on it: the pointer's x less the line's
    // Reordering the pinned apps by dragging one (see "Reordering pins").
    int g_dragPin = -1;                       // the pin pressed, which moving the pointer would drag (index in g_pinned, or g_folders)
    bool g_dragFolders = false;               // it is a folder, in the folder list, rather than an app in the grid
    bool g_pinDragging = false;               // it is being dragged
    int g_dragSlot = 0;                       // where it would drop now
    bool g_settling = false;                  // let go: the tiles are still sliding into place
    D2D1_POINT_2F g_dragFrom{}, g_dragGrab{}; // where it was pressed; the pointer less the tile's top left
    bool g_scrollDragging = false;            // a scrollbar's thumb is being dragged
    int g_scrollDragBar = 0;                  // which: 0 the apps side's, 1 the folder list's
    float g_scrollGrab = 0;                   // where on it: the pointer's y less the thumb's top
    extern bool g_setOpen;                    // the settings panel is open: the line can be dragged
    extern uint32_t g_accent;
    // The list of every app, each marked pinned or not, a click pinning or unpinning it: for pinning
    // many at once. Opened by the "All apps ›" button at the apps title's end (KeyAllApps), always there.
    bool g_allApps = false;
    int g_selected = 0;
    int g_hot = KeyNone, g_pressed = KeyNone;
    int g_keyFocus = KeyNone; // the pinned app or folder picked with the arrows (see "Keyboard on the home view")
    bool g_tracking = false;
    D2D1_POINT_2F g_mouse{-1, -1};
    std::unordered_map<int, float> g_highlights; // target opacity of each hover highlight

    bool g_visible = false, g_closing = false;
    bool g_updatesPending = false; // read on every open; puts the orange dot on the power button
    ULONGLONG g_deactivatedAt = 0; // the tray click that deactivated the menu must not reopen it

    // ---- Graphics ----

    ComPtr<ID2D1Factory1> g_d2d;
    ComPtr<IDWriteFactory> g_dwrite;
    ComPtr<ID3D11Device> g_d3d;
    ComPtr<ID2D1Device> g_d2dDevice;
    ComPtr<ID2D1DeviceContext> g_resources; // creates brushes and bitmaps; usable by every context of the device
    ComPtr<ID2D1DeviceContext> g_dc;        // the context being drawn into right now
    ComPtr<ID2D1SolidColorBrush> g_brush;
    ComPtr<IDWriteTextFormat> g_fmtTitle, g_fmtCell, g_fmtRow, g_fmtFolder, g_fmtSearch, g_fmtName, g_fmtGlyph, g_fmtBarGlyph;
    ComPtr<IDWriteInlineObject> g_ellipsis, g_folderEllipsis;
    ComPtr<ID2D1Bitmap1> g_accountPicture;
    UINT g_accountPictureDpi = 0; // the DPI the picture was picked for; 0 means not loaded yet

    // ---- Composition ----

    winrt::Windows::System::DispatcherQueueController g_queue{nullptr};
    wuc::Compositor g_compositor{nullptr};
    wuc::CompositionGraphicsDevice g_graphics{nullptr};

    // Every window of the app (the menu, its flyout, and the ones to come) is the same kind of thing:
    // a borderless popup whose visible part is a rounded body with a soft shadow, all drawn in the
    // visual tree. The window reaches ShadowMargin past the body on every side to hold the shadow.
    // See "Popup windows" below for the functions that build, place, open and close one.
    struct PopupWindow
    {
        HWND hwnd{};
        wuc::Desktop::DesktopWindowTarget target{nullptr};
        wuc::ContainerVisual root{nullptr}; // the whole window; opening and closing move and fade this
        wuc::SpriteVisual shadow{nullptr};
        wuc::ContainerVisual body{nullptr};  // clipped to the rounded shape; the window's own layers go here
        wuc::SpriteVisual backdrop{nullptr}; // the acrylic (the blurred desktop, toned), under the body (PlacePopup)
        wuc::CompositionRoundedRectangleGeometry shape{nullptr};
        wuc::CompositionRoundedRectangleGeometry backShape{nullptr}; // shape, where the body is in the backdrop
        wuc::CompositionScopedBatch closing{nullptr};
        float width = 0, height = 0; // the body, in DIPs
        POINT inset{};               // pixels: the body this far in from its usual place, in a window larger than it (PlacePopup)
        double acrylicFor = -1;      // the transparency setting the acrylic was built for
        uint32_t tintFor = 0;        // and the tint (g_tint)
        bool opaque = false;         // a plain dark fill under the body instead of the acrylic (UpdateAcrylic)
        UINT shadowDpi = 0;          // what the shadow was last cast for, to skip recasting it
        int generation = 0;          // bumps on every open and close, so a stale close cannot hide a reopened window
    };

    PopupWindow g_main; // the menu
    wuc::ContainerVisual g_contentHovers{nullptr}, g_barHovers{nullptr};
    wuc::SpriteVisual g_backVisual{nullptr}, g_frontVisual{nullptr}, g_caretVisual{nullptr};
    // The search box's magnifier and back arrow, each a visual of its own in a host at the box's place,
    // so the compositor plays the change between them (SearchIcons).
    wuc::ContainerVisual g_searchIcons{nullptr};
    wuc::SpriteVisual g_magnifierVisual{nullptr}, g_backArrowVisual{nullptr};
    int g_searchIconsBack = -1; // the arrow shown (1) or the magnifier (0); -1: not placed yet
    // The apps side (the pinned grid, the every-apps list, or the search results) has a layer of its
    // own, so that it can scroll smoothly. It is drawn once per scroll step, a little past the visible
    // area (ScrollOverscan above and below), and the compositor glides it from where it was to where it
    // now is; its hover highlights (g_appsHovers) glide with it. Nothing is drawn per frame.
    constexpr float ScrollOverscan = 180; // three wheel steps
    constexpr auto GlideTime = 220ms;
    wuc::LayerVisual g_appsLayer{nullptr}; // a layer, so its ends can fade (see "Edge fades")
    wuc::ContainerVisual g_appsHovers{nullptr};
    // The folder list (its title and rows, their highlights, and the rows while one is dragged) is a
    // layer of its own too, to fade the same way; the column's line and "+" row stay on the front layer.
    wuc::LayerVisual g_folderLayer{nullptr};
    wuc::ContainerVisual g_folderHovers{nullptr};
    wuc::SpriteVisual g_folderSprite{nullptr};
    wuc::CompositionDrawingSurface g_folderSurface{nullptr};
    SIZE g_folderSurfacePx{};
    wuc::ContainerVisual g_dragTiles{nullptr}; // the pins' tiles while one is dragged; offset by -scroll
    // The folders' rows while one is dragged, offset by -folder scroll, in a host clipped to the content area.
    wuc::ContainerVisual g_folderTilesHost{nullptr}, g_folderTiles{nullptr};
    wuc::SpriteVisual g_appsSprite{nullptr};
    wuc::CompositionDrawingSurface g_appsSurface{nullptr};
    wuc::InsetClip g_appsClip{nullptr};
    SIZE g_appsSurfacePx{};
    float g_drawnScroll = 0; // the scroll the apps layer was last drawn at
    // The apps side's scrollbar thumb, a layer of its own so it glides with the content.
    // The folder list has one too. By index: 0 the apps side's, 1 the folder list's.
    struct ThumbLayer
    {
        wuc::SpriteVisual visual{nullptr};
        wuc::CompositionDrawingSurface surface{nullptr};
        float drawnH = -1; // the height and look it was last drawn in
        bool drawnHot = false;
        winrt::Windows::Foundation::Numerics::float3 at{}; // where it was last put (or is gliding to)
        float scroll = 0;                                  // the scroll it was last put for: a change shows it
        bool shown = false;                                // showing, or fading in (see ShowScrollbar)
    };
    ThumbLayer g_thumbs[2];
    // A glide: from what scroll to what, and since when, so a new step can start from where it is now.
    struct Glide
    {
        float from = 0, to = 0;
        ULONGLONG start = 0;
        bool pending = false;
    } g_glide;
    // What gets drawn of the scrolling content: the visible area, or into the apps layer, more.
    float g_drawTop = ContentTop, g_drawBottom = ContentBottom;
    wuc::CompositionDrawingSurface g_backSurface{nullptr}, g_frontSurface{nullptr};
    std::unordered_map<int, wuc::SpriteVisual> g_hoverLayers;
    std::unordered_map<uint64_t, wuc::CompositionSurfaceBrush> g_shapes; // highlight shapes by size
    UINT g_layersDpi = 0;
    float g_layersW = 0, g_layersH = 0; // the menu size they were made for
    bool g_backDirty = true;

    void Invalidate() { g_dirty = true; }
    float Px(float dips) { return std::round(dips * g_dpi / 96); }

    // A popup's body sits ShadowMargin in from its window's edges (and its inset further), and layout
    // works in body DIPs. Window client pixels to body DIPs.
    D2D1_POINT_2F ClientToBody(PopupWindow const &popup, int x, int y)
    {
        float toDips = 96.f / g_dpi;
        return {(x - popup.inset.x) * toDips - ShadowMargin, (y - popup.inset.y) * toDips - ShadowMargin};
    }

    POINT BodyToScreen(PopupWindow const &popup, float x, float y)
    {
        POINT p{LONG(Px(x + ShadowMargin)) + popup.inset.x, LONG(Px(y + ShadowMargin)) + popup.inset.y};
        ClientToScreen(popup.hwnd, &p);
        return p;
    }

    bool InBody(PopupWindow const &popup, D2D1_POINT_2F p)
    {
        return p.x >= 0 && p.y >= 0 && p.x < popup.width && p.y < popup.height;
    }

    // ---- Drawing helpers ----

    D2D1_COLOR_F Argb(uint32_t c, float opacity = 1)
    {
        return D2D1::ColorF(((c >> 16) & 255) / 255.f, ((c >> 8) & 255) / 255.f, (c & 255) / 255.f,
                            ((c >> 24) & 255) / 255.f * opacity);
    }

    ID2D1SolidColorBrush *Brush(uint32_t argb, float opacity = 1)
    {
        g_brush->SetColor(Argb(argb, opacity));
        return g_brush.Get();
    }

    void Text(std::wstring_view text, IDWriteTextFormat *format, D2D1_RECT_F rect, uint32_t color)
    {
        g_dc->DrawText(text.data(), UINT32(text.size()), format, &rect, Brush(color), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    // Queues an icon for the helper, once per apps generation.
    void RequestIcon(std::wstring const &id, int size)
    {
        std::wstring key = std::to_wstring(size) + L"|" + id;
        if (auto it = g_iconAsked.find(key); it != g_iconAsked.end() && it->second == g_appsGeneration)
            return;
        g_iconAsked[key] = g_appsGeneration;
        g_iconQueue.push_back({size, id});
    }

    bool g_recycleBinFull = false; // asked on every open (UpdateRecycleBin)

    bool IsRecycleBin(std::wstring const &id) { return _wcsicmp(id.c_str(), RecycleBinId) == 0; }

    // What a tile's icon is cached under: its id, but the Recycle Bin has one icon full and one empty.
    std::wstring IconId(Tile const &tile) { return IsRecycleBin(tile.id) ? RecycleBinIcon(g_recycleBinFull) : tile.id; }

    // From the helper's disk cache; an icon not there yet (or older than the last app change, or than
    // a week) is queued for the helper and picked up when it is done. --render, which has no helper,
    // asks the shell itself.
    ID2D1Bitmap1 *IconOf(Tile &tile, int size)
    {
        if (tile.iconPixels != size)
        {
            // An icon of another size stays until this one is there (it is drawn scaled meanwhile).
            tile.iconPixels = size;
            std::vector<uint8_t> pixels;
            int w = 0, h = 0;
            bool loaded;
            std::wstring id = IconId(tile);
            if (!g_hwnd)
                loaded = LoadIconPixels(id, size, pixels, w, h);
            else
            {
                uint64_t written = 0;
                constexpr uint64_t week = 7ull * 24 * 3600 * 10000000;
                loaded = ReadCachedIcon(id, size, pixels, w, h, written);
                if (!loaded || written < g_appsChangedAt || written + week < FileTimeNow())
                    RequestIcon(id, size);
            }
            if (loaded)
            {
                auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,
                                                     D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
                tile.icon.Reset();
                g_resources->CreateBitmap(D2D1::SizeU(w, h), pixels.data(), w * 4, props, &tile.icon);
                tile.iconOf = size;
            }
        }
        return tile.icon.Get();
    }

    // Starts the helper on the queued icons of one size, unless it is already busy.
    void PumpIcons()
    {
        if (!g_iconBatch.empty() || g_iconQueue.empty() || !g_hwnd)
            return;
        // Sizes no tile wants any more (dragging the icon size slider passes many) are dropped, and
        // may be asked for again should the slider come back to them.
        std::unordered_set<std::wstring> wanted;
        for (auto *tiles : {&g_pinned, &g_folders, &g_apps})
            for (auto const &tile : *tiles)
                wanted.insert(std::to_wstring(tile.iconPixels) + L"|" + IconId(tile));
        std::erase_if(g_iconQueue, [&](auto const &entry)
                      {
        std::wstring key = std::to_wstring(entry.first) + L"|" + entry.second;
        if (wanted.contains(key)) return false;
        g_iconAsked.erase(key);
        return true; });
        if (g_iconQueue.empty())
            return;
        int size = g_iconQueue.front().first;
        std::vector<std::wstring> ids;
        for (auto it = g_iconQueue.begin(); it != g_iconQueue.end();)
            if (it->first == size)
            {
                ids.push_back(it->second);
                g_iconBatch.push_back(std::move(*it));
                it = g_iconQueue.erase(it);
            }
            else
                ++it;
        std::wstring list = CacheFolder() + L"\\icons-wanted-" + std::to_wstring(size) + L".txt";
        WriteIdList(list, ids);
        if (!StartHelper(L"--icons " + std::to_wstring(size) + L" \"" + list + L"\"", g_hwnd, WM_ICONS_READY))
            g_iconBatch.clear();
    }

    void Invalidate();

    // The helper finished a batch: tiles waiting for those icons read them from disk on their next draw.
    void IconsReady()
    {
        std::unordered_map<std::wstring, int> done;
        for (auto const &[size, id] : g_iconBatch)
            done[id] = size;
        g_iconBatch.clear();
        for (auto *tiles : {&g_pinned, &g_folders, &g_apps})
            for (auto &tile : *tiles)
                if (auto it = done.find(IconId(tile)); it != done.end() && tile.iconPixels == it->second)
                    tile.iconPixels = 0;
        Invalidate();
        PumpIcons();
    }

    // The Recycle Bin filled or emptied since it was last asked: its tiles load the other icon.
    void UpdateRecycleBin()
    {
        bool full = RecycleBinFull();
        if (full == g_recycleBinFull)
            return;
        g_recycleBinFull = full;
        for (auto *tiles : {&g_pinned, &g_folders, &g_apps})
            for (auto &tile : *tiles)
                if (IsRecycleBin(tile.id))
                    tile.iconPixels = 0;
        Invalidate();
    }

    // Asks for the icon at exactly the size it covers on screen and draws it pixel for pixel, as
    // Windows does: icons have their own drawing for each size, crisper than one scaled down.
    void Icon(Tile &tile, D2D1_RECT_F rect)
    {
        float scale = g_dpi / 96.f;
        int size = int(std::round((rect.right - rect.left) * scale));
        auto icon = IconOf(tile, size);
        if (!icon)
            return;
        // On whole device pixels, so nothing gets resampled.
        float left = std::round(rect.left * scale) / scale, top = std::round(rect.top * scale) / scale;
        auto bitmap = icon->GetPixelSize();
        if (tile.iconOf != size)
        {
            // Another size's icon, until this size's arrives: scaled to match.
            float fit = float(size) / tile.iconOf / scale;
            float w = bitmap.width * fit, h = bitmap.height * fit;
            float x = (rect.left + rect.right - w) / 2, y = (rect.top + rect.bottom - h) / 2;
            D2D1_RECT_F scaled{x, y, x + w, y + h};
            g_dc->DrawBitmap(icon, &scaled, 1.f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
            return;
        }
        D2D1_RECT_F snapped{left, top, left + bitmap.width / scale, top + bitmap.height / scale};
        g_dc->DrawBitmap(icon, &snapped, 1.f, D2D1_INTERPOLATION_MODE_LINEAR);
    }

    // ---- Layout ----

    // The home view: each column is a title over its pins (see FolderSplit). One with no pins shows a
    // line of text instead.
    // The folder icon size is a setting (FolderIconMin..FolderIconMax); rows grow with it past 40 high.
    float FolderIcon() { return float(g_config.folderIconSize); }
    float FolderRowH() { return std::max(RowH, FolderIcon() + 20); }
    float FolderMinW() { return 12 + FolderIcon() + 12; } // the icons alone
    float FolderNamesMinW() { return FolderMinW() + 52; } // narrower than this, the names go

    // The pinned apps' grid (the user's choices, 16.10.2026): each app is a tile of a size set (config
    // `tileSize`), its icon in the middle, its name under the icon inside it; the highlight under the
    // pointer is the tile. The icon's size (`iconSize`) changes only the icon, never the tiles or how
    // many fit. Between the tiles a gap (`tileGap`), the same across and down, so tiles never touch.
    // As many columns as fit beside the folder column, which keeps its set width (`folderWidth`; the
    // user's wish: it took what the columns left over, and changed size); the grid is centred in its
    // room, what is left over, short of a column, either side of it.
    float CellWidth() { return float(g_config.tileSize); } // a tile's width
    float TileGap() { return float(g_config.tileGap); }
    constexpr float NameLineH = 16; // a name's line
    constexpr float NameRoom = 28;  // with names, a tile is this much taller: its first line, and some
    constexpr float IconPadMin = 8; // an icon keeps at least this far in from its tile's edges
    float CellH() { return CellWidth() + (g_config.appNames ? NameRoom : 0); }
    float CellIcon() { return std::min(float(g_config.iconSize), CellWidth() - 2 * IconPadMin); }
    // The icon's top in its tile: in the middle, or with a name, the icon and the name's first line
    // together in the middle.
    float CellIconTop() { return std::round((CellH() - CellIcon() - (g_config.appNames ? 4 + NameLineH : 0)) / 2); }
    float CellLabelTop() { return CellIconTop() + CellIcon() + 4; }

    // The sections' tiles may be smaller than the rest (config `sectionScale`, the user's wish,
    // 18.10.2026: more of them to a section's row): the tile, its icon and the gap between tiles all by
    // the same share, and no names (at half there is no room for one). The apps in no section keep the
    // size set. A tile's measures, in its section or out of one (the Cell* above are the latter's).
    constexpr float SectionScales[] = {1.f, 3 / 4.f, 2 / 3.f, 1 / 2.f};
    float SectionScale() { return SectionScales[std::clamp(g_config.sectionScale, 0, int(std::size(SectionScales)) - 1)]; }
    struct TileDims
    {
        float w, h, gap, icon, iconTop, labelTop;
        bool names;
    };
    TileDims Dims(bool grouped)
    {
        if (!grouped || SectionScale() == 1)
            return {CellWidth(), CellH(), TileGap(), CellIcon(), CellIconTop(), CellLabelTop(), g_config.appNames};
        float s = SectionScale();
        TileDims d{};
        d.w = std::round(CellWidth() * s);
        d.h = d.w;
        d.gap = std::round(TileGap() * s);
        d.icon = std::min(std::round(g_config.iconSize * s), d.w - 2 * std::round(IconPadMin * s));
        d.iconTop = std::round((d.h - d.icon) / 2);
        d.labelTop = d.iconTop + d.icon + 4;
        d.names = false;
        return d;
    }

    float FolderWidth() { return std::clamp(float(g_config.folderWidth), FolderMinW(), FolderMaxW); }
    float FolderLeft() { return FolderRight - FolderWidth(); }
    // With the folder column off (config `folders`, the user's wish: some want the apps alone), the line
    // is as far in from the menu's right edge as the grid is from its left: the apps side is the whole
    // menu, the grid in its middle, and the apps side's scrollbar where the folders' was.
    float FolderSplit() { return g_config.folders ? FolderLeft() - 12 : MenuW - GridX + SplitGap; }
    bool FolderNames() { return FolderWidth() >= FolderNamesMinW(); }

    float GridRoom() { return FolderSplit() - SplitGap - GridX; }
    int Columns() { return std::max(1, int((GridRoom() + TileGap()) / (CellWidth() + TileGap()))); }
    float GridWidth() { return Columns() * CellWidth() + (Columns() - 1) * TileGap(); }
    float GridLeft() { return GridX + std::floor((GridRoom() - GridWidth()) / 2); }

    // ---- Sections ----
    //
    // The pinned apps' grid is in sections, one under another: first the apps in no section, then
    // each section under its name ("Yeni bölüm" as it starts; see "Renaming a section"). In each, the apps
    // run left to right in order, row after row, with no gaps. An app is dragged between others, into
    // another section, or onto "+ Yeni bölüm" (shown at the bottom while dragging) to start one; a
    // section left empty goes. All the user's choices (15.10.2026), after a free grid of framed,
    // sizable groups turned out shapeless. In the config a section is a pin group (`group`, `pinGroups`).
    // On screen a section is a "grup" (the user's choice, 17.10.2026; it was "bölüm"), as Windows 10's
    // tile groups were; in the code and the config it stays a section / pin group.
    constexpr wchar_t NewSectionName[] = L"Yeni grup";
    constexpr int NewSection = -1; // the section a dragged app would start

    // A section's name as set; "Yeni bölüm" for one with none. Every section has a name (the user's
    // choice: untitled ones were tried, and changed the section's size as the name went).
    std::wstring SectionName(int section)
    {
        for (auto const &g : g_config.pinGroups)
            if (g.id == section && !g.name.empty())
                return g.name;
        return NewSectionName;
    }

    // A section is shown (while dragging, or always with `sectionStyle` 1) as a faint box round its
    // name and its tiles, SectionPad out from the tiles, so no tile reaches past it. Sections are parted
    // by SectionGap beyond the tiles' own gap, which leaves the boxes the gap and 8 apart. A section's
    // name is a row over its tiles.
    constexpr float SectionNameH = 20;
    // The name row; none with the names off (config `sectionNames`, the user's wish: they caught the
    // eye). Off, every section loses it alike, so they stay the same as one another.
    float SectionNameRowH() { return g_config.sectionNames ? SectionNameH + 12 : 0; }
    constexpr float SectionPad = 8, SectionGap = 2 * SectionPad + 8;

    // What is laid out: pins (indices into g_pinned) in order, each with its section (0: none).
    struct GridEntry
    {
        int pin, section;
    };

    // Where each pin's tile goes (by its index in g_pinned), each section's name row and its box (the
    // apps in no section too, with no name row), and, while dragging, "+ Yeni bölüm"'s box. Not scrolled.
    struct GridLayout
    {
        std::vector<D2D1_RECT_F> cells;
        std::vector<std::pair<int, D2D1_RECT_F>> headers, regions;
        D2D1_RECT_F newZone{};
        bool newZonePressed = false; // the dragged app is over it, to start a section
        float height = 0;
    };

    // While dragging (`dragging`), the apps in no section have a row even with none, to drop one into,
    // and "+ Yeni bölüm" follows the last section: where the section it starts would be, a name row
    // and a row of tiles (the user's wish). With the dragged app over it (starting a section) it stays,
    // pressed, the app where it would be in it (the user's wish: it vanished, a name row showing
    // instead); the section's name row comes once the app is let go.
    // The sections may stand side by side (config `sectionColumns`, 1 to 4; the user's idea, after
    // Windows 10's tile groups): the apps in no section across the top, then the sections row by row,
    // that many to a row, left to right, each as many tiles wide as its share of the grid allows; a
    // row is as tall as its tallest section. With one, each section runs across, as it always did.
    int SectionColumnCount() { return std::clamp(g_config.sectionColumns, 1, 4); }
    // A section column's width: an even share of the grid's, the columns parted by SectionGap as the
    // rows are, so together they span the grid (the user's wish: cut to their whole tiles, they left
    // the grid's right end bare). Its tiles run from its left as many as fit in it.
    float SectionColumnWidth()
    {
        int n = SectionColumnCount();
        return n == 1 ? GridWidth() : std::floor((GridWidth() - (n - 1) * SectionGap) / n);
    }
    float SectionColumnStep() { return SectionColumnWidth() + SectionGap; }
    // Within a side-by-side section its own grid (the user's wish: its right end stayed bare, then the
    // room was uneven): its tiles spread evenly across its box, the same room at the box's sides as
    // between them, never less than the gap set; down, the gap set (the user found the rows right).
    // With one column and the tiles their size set, the menu's grid; smaller (SectionScale), their own.
    bool SectionOwnGrid() { return SectionColumnCount() > 1 || SectionScale() != 1; }
    int SectionColumnTiles()
    {
        if (!SectionOwnGrid())
            return Columns();
        auto d = Dims(true);
        float box = SectionColumnWidth() + 2 * SectionPad;
        return std::max(1, int((box - d.gap) / (d.w + d.gap)));
    }
    float SectionTileGap()
    {
        auto d = Dims(true);
        if (!SectionOwnGrid())
            return d.gap;
        int k = SectionColumnTiles();
        return std::max(d.gap, (SectionColumnWidth() + 2 * SectionPad - k * d.w) / (k + 1));
    }
    float SectionTileStep() { return Dims(true).w + SectionTileGap(); }
    // The first tile's left edge in a section from x0: the even room in from its box's edge.
    float SectionTilesLeft(float x0) { return !SectionOwnGrid() ? x0 : x0 - SectionPad + SectionTileGap(); }

    // Each column holds its own sections, top to bottom, independent of the others (the user's choice,
    // 16.10.2026: in rows, a tall section left room bare beside its neighbours). A section keeps its
    // column (`PinGroup::column`); in a column, they go in the pins' order. One with none yet (new, or
    // its column gone as there are fewer now) goes under the shortest column. While a section is moved
    // (see "Moving a section") it stands where it would drop.
    using SectionPlan = std::vector<std::vector<int>>;
    int g_secDrag = 0; // the section being moved, if any
    // While an app is dragged, the section it came from (0: none) keeps the rows it had, even emptied,
    // till it is let go (the user's wish: as it lost a row, everything under it slid away from under
    // the pointer); then they close, gently, as the tiles settle.
    int g_dragFromSection = -1, g_dragFromRows = 0; // -1: no app dragged
    int g_secDragColumn = 0, g_secDragIndex = 0;    // where it would drop: its column, its place there

    int StoredColumn(int section)
    {
        for (auto const &g : g_config.pinGroups)
            if (g.id == section)
                return g.column;
        return -1;
    }

    // From the room between one section's box and the next one's under it, with the boxes' pads.
    float SectionParting() { return TileGap() + SectionGap - SectionPad; }

    SectionPlan PlanSections(std::vector<GridEntry> const &entries, bool dragging)
    {
        int n = SectionColumnCount(), across = SectionColumnTiles();
        std::vector<std::pair<int, int>> sections; // each section and its apps, in order
        for (auto const &e : entries)
        {
            if (!e.section || (dragging && e.section == NewSection))
                continue;
            if (sections.empty() || sections.back().first != e.section)
                sections.push_back({e.section, 0});
            sections.back().second++;
        }
        // While dragging, the section the app came from stays, emptied or not (see g_dragFromSection),
        // in its place among the others.
        if (dragging && g_dragFromSection > 0 &&
            std::none_of(sections.begin(), sections.end(), [](auto const &s)
                         { return s.first == g_dragFromSection; }))
        {
            size_t at = 0;
            for (auto const &tile : g_pinned)
            {
                if (tile.group == g_dragFromSection)
                    break;
                if (tile.group && at < sections.size() && sections[at].first == tile.group)
                    at++;
            }
            sections.insert(sections.begin() + std::min(at, sections.size()), {g_dragFromSection, 0});
        }
        SectionPlan plan(n);
        std::vector<float> height(n, 0.f);
        for (auto const &[section, count] : sections)
        {
            if (section == g_secDrag)
                continue; // put where it would drop, below
            int c = StoredColumn(section);
            if (c < 0 || c >= n)
                c = int(std::min_element(height.begin(), height.end()) - height.begin());
            plan[c].push_back(section);
            int rows = std::max(1, (count + across - 1) / across);
            auto d = Dims(true);
            height[c] += SectionNameRowH() + rows * d.h + (rows - 1) * d.gap + 2 * SectionPad + SectionParting();
        }
        if (g_secDrag && std::any_of(sections.begin(), sections.end(), [](auto const &s)
                                     { return s.first == g_secDrag; }))
        {
            auto &column = plan[std::clamp(g_secDragColumn, 0, n - 1)];
            column.insert(column.begin() + std::clamp(g_secDragIndex, 0, int(column.size())), g_secDrag);
        }
        return plan;
    }

    GridLayout LayOutSections(std::vector<GridEntry> const &entries, bool dragging)
    {
        GridLayout layout;
        layout.cells.resize(g_pinned.size());
        float left = GridLeft();
        float bottom = GridY; // the lowest box's bottom
        // Each section's entries, in order.
        std::unordered_map<int, std::vector<GridEntry>> of;
        for (auto const &e : entries)
            of[e.section].push_back(e);
        auto box = [&](float x0, float x1, float top, float end)
        {
            D2D1_RECT_F r{x0 - SectionPad, top - SectionPad, x1 + SectionPad, end + SectionPad};
            bottom = std::max(bottom, r.bottom);
            return r;
        };
        // One section (0: the apps in none) from `start`, its box from x0 to x1, its tiles from
        // `first`, `across` to a row, `g` apart across (the gap set down): its name row (lined up with
        // its tiles), its tiles; its box's bottom.
        // The apps in no section may be centred (config `centerUngrouped`, the user's wish): each row in
        // the grid's middle, so a short last row is too; whole DIPs, as the rest.
        auto place = [&](int section, float x0, float x1, float first, int across, float g, float start)
        {
            auto d = Dims(section != 0);
            float w = d.w, h = d.h, gap = d.gap;
            float tiles = start + (section ? SectionNameRowH() : 0), last = first + (across - 1) * (w + g) + w;
            if (section)
                layout.headers.push_back({section, {first, start, last, tiles}});
            bool centred = !section && g_config.centerUngrouped;
            int k = 0, total = int(of[section].size());
            for (auto const &e : of[section])
            {
                int inRow = std::min(across, total - k / across * across);
                float shift = centred ? std::floor((across - inRow) * (w + g) / 2) : 0;
                float x = first + shift + (k % across) * (w + g), top = tiles + (k / across) * (h + gap);
                layout.cells[e.pin] = {x, top, x + w, top + h};
                k++;
            }
            // The apps in no section get no row of their own while dragging unless the dragged app goes
            // there (TargetAt: over the sections' top): opened at once as a drag started, it threw the
            // grid down (the user saw it); now it slides open when wanted.
            int rows = (k + across - 1) / across;
            if (dragging && section == g_dragFromSection)
                rows = std::max(rows, g_dragFromRows);
            float end = rows ? tiles + rows * h + (rows - 1) * gap : tiles;
            auto r = box(x0, x1, start, end);
            layout.regions.push_back({section, r});
            return rows || section ? r.bottom : start;
        };
        float boxEnd = place(0, left, left + GridWidth(), left, Columns(), TileGap(), GridY);
        bool above = boxEnd > GridY; // the apps in no section are there
        // The sections, each column down from the same top; each box the gap and 8 under the last's.
        int n = SectionColumnCount(), across = SectionColumnTiles();
        float step = SectionColumnStep(), g = SectionTileGap(), parting = SectionParting();
        // With no apps out of sections, the boxes start where the tiles would (their pad below it):
        // from GridY itself they touched the title row's "Tüm uygulamalar" (the user saw it).
        float top = above ? boxEnd + parting : GridY + SectionPad;
        std::vector<float> next(n, top); // each column's next section's top
        auto plan = PlanSections(entries, dragging);
        for (int c = 0; c < n; c++)
        {
            float x0 = left + c * step;
            for (int section : plan[c])
                next[c] = place(section, x0, x0 + SectionColumnWidth(), SectionTilesLeft(x0), across, g, next[c]) + parting;
        }
        if (dragging)
        {
            // Where the section it starts would go: under the shortest column.
            int c = int(std::min_element(next.begin(), next.end()) - next.begin());
            auto d = Dims(true);
            float x0 = left + c * step, first = SectionTilesLeft(x0), start = next[c], tiles = start + SectionNameRowH();
            layout.newZone = box(x0, x0 + SectionColumnWidth(), start, tiles + d.h);
            if (auto it = of.find(NewSection); it != of.end() && !it->second.empty())
            {
                layout.cells[it->second.front().pin] = {first, tiles, first + d.w, tiles + d.h};
                layout.newZonePressed = true;
            }
        }
        layout.height = bottom - GridY;
        return layout;
    }

    std::vector<GridEntry> PinEntries();
    extern int g_pinsVersion;

    // A section's grip, while the settings panel is open (see "Moving a section"): at its box's top
    // right, in its name row; with the names off, on the box's top edge, so it covers no tile.
    constexpr float SectionGripW = 32, SectionGripH = 22;
    D2D1_RECT_F SectionGripRect(D2D1_RECT_F box)
    {
        float top = g_config.sectionNames ? box.top + 8 : box.top - SectionGripH / 2 + 2;
        return {box.right - 8 - SectionGripW, top, box.right - 8, top + SectionGripH};
    }
    bool IsSectionGrip(int key) { return key >= KeySectionGrip && key < KeySearchBack; }
    bool g_secDragging = false; // a section held by its grip

    // True if it was another.
    bool SetSectionColumn(int section, int column)
    {
        auto it = std::find_if(g_config.pinGroups.begin(), g_config.pinGroups.end(), [&](PinGroup const &g)
                               { return g.id == section; });
        if (it == g_config.pinGroups.end())
            it = g_config.pinGroups.insert(g_config.pinGroups.end(), {section, L"", -1});
        if (it->column == column)
            return false;
        it->column = column;
        return true;
    }

    // Writes down each section's column as the plan has it, those with none (or a column no longer
    // there) included; true if any changed. Before the first time, as the rows had them: 1, 2, 3, 1, ...
    bool FixSectionColumns()
    {
        int n = SectionColumnCount();
        auto entries = PinEntries();
        bool any = std::any_of(g_config.pinGroups.begin(), g_config.pinGroups.end(), [n](PinGroup const &g)
                               { return g.column >= 0 && g.column < n; });
        bool changed = false;
        auto set = [&](int section, int column)
        {
            if (SetSectionColumn(section, column))
                changed = true, g_pinsVersion++;
        };
        if (!any)
        {
            int k = 0, last = 0;
            for (auto const &e : entries)
                if (e.section && e.section != last)
                    set(last = e.section, k++ % n);
            return changed;
        }
        auto plan = PlanSections(entries, false);
        for (int c = 0; c < n; c++)
            for (int section : plan[c])
                set(section, c);
        return changed;
    }

    int g_pinsVersion = 0; // bumps whenever g_pinned (or a section's name) changes, so its layout is made again

    // g_pinned in order: the apps in no section first, then each section's (SplitPins).
    std::vector<GridEntry> PinEntries()
    {
        std::vector<GridEntry> entries;
        for (int i = 0; i < int(g_pinned.size()); i++)
            entries.push_back({i, g_pinned[i].group});
        return entries;
    }

    GridLayout const &PinLayout()
    {
        static GridLayout layout;
        // Remade when the pins, the columns, the tiles or the gap change, the grid moves in its room,
        // or the sections stand in more or fewer columns.
        static std::tuple<int, int, float, float, float, float, int, float, bool, float> made{-1, 0, 0.f, 0.f, 0.f, 0.f, 0, 0.f, false, 0.f};
        std::tuple<int, int, float, float, float, float, int, float, bool, float> now{g_pinsVersion, Columns(), CellWidth(), CellH(), TileGap(),
                                                                                      GridLeft(), SectionColumnCount(), SectionNameRowH(),
                                                                                      g_config.centerUngrouped, SectionScale()};
        if (now != made)
        {
            made = now;
            layout = LayOutSections(PinEntries(), false);
        }
        return layout;
    }

    float GridHeight() { return g_pinned.empty() ? 30 : PinLayout().height; }

    // On whole device pixels, as the cells are, scrolled.
    D2D1_RECT_F Snapped(D2D1_RECT_F r, float scroll)
    {
        float scale = g_dpi / 96.f;
        float x = std::round(r.left * scale) / scale, y = std::round(r.top * scale) / scale - scroll;
        return {x, y, x + (r.right - r.left), y + (r.bottom - r.top)};
    }

    // Each cell starts on a whole device pixel (the cells are a fraction of a pixel wide once scaled).
    // So a tile looks the same, pixel for pixel, drawn in the grid or in a layer of its own at the
    // cell's place, as it is while pins are dragged: icons snap to pixels from the same offset.
    D2D1_RECT_F PinnedRect(int i) { return Snapped(PinLayout().cells[i], g_scroll); }

    // Where a dragged app would go (see "Reordering pins"): a section (or NewSection), and its place
    // among that section's other apps.
    struct AppTarget
    {
        int section = 0, index = 0;
        bool operator==(AppTarget const &) const = default;
    };
    AppTarget g_dragTarget;

    // The pins in order with the dragged app at `target`: out of where it was, into its section at its
    // place (a new section at the end; one it left empty gone).
    std::vector<GridEntry> DragEntries(AppTarget const &target)
    {
        auto all = PinEntries();
        std::vector<GridEntry> entries;
        int from = 0;
        for (auto const &e : all)
            if (e.pin == g_dragPin)
                from = int(entries.size());
            else
                entries.push_back(e);
        GridEntry dragged{g_dragPin, target.section};
        if (target.section == NewSection)
        {
            entries.push_back(dragged);
            return entries;
        }
        // Before the target.index-th of its section; after its last; or, a section with no others
        // (the apps in no section, or the one it is the last of), where it was.
        int at = -1, seen = 0, last = -1;
        for (int k = 0; k < int(entries.size()); k++)
            if (entries[k].section == target.section)
            {
                if (seen++ == target.index && at < 0)
                    at = k;
                last = k;
            }
        if (at < 0)
            at = last >= 0 ? last + 1 : target.section ? from
                                                       : 0;
        entries.insert(entries.begin() + at, dragged);
        return entries;
    }

    D2D1_RECT_F FolderRect(int i)
    {
        float y = GridY + i * FolderRowH() - g_folderScroll;
        return {FolderLeft(), y, FolderRight, y + FolderRowH()};
    }

    // The "+" row after the folders, which adds one (see "Adding a folder"): at the list's end, or held
    // at the scrolling view's bottom while the end is below it, the rows scrolling up from under it.
    D2D1_RECT_F FolderAddRect()
    {
        float h = FolderRowH();
        float y = std::min(GridY + g_folders.size() * h - g_folderScroll, SplitBottom - h);
        return {FolderLeft(), y, FolderRight, y + h};
    }

    bool InFolderColumn(D2D1_POINT_2F p) { return g_config.folders && p.x >= FolderSplit(); }

    // At the box's right end, always, faint: a reminder that Windows' own search (files, settings)
    // is Win+S; this menu searches only apps (the user's choice, to keep it light).
    constexpr wchar_t SearchHint[] = L"Detaylı arama için Win+S";
    constexpr float SearchHintGap = 16;   // between the text's room and the hint
    constexpr float SearchTextMinW = 120; // a box with less room for the text than this leaves the hint out
    float TextWidth(std::wstring_view text, IDWriteTextFormat *format);
    float SearchHintW()
    {
        static float width = -1;
        if (width < 0)
            width = std::ceil(TextWidth(SearchHint, g_fmtName.Get()));
        return width;
    }

    // Puts the search box where the layout now has it (see SearchBox); called before every draw, as
    // the column line, the menu's size or the setting can move it. Its fill is on the bottom layer,
    // which is drawn again when it moves.
    D2D1_RECT_F AccountRect();
    D2D1_RECT_F BarRect(int i);

    void PlaceSearchBox()
    {
        // Centred on the apps side whatever is beside it: where the picture or the buttons come too
        // close, the box gives way on both sides alike, so its middle never moves.
        float middle = GridLeft() + GridWidth() / 2;
        float left = AccountRect().right + SearchBarGap, right = BarRect(0).left - SearchBarGap;
        float half = std::max(0.f, std::min({SearchBoxMaxW / 2, middle - left, right - middle}));
        float top = ContentBottom + std::floor((BarH - SearchBoxH) / 2); // whole pixels, as the bar's buttons
        D2D1_RECT_F box{std::round(middle - half), top, std::round(middle + half), top + SearchBoxH};
        if (box.left != SearchBox.left || box.right != SearchBox.right || box.top != SearchBox.top)
            g_backDirty = true;
        SearchBox = box;
        SearchTextLeft = box.left + 44;
        // The text stops short of the hint; a box too short for both leaves the hint out.
        SearchTextRight = box.right - 14;
        if (SearchTextRight - SearchTextLeft - SearchHintW() - SearchHintGap >= SearchTextMinW)
            SearchTextRight -= SearchHintW() + SearchHintGap;
    }

    // While searching, the magnifier turns into a back arrow that leaves the search (as Esc does); lit
    // under the pointer in a pill inside the box, 4 in from its edges, its round end on the box's own
    // (the user's wish: in the box's form). Centred on the arrow, at the magnifier's place.
    constexpr float SearchBackInset = 4;
    D2D1_RECT_F SearchBackRect()
    {
        float cx = SearchBox.left + 23;
        return {SearchBox.left + SearchBackInset, SearchBox.top + SearchBackInset,
                2 * cx - (SearchBox.left + SearchBackInset), SearchBox.bottom - SearchBackInset};
    }

    // The largest menu the screen has room for (DIPs): its work area less the gap around the menu.
    D2D1_SIZE_F MenuRoom()
    {
        RECT work;
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        float toDips = 96.f / g_dpi;
        return {std::max(float(MenuWidthMin), (work.right - work.left) * toDips - 2 * ScreenGap),
                std::max(float(MenuHeightMin), (work.bottom - work.top) * toDips - 2 * ScreenGap)};
    }

    // The menu's sizes to choose from (the user's choice, 16.10.2026, in place of sliders), as shares
    // of the screen so they mean as much on any: the width from Windows' own to the screen's in three
    // even steps; the height Windows' own, the screen's, and as far under Windows' own as the screen's
    // is over it (no lower than 600).
    constexpr int MenuWidthSteps = 4, MenuHeightSteps = 4;
    constexpr wchar_t const *MenuWidthNames[MenuWidthSteps] = {L"Standart", L"Geniş", L"Daha geniş", L"Tam"};
    constexpr wchar_t const *MenuHeightNames[MenuHeightSteps] = {L"Daha alçak", L"Alçak", L"Standart", L"Yüksek"};

    float MenuWidthOf(int step)
    {
        float room = MenuRoom().width, standard = std::min(WindowsMenuW, room);
        return std::round(standard + std::clamp(step, 0, MenuWidthSteps - 1) * (room - standard) / (MenuWidthSteps - 1));
    }

    // The height: lower still (the user's wish) as far again under the low one, no lower than the least.
    float MenuHeightOf(int step)
    {
        float room = MenuRoom().height, standard = std::min(866.f, room);
        float low = std::round(std::max(600.f, standard - std::max(room - standard, 120.f)));
        if (step >= 3)
            return std::round(room);
        if (step == 2)
            return standard;
        if (step == 1)
            return low;
        return std::round(std::max(float(MenuHeightMin), low - (standard - low)));
    }

    // A size saved in DIPs by an older version: the nearest choice.
    int NearestStep(float size, float (*of)(int), int steps)
    {
        int best = 0;
        for (int k = 1; k < steps; k++)
            if (std::abs(of(k) - size) < std::abs(of(best) - size))
                best = k;
        return best;
    }

    // Sizes the menu from the settings.
    void LayoutMenu()
    {
        MenuW = MenuWidthOf(g_config.menuWidthStep);
        MenuH = MenuHeightOf(g_config.menuHeightStep);
        g_barShift = std::round((MenuW - WindowsMenuW) / 2); // for --render; the window works it out from the screen
        ContentBottom = MenuH - BarH;
        FolderRight = MenuW - 20;
        SplitBottom = ContentBottom - 20;
        PlaceSearchBox();
        g_drawTop = ContentTop;
        g_drawBottom = ContentBottom;
    }

    D2D1_RECT_F SplitHandleRect()
    {
        float x = FolderSplit() + 0.5f, y = (SplitTop + SplitBottom) / 2;
        return {x - SplitHandleW / 2, y - SplitHandleH / 2, x + SplitHandleW / 2, y + SplitHandleH / 2};
    }

    // Typing shows the search results on the apps side, the folders staying beside them (the user's
    // wish: the results covered both columns before); in the every-apps list it only narrows the list.
    bool SearchView() { return !g_query.empty() && !g_allApps; }
    // The apps side shows a list of apps (g_results) rather than the pinned grid.
    bool ShowingList() { return SearchView() || g_allApps; }

    // The every-apps list has a title row above it, as the home view; search results start at the top.
    // Search results run upward from the bottom, the best one just above the search box in the bar
    // (user's choice): the list sits at the scrolling view's bottom, in reverse, and opens scrolled
    // to its end. The every-apps list runs downward from its title row.
    bool ResultsUpward() { return SearchView(); }

    float ListTop()
    {
        if (g_allApps)
            return GridY;
        if (!ResultsUpward())
            return ListY;
        return ListY + std::max(0.f, SplitBottom - ListY - std::max<size_t>(g_results.size(), 1) * RowH);
    }

    // The top of what scrolls on the apps side. The every-apps list's title row (its title, sort
    // buttons and "‹ Geri") stays put, the rows passing under it (the user's wish: they scrolled away
    // with the list, and changing the sort meant going back to the top).
    float AppsViewTop() { return g_allApps ? GridY - 8 : SplitTop; }

    // Where result i is in the list as drawn, from the top.
    int ResultSlot(int i) { return ResultsUpward() ? int(g_results.size()) - 1 - i : i; }

    D2D1_RECT_F ResultRect(int i)
    {
        float y = ListTop() + ResultSlot(i) * RowH - g_scroll;
        return {20, y, FolderSplit() - 16, y + RowH};
    }

    // In the every-apps list sorted by letter, a row can be a letter heading: stored as -(letter + 1).
    bool IsHeading(int row) { return g_results[row] < 0; }
    wchar_t HeadingLetter(int row) { return wchar_t(-g_results[row] - 1); }

    float TextWidth(std::wstring_view text, IDWriteTextFormat *format)
    {
        ComPtr<IDWriteTextLayout> layout;
        DWRITE_TEXT_METRICS m{};
        if (SUCCEEDED(g_dwrite->CreateTextLayout(text.data(), UINT32(text.size()), format, 10000, 100, &layout)))
            layout->GetMetrics(&m);
        return m.widthIncludingTrailingWhitespace;
    }

    // A-Z is grouped under letter headings, as Windows' all-apps list.
    constexpr wchar_t const *SortNames[] = {L"A–Z", L"Son eklenen"};
    constexpr int SortCount = int(std::size(SortNames));
    constexpr wchar_t AllAppsTitle[] = L"Tüm uygulamalar";

    // The every-apps list's sort buttons, after its title and count, in its fixed title row.
    D2D1_RECT_F SortButtonRect(int i)
    {
        float x = 32 + TextWidth(AllAppsTitle, g_fmtTitle.Get()) + 8 + TextWidth(std::to_wstring(g_apps.size()), g_fmtName.Get()) + 16;
        for (int j = 0; j < i; j++)
            x += TextWidth(SortNames[j], g_fmtName.Get()) + 24 + 4;
        float w = TextWidth(SortNames[i], g_fmtName.Get()) + 24, cy = TitleY + 10;
        x = std::round(x);
        return {x, cy - 14, std::round(x + w), cy + 14};
    }

    // The button at the apps title row's end, like Windows' "All >": it opens the every-apps list,
    // scrolling with the grid, and in the list it goes back, in the list's fixed title row. Always there
    // outside the search view.
    float TextWidth(std::wstring_view text, IDWriteTextFormat *format);
    wchar_t const *AllAppsButtonText() { return g_allApps ? L"Geri" : L"Tüm uygulamalar"; }
    constexpr float ButtonPad = 10, ChevronW = 5, ChevronGap = 8;

    D2D1_RECT_F AllAppsButtonRect()
    {
        // On the grid, its right edge on the grid's (the last tile's), inside the sections' boxes (the
        // user's wish: it stood out past them); over the every-apps list, on the list's.
        float right = g_allApps ? FolderSplit() - 16 : GridLeft() + GridWidth(), cy = TitleY + 10 - (g_allApps ? 0 : g_scroll);
        float w = std::round(ButtonPad + TextWidth(AllAppsButtonText(), g_fmtName.Get()) + ChevronGap + ChevronW + ButtonPad);
        return {right - w, cy - 13, right, cy + 13};
    }

    bool AllAppsButtonShown() { return !SearchView(); }

    // The stretch of the bottom bar its buttons keep to: the whole menu; or, with the bar fixed (config
    // `fixedBar`) on a menu wider than Windows' own, where Windows' 834 wide menu would be. Centred on
    // the taskbar it is centred as the menu is, so the buttons stay on the screen where Windows' are,
    // whatever the menu's width (PlaceWindow works that out in pixels: g_barShift).

    float BarSpanLeft() { return g_config.fixedBar && MenuW > WindowsMenuW ? g_barShift : 0; }
    float BarSpanRight() { return g_config.fixedBar && MenuW > WindowsMenuW ? g_barShift + WindowsMenuW : MenuW; }

    D2D1_RECT_F BarRect(int i)
    {
        // The power button at the right; the shown small ones (a size down from it in its ring, user's
        // wish) close up to its left, the same gap between each two (user's wish). A hidden one gets
        // the place of the next shown, so BarRect(0).left is always where the leftmost shown starts.
        float x = BarSpanRight() - BarRight - BarButton;
        float y = ContentBottom + std::floor((BarH - BarButton) / 2); // whole pixels, as Windows places them
        int shown = 0;                                                // the shown small ones from this one to the power button
        for (int j = i; j < PowerButton; j++)
            shown += g_barShown[j];
        if (i == PowerButton || shown == 0)
            return {x, y, x + BarButton, y + BarButton};
        float size = BarButton - 2 * BarSmallInset, right = x - shown * BarGap - (shown - 1) * size;
        return {right - size, y + BarSmallInset, right, y + BarSmallInset + size};
    }

    // The account button: just the picture (no name, user's choice), in a square with 4 around it.
    D2D1_RECT_F AccountRect()
    {
        float y = ContentBottom + (BarH - AccountH) / 2;
        float x = BarSpanLeft() + BarLeft + 4; // the picture stays where it was when the name followed it
        // On whole device pixels: the highlight layer is placed in whole pixels, so a fractional
        // rect (the bar centres it at half a DIP) left the circle a pixel off the picture.
        return Snapped({x, y, x + AccountH, y + AccountH}, 0);
    }

    D2D1_RECT_F KeyRect(int key)
    {
        if (key >= KeyResult)
            return ResultRect(key - KeyResult);
        if (key == KeyAllApps)
            return AllAppsButtonRect();
        if (key >= KeySort && key < KeySort + SortCount)
            return SortButtonRect(key - KeySort);
        if (key == KeyAccount)
            return AccountRect();
        if (key == KeySearchBack)
            return SearchBackRect();
        if (key == KeyFolderAdd)
            return FolderAddRect();
        if (key >= KeyBar)
            return BarRect(key - KeyBar);
        if (key >= KeyFolder)
            return FolderRect(key - KeyFolder);
        return PinnedRect(key - KeyPinned);
    }

    bool IsContentKey(int key) { return key < KeyBar || key >= KeyResult || (key >= KeySort && key <= KeyAllApps); }

    // How far each column scrolls: its content's end up to the scrolling view's bottom (SplitBottom),
    // which leaves its own margin above the bottom bar.
    float MaxScroll()
    {
        float end = !ShowingList() ? GridY + GridHeight() : ListTop() + g_results.size() * RowH;
        return std::max(0.f, end - SplitBottom);
    }

    float MaxFolderScroll()
    {
        float end = GridY + (g_folders.size() + 1) * FolderRowH(); // the rows and the "+" row
        return std::max(0.f, end - SplitBottom);
    }

    // The scrollbars, from the first row down, shown when there is more than fits; thin as Windows 11
    // draws them, wider under the pointer. By index (`bar`): 0 the apps side's, in the gap between its
    // content and the column line; 1 the folder list's, between the list and the menu's edge.
    constexpr float ScrollbarHit = 12; // the width that takes the pointer

    float &BarScroll(int bar) { return bar ? g_folderScroll : g_scroll; }
    float BarMaxScroll(int bar) { return bar ? MaxFolderScroll() : MaxScroll(); }
    int BarKey(int bar) { return bar ? KeyFolderScrollbar : KeyScrollbar; }

    D2D1_RECT_F ScrollTrack(int bar = 0)
    {
        float x = bar ? MenuW - 10 : FolderSplit() - 8;
        return {x - ScrollbarHit / 2, GridY, x + ScrollbarHit / 2, SplitBottom};
    }

    // The search results have none; the folder list keeps its own beside them.
    bool ScrollbarShown(int bar = 0) { return (bar ? g_config.folders : !SearchView()) && BarMaxScroll(bar) > 0; }

    // The thumb's length is the visible share of the whole; its place, how far down the scroll is.
    D2D1_RECT_F ScrollThumbAt(float scroll, int bar = 0)
    {
        auto track = ScrollTrack(bar);
        float max = BarMaxScroll(bar), trackH = track.bottom - track.top, visible = SplitBottom - SplitTop;
        float h = std::max(24.f, trackH * visible / (visible + max));
        float y = track.top + (trackH - h) * (max > 0 ? scroll / max : 0);
        return {track.left, y, track.right, y + h};
    }

    D2D1_RECT_F ScrollThumb(int bar = 0) { return ScrollThumbAt(BarScroll(bar), bar); }

    // After the pins or the layout change, both columns back within their range.
    void ClampScrolls()
    {
        g_scroll = std::clamp(g_scroll, 0.f, MaxScroll());
        g_folderScroll = std::clamp(g_folderScroll, 0.f, MaxFolderScroll());
    }

    bool Inside(D2D1_RECT_F r, D2D1_POINT_2F p) { return p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom; }

    int HitTest(D2D1_POINT_2F p)
    {
        if (!g_query.empty() && Inside(SearchBackRect(), p))
            return KeySearchBack;
        if (Inside(SearchBox, p))
            return KeySearch;
        if (p.y >= ContentTop && p.y < ContentBottom)
        {
            // Beyond the scrolling view nothing shows, so nothing there is hit.
            if (p.y < SplitTop || p.y >= SplitBottom)
                return KeyNone;
            if (AllAppsButtonShown() && Inside(AllAppsButtonRect(), p))
                return KeyAllApps;
            if (g_allApps)
                for (int i = 0; i < SortCount; i++)
                    if (Inside(SortButtonRect(i), p))
                        return KeySort + i;
            // The folder column stays beside the search results too (the user's wish), as beside the
            // every-apps list.
            if (g_setOpen && g_config.folders && (Inside(SplitHandleRect(), p) ||
                              (std::abs(p.x - FolderSplit()) <= SplitGrab && p.y >= SplitTop && p.y < SplitBottom)))
                return KeySplitter;
            // The sections' grips, while the settings panel is open.
            if (g_setOpen && !ShowingList())
            {
                auto const &regions = PinLayout().regions;
                for (int k = 0; k < int(regions.size()); k++)
                    if (regions[k].first && Inside(SectionGripRect(Snapped(regions[k].second, g_scroll)), p))
                        return KeySectionGrip + k;
            }
            for (int bar : {0, 1})
                if (ScrollbarShown(bar) && Inside(ScrollTrack(bar), p))
                    return BarKey(bar);
            if (InFolderColumn(p))
            {
                // The "+" row first: the rows under it, where it is held at the bottom, are hidden.
                if (Inside(FolderAddRect(), p))
                    return KeyFolderAdd;
                if (p.y >= FolderAddRect().top)
                    return KeyNone;
                int i = int((p.y - GridY + g_folderScroll) / FolderRowH());
                if (p.y - GridY + g_folderScroll >= 0 && i < int(g_folders.size()) && Inside(FolderRect(i), p))
                    return KeyFolder + i;
                return KeyNone;
            }
            if (ShowingList())
            {
                if (p.y < AppsViewTop())
                    return KeyNone; // under the every-apps list's title row
                // Letter headings are not items.
                int slot = int((p.y - ListTop() + g_scroll) / RowH);
                if (p.y - ListTop() + g_scroll >= 0 && slot < int(g_results.size()))
                    if (int i = ResultSlot(slot); !IsHeading(i) && Inside(ResultRect(i), p))
                        return KeyResult + i;
            }
            else
            {
                for (int i = 0; i < int(g_pinned.size()); i++)
                    if (Inside(PinnedRect(i), p))
                        return KeyPinned + i;
            }
            return KeyNone;
        }
        if (Inside(AccountRect(), p))
            return KeyAccount;
        for (int i = 0; i < BarCount; i++)
            if (g_barShown[i] && Inside(BarRect(i), p))
                return KeyBar + i;
        return KeyNone;
    }

    // ---- Composition helpers ----

    wuc::CompositionDrawingSurface CreateSurface(float width, float height)
    {
        return g_graphics.CreateDrawingSurface({Px(width), Px(height)},
                                               winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
                                               winrt::Windows::Graphics::DirectX::DirectXAlphaMode::Premultiplied);
    }

    template <class F>
    void DrawSurface(wuc::CompositionDrawingSurface const &surface, float width, float height, F &&draw)
    {
        auto interop = surface.as<ABI::Windows::UI::Composition::ICompositionDrawingSurfaceInterop>();
        ComPtr<ID2D1DeviceContext> dc;
        POINT offset{};
        if (FAILED(interop->BeginDraw(nullptr, __uuidof(ID2D1DeviceContext), reinterpret_cast<void **>(dc.GetAddressOf()), &offset)))
            return;
        float toDips = 96.f / g_dpi;
        dc->SetDpi(float(g_dpi), float(g_dpi));
        dc->SetTransform(D2D1::Matrix3x2F::Translation(offset.x * toDips, offset.y * toDips));
        dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE); // ClearType needs an opaque background
        // The surface can live inside a shared atlas, so stay within our own rectangle.
        dc->PushAxisAlignedClip({0, 0, width, height}, D2D1_ANTIALIAS_MODE_ALIASED);
        dc->Clear(D2D1::ColorF(0, 0, 0, 0));
        g_dc = dc;
        draw();
        g_dc = g_resources;
        dc->PopAxisAlignedClip();
        interop->EndDraw();
    }

    // A brush for a sprite w x h (its size Px(w) x Px(h)) that moves, or scales, by fractions of a
    // pixel. Small surfaces share an atlas, and a sprite between pixels samples a little past its
    // surface's edge: the neighbour's pixels showed as thin lines beside the dragged tiles (the user
    // saw them). So the surface has a clear margin all round, its own pixels to sample there, and the
    // brush shows the middle of it, unstretched (the margin is the same each side, in whole pixels).
    constexpr int AtlasPad = 2; // pixels

    template <class F>
    wuc::CompositionSurfaceBrush PaddedBrush(float w, float h, F &&draw)
    {
        float toDips = 96.f / g_dpi, pad = AtlasPad * toDips;
        float sw = (Px(w) + 2 * AtlasPad) * toDips, sh = (Px(h) + 2 * AtlasPad) * toDips;
        auto surface = g_graphics.CreateDrawingSurface({Px(w) + 2 * AtlasPad, Px(h) + 2 * AtlasPad},
                                                       winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
                                                       winrt::Windows::Graphics::DirectX::DirectXAlphaMode::Premultiplied);
        DrawSurface(surface, sw, sh, [&]
                    {
        D2D1::Matrix3x2F at;
        g_dc->GetTransform(&at);
        g_dc->SetTransform(D2D1::Matrix3x2F::Translation(pad, pad) * at);
        draw();
        g_dc->SetTransform(at); });
        auto brush = g_compositor.CreateSurfaceBrush(surface);
        brush.Stretch(wuc::CompositionStretch::None); // centred, at 1:1
        return brush;
    }

    void Animate(wuc::CompositionObject const &object, wchar_t const *property, float to, std::chrono::milliseconds duration,
                 wuc::CompositionEasingFunction const &ease)
    {
        // With no key frame at 0 the animation starts from wherever the value is now, so an
        // interrupted fade turns around smoothly.
        auto animation = g_compositor.CreateScalarKeyFrameAnimation();
        animation.InsertKeyFrame(1.f, to, ease);
        animation.Duration(duration);
        object.StartAnimation(property, animation);
    }

    wuc::CompositionEasingFunction Bezier(float x1, float y1, float x2, float y2)
    {
        return g_compositor.CreateCubicBezierEasingFunction({x1, y1}, {x2, y2});
    }

    // Puts a soft shadow under a rounded body: the sprite is sized and placed like the body, and the
    // shadow's shape comes from a mask that follows the sprite's size. The mask is a small rounded
    // square stretched by a nine-grid: its corners and edges stay 1:1, only its middle stretches. A
    // full-size mask would cost megabytes for the menu; a quarter-size one stretched fourfold sampled
    // its neighbours in the compositor's atlas at its edges, and the menu's bottom shadow came out
    // darker after some size changes (the user saw it; measured against a fresh open, 17.10.2026).
    void CastShadow(wuc::SpriteVisual const &sprite)
    {
        constexpr float corner = 8, box = 2 * corner + 2;
        auto mask = CreateSurface(box, box);
        DrawSurface(mask, box, box, [&]
                    { g_dc->FillRoundedRectangle(D2D1::RoundedRect({0, 0, box, box}, corner, corner), Brush(0xFF000000)); });
        auto grid = g_compositor.CreateNineGridBrush();
        auto source = g_compositor.CreateSurfaceBrush(mask);
        source.Stretch(wuc::CompositionStretch::Fill);
        grid.Source(source);
        grid.SetInsets(Px(corner));
        auto shadow = g_compositor.CreateDropShadow();
        shadow.Mask(grid);
        shadow.BlurRadius(Px(ShadowMargin * 0.75f));
        shadow.Offset({0, Px(4), 0});
        shadow.Color(winrt::Windows::UI::Color{255, 0, 0, 0});
        shadow.Opacity(0.45f);
        sprite.Shadow(shadow);
    }

    // ---- Acrylic ----
    //
    // The compositor runs Direct2D effects on what is behind a window, if they are described to it
    // through IGraphicsEffect. Win2D's effect classes do that; we have no Win2D, so this is a minimal
    // description of one Direct2D effect: its id, its properties by index and its inputs.

    namespace wge = winrt::Windows::Graphics::Effects;

    struct Effect : winrt::implements<Effect, wge::IGraphicsEffect, wge::IGraphicsEffectSource,
                                      ABI::Windows::Graphics::Effects::IGraphicsEffectD2D1Interop>
    {
        GUID id{};
        std::vector<winrt::Windows::Foundation::IInspectable> properties; // property values, in the effect's index order
        std::vector<wge::IGraphicsEffectSource> sources;
        winrt::hstring name;

        winrt::hstring Name() { return name; }
        void Name(winrt::hstring const &value) { name = value; }

        HRESULT __stdcall GetEffectId(GUID *out) noexcept override
        {
            *out = id;
            return S_OK;
        }
        HRESULT __stdcall GetNamedPropertyMapping(LPCWSTR, UINT *, ABI::Windows::Graphics::Effects::GRAPHICS_EFFECT_PROPERTY_MAPPING *) noexcept override
        {
            return E_INVALIDARG; // nothing is animated by name
        }
        HRESULT __stdcall GetPropertyCount(UINT *count) noexcept override
        {
            *count = UINT(properties.size());
            return S_OK;
        }
        HRESULT __stdcall GetProperty(UINT index, ABI::Windows::Foundation::IPropertyValue **value) noexcept override
        {
            if (index >= properties.size())
                return E_BOUNDS;
            properties[index].as<ABI::Windows::Foundation::IPropertyValue>().copy_to(value);
            return S_OK;
        }
        HRESULT __stdcall GetSource(UINT index, ABI::Windows::Graphics::Effects::IGraphicsEffectSource **source) noexcept override
        {
            if (index >= sources.size())
                return E_BOUNDS;
            sources[index].as<ABI::Windows::Graphics::Effects::IGraphicsEffectSource>().copy_to(source);
            return S_OK;
        }
        HRESULT __stdcall GetSourceCount(UINT *count) noexcept override
        {
            *count = UINT(sources.size());
            return S_OK;
        }
    };

    // A plain colour (straight alpha).
    wge::IGraphicsEffect FloodEffect(D2D1_COLOR_F color)
    {
        auto effect = winrt::make_self<Effect>();
        effect->id = CLSID_D2D1Flood;
        float value[] = {color.r, color.g, color.b, color.a};
        effect->properties.push_back(winrt::Windows::Foundation::PropertyValue::CreateSingleArray(value));
        return effect.as<wge::IGraphicsEffect>();
    }

    wge::IGraphicsEffect BlendEffect(D2D1_BLEND_MODE mode, wge::IGraphicsEffectSource const &background,
                                     wge::IGraphicsEffectSource const &foreground)
    {
        auto effect = winrt::make_self<Effect>();
        effect->id = CLSID_D2D1Blend;
        effect->properties.push_back(winrt::Windows::Foundation::PropertyValue::CreateUInt32(mode));
        effect->sources = {background, foreground};
        return effect.as<wge::IGraphicsEffect>();
    }

    // Windows' acrylic, built the way WinUI's AcrylicBrush builds it. The blurred desktop keeps its
    // hue but takes its brightness from the tint, so the body stays as light over a dark window as it
    // is dark over a bright wallpaper: that is what keeps the bands apart from the middle on any
    // background. A touch of the tint's colour goes on top. The more see-through the user set the
    // menu, the more of the desktop's own brightness is let through.
    // Taking a light colour's brightness down keeps its saturation, so a pale pink became a deep red
    // (the user saw it: "aşırı sature" as the menu was made less see-through). So the grey tint grows
    // with the darkening: WinUI's 15% at the most see-through, up to 50% (Windows 11's own dark
    // acrylic, the Start menu's) where the luminosity tops out (27%). Under that, the slider did nothing
    // (the user saw it: "below 30% the colours stay put"); there the grey goes on up to all of it at 0%
    // ("Kapalı"), a plain dark body.
    //
    // The tint is Windows' dark acrylic grey, or (config `tint`, the user's wish, 17.10.2026) a dark
    // shade of Windows' accent colour or of a colour picked, at the grey's brightness: the menu's brightness, and so its
    // text's contrast, stay as they are; only its hue and saturation take the accent. Windows' own
    // "show accent colour on Start" was no use to sync with: it can only be on in Windows' dark mode,
    // and the user's is light. What is opaque (the settings panel, the flyouts' fill at 0%, --render)
    // and the search box take the tint too (TintedGrey).
    constexpr uint32_t GreyTint = 0xFF2C2C2C; // Windows' dark acrylic tint
    uint32_t g_tint = GreyTint;

    float Luminance(uint32_t c) { return (0.3f * (c >> 16 & 0xFF) + 0.59f * (c >> 8 & 0xFF) + 0.11f * (c & 0xFF)) / 255; }

    // The colour's hue and saturation at another brightness, as the blend modes do it (clipped into range).
    uint32_t AtLuminance(uint32_t c, float lum)
    {
        float rgb[3] = {(c >> 16 & 0xFF) / 255.f, (c >> 8 & 0xFF) / 255.f, (c & 0xFF) / 255.f};
        float d = lum - Luminance(c);
        for (float &v : rgb)
            v += d;
        float l = lum, lo = std::min({rgb[0], rgb[1], rgb[2]}), hi = std::max({rgb[0], rgb[1], rgb[2]});
        for (float &v : rgb)
        {
            if (lo < 0)
                v = l + (v - l) * l / (l - lo);
            if (hi > 1)
                v = l + (v - l) * (1 - l) / (hi - l);
        }
        auto byte = [](float v)
        { return uint32_t(std::lround(std::clamp(v, 0.f, 1.f) * 255)); };
        return 0xFF000000 | byte(rgb[0]) << 16 | byte(rgb[1]) << 8 | byte(rgb[2]);
    }

    // One of Windows' accent palette, lightest (0) to darkest; 0 if there is none.
    uint32_t AccentShade(int k)
    {
        BYTE palette[32];
        DWORD size = sizeof palette;
        if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Accent", L"AccentPalette",
                         RRF_RT_REG_BINARY, nullptr, palette, &size) != ERROR_SUCCESS ||
            size < DWORD(4 * k + 3))
            return 0;
        return 0xFF000000 | palette[4 * k] << 16 | palette[4 * k + 1] << 8 | palette[4 * k + 2];
    }

    // The colours to pick from (config `tint`; the user's choice, 17.10.2026: grey, Windows' accent, or
    // one of these, as Windows' own accent picker shows a few). Each tints as a dark shade of itself.
    struct TintChoice
    {
        wchar_t const *name;
        uint32_t color;
    }; // color 0: grey; 1: Windows' accent
    constexpr TintChoice TintChoices[] = {
        {L"Gri", 0},
        {L"Windows", 1},
        {L"Mavi", 0xFF0078D4},
        {L"Lacivert", 0xFF4050B5},
        {L"Mor", 0xFF8E44C9},
        {L"Pembe", 0xFFE3008C},
        {L"Kırmızı", 0xFFE81123},
        {L"Turuncu", 0xFFF7630C},
        {L"Yeşil", 0xFF10893E},
        {L"Camgöbeği", 0xFF00B7C3},
    };
    std::wstring TintSetting(int k)
    {
        uint32_t c = TintChoices[k].color;
        if (c <= 1)
            return c ? L"accent" : L"";
        wchar_t hex[8];
        swprintf_s(hex, L"#%06X", c & 0xFFFFFF);
        return hex;
    }
    int TintIndex()
    {
        for (int k = 0; k < int(std::size(TintChoices)); k++)
            if (_wcsicmp(TintSetting(k).c_str(), g_config.tint.c_str()) == 0)
                return k;
        return -1; // a colour put in the file by hand: used, but none of these is lit
    }

    // The tint as set: Windows' grey; or the colour (the accent palette's "dark 2", Windows' Start
    // shades are round it) at the grey's brightness.
    uint32_t ReadTint()
    {
        uint32_t color = 0;
        if (g_config.tint == L"accent")
            color = AccentShade(5);
        else if (g_config.tint.size() == 7 && g_config.tint[0] == L'#')
            color = 0xFF000000 | wcstoul(g_config.tint.c_str() + 1, nullptr, 16);
        if (!color)
            return GreyTint;
        // Half way to the grey: at the grey's brightness a colour keeps all its saturation, and a full
        // red made the opaque panel (and the menu at 0%) a blazing dark red; Windows' own dark shades
        // are calmer. Both are as bright, so the mix is too.
        uint32_t full = AtLuminance(color, Luminance(GreyTint)), out = 0xFF000000;
        for (int shift : {16, 8, 0})
            out |= uint32_t(((full >> shift & 0xFF) + (GreyTint >> shift & 0xFF) + 1) / 2) << shift;
        return out;
    }

    // A grey of the body's (the search box's fill, the opaque body), in the tint's hue; `amount` of the
    // way from the plain grey.
    uint32_t TintedGrey(uint8_t grey, float amount = 1)
    {
        uint32_t tinted = AtLuminance(g_tint, grey / 255.f), out = 0xFF000000;
        for (int shift : {16, 8, 0})
            out |= uint32_t(std::lround(grey + amount * (int(tinted >> shift & 0xFF) - grey))) << shift;
        return out;
    }

    // The search box (and a section's name box): a third of the tint (the user's wish: in the full
    // tint it caught the eye, over a body that is see-through and so takes little of it).
    uint32_t SearchFill() { return 0xAA000000 | (TintedGrey(0x1F, 0.33f) & 0xFFFFFF); }

    wuc::CompositionBrush AcrylicBrush(double transparency)
    {
        uint32_t tint = g_tint;
        constexpr float lumMin = 0.26f, lumMax = 0.99f, knee = 1.26f - lumMax;
        float t = float(transparency);
        float luminosity = std::clamp(1.26f - t, lumMin, lumMax); // 0.96 at the default 30%
        float tintOpacity = t >= knee ? 0.15f + 0.35f * (luminosity - lumMin) / (lumMax - lumMin)
                                      : 0.5f + 0.5f * (knee - t) / knee;
        // The compositor has the two modes' names swapped; WinUI asks for "colour" here too.
        auto toned = BlendEffect(D2D1_BLEND_MODE_COLOR, wuc::CompositionEffectSourceParameter(L"Backdrop"),
                                 FloodEffect(Argb(tint, luminosity)));
        auto tinted = BlendEffect(D2D1_BLEND_MODE_LUMINOSITY, toned, FloodEffect(Argb(tint, tintOpacity)));
        auto brush = g_compositor.CreateEffectFactory(tinted).CreateBrush();
        brush.SetSourceParameter(L"Backdrop", g_compositor.CreateHostBackdropBrush());
        return brush;
    }

    // ---- Edge fades ----
    //
    // What scrolls fades out softly toward the scrolling view's ends (SplitTop, SplitBottom), as long
    // as there is more beyond that end. Each column is a layer visual whose flattened content is
    // multiplied by the alpha of a fixed vertical gradient (Direct2D's composite, destination-in); the
    // content scrolls under it, and an end's fade comes and goes by its stop's colour, all played by
    // the compositor. The "+" row, the column line and its handle are not in the layers, so stay solid.

    constexpr float FadeH = 24; // the fade's length at each end
    constexpr auto FadeTime = 150ms;

    struct Fade
    {
        wuc::CompositionLinearGradientBrush mask{nullptr};
        wuc::CompositionColorGradientStop stops[4]{nullptr, nullptr, nullptr, nullptr}; // the top's, the solid span's two, the bottom's
        bool top = false, bottom = false;                                               // each end fading now
    };
    Fade g_appsFade, g_folderFade;

    constexpr winrt::Windows::UI::Color FadeSolid{255, 255, 255, 255}, FadeClear{0, 255, 255, 255};

    // The layer's effect: its content, kept as much as the mask is opaque.
    wuc::CompositionEffectBrush FadeBrush(Fade &fade)
    {
        fade.mask = g_compositor.CreateLinearGradientBrush();
        fade.mask.MappingMode(wuc::CompositionMappingMode::Absolute);
        for (int i = 0; i < 4; i++)
        {
            fade.stops[i] = g_compositor.CreateColorGradientStop(i / 3.f, FadeSolid);
            fade.mask.ColorStops().Append(fade.stops[i]);
        }
        auto effect = winrt::make_self<Effect>();
        effect->id = CLSID_D2D1Composite;
        effect->properties.push_back(winrt::Windows::Foundation::PropertyValue::CreateUInt32(D2D1_COMPOSITE_MODE_DESTINATION_IN));
        effect->sources = {wuc::CompositionEffectSourceParameter(L"content"), wuc::CompositionEffectSourceParameter(L"mask")};
        auto brush = g_compositor.CreateEffectFactory(effect.as<wge::IGraphicsEffect>()).CreateBrush();
        brush.SetSourceParameter(L"mask", fade.mask); // "content" is left to the layer: its children
        return brush;
    }

    // Puts the fade between `from` and `to` (DIPs, in the layer), each end fading or not.
    void SetFade(Fade &fade, float from, float to, bool top, bool bottom)
    {
        if (!fade.mask)
            return;
        fade.mask.StartPoint({0, Px(from)});
        fade.mask.EndPoint({0, Px(to)});
        float edge = std::min(0.45f, FadeH / std::max(1.f, to - from));
        fade.stops[1].Offset(edge);
        fade.stops[2].Offset(1 - edge);
        fade.stops[3].Offset(1);
        for (auto [stop, on, was] : {std::tuple{fade.stops[0], top, &fade.top}, std::tuple{fade.stops[3], bottom, &fade.bottom}})
        {
            if (on == *was)
                continue;
            *was = on;
            auto animation = g_compositor.CreateColorKeyFrameAnimation();
            animation.InsertKeyFrame(1.f, on ? FadeClear : FadeSolid);
            animation.Duration(FadeTime);
            stop.StartAnimation(L"Color", animation);
        }
    }

    // Rebuilds a popup's acrylic when the transparency setting has changed since it was built.
    void UpdateAcrylic(PopupWindow &popup)
    {
        if (popup.acrylicFor == g_config.transparency && popup.tintFor == g_tint)
            return;
        popup.acrylicFor = g_config.transparency;
        popup.tintFor = g_tint;
        if (popup.opaque)
        {
            uint32_t c = TintedGrey(0x2C);
            popup.backdrop.Brush(g_compositor.CreateColorBrush(winrt::Windows::UI::Color{255, uint8_t(c >> 16), uint8_t(c >> 8), uint8_t(c)}));
            return;
        }
        try
        {
            popup.backdrop.Brush(AcrylicBrush(g_config.transparency));
        }
        catch (winrt::hresult_error const &error)
        {
            // Without the effect, the plain blur still works.
            Log(L"acrylic failed: " + std::to_wstring(error.code()) + L" " + std::wstring(error.message()));
            popup.backdrop.Brush(g_compositor.CreateHostBackdropBrush());
        }
    }

    // ---- Popup windows ----
    //
    // The body's rounded corners and its shadow are ours, not the window manager's: the window is
    // square, and a square popup gets no system shadow. That way both slide and fade with the
    // content; the system shadow stayed where the window was while the content slid away.

    // Creates the window (hidden) and its visual tree: the shadow, then the body with the blurred
    // desktop at its bottom. The caller adds its own layers to body on top of that.
    HRESULT CreatePopup(PopupWindow &popup, wchar_t const *className, wchar_t const *title, WNDPROC proc,
                        DWORD exStyle, HWND owner, HINSTANCE instance)
    {
        WNDCLASSEXW wc{sizeof wc};
        wc.lpfnWndProc = proc;
        wc.hInstance = instance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = className;
        RegisterClassExW(&wc);
        // No redirection surface: the visual tree is the only thing drawn, so its transparent pixels
        // (the shadow margin) really are transparent.
        popup.hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP | exStyle, className, title, WS_POPUP,
                                     0, 0, 0, 0, owner, nullptr, instance, nullptr);
        if (!popup.hwnd)
            return HRESULT_FROM_WIN32(GetLastError());

        DWM_WINDOW_CORNER_PREFERENCE square = DWMWCP_DONOTROUND;
        DwmSetWindowAttribute(popup.hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &square, sizeof square);
        COLORREF none = DWMWA_COLOR_NONE; // no system-drawn edge line either
        DwmSetWindowAttribute(popup.hwnd, DWMWA_BORDER_COLOR, &none, sizeof none);
        // Lets the visual tree use the host backdrop brush (the blurred desktop behind the window).
        BOOL backdrop = TRUE;
        DwmSetWindowAttribute(popup.hwnd, DWMWA_USE_HOSTBACKDROPBRUSH, &backdrop, sizeof backdrop);

        auto desktop = g_compositor.as<ABI::Windows::UI::Composition::Desktop::ICompositorDesktopInterop>();
        HRESULT hr = desktop->CreateDesktopWindowTarget(popup.hwnd, TRUE,
                                                        reinterpret_cast<ABI::Windows::UI::Composition::Desktop::IDesktopWindowTarget **>(winrt::put_abi(popup.target)));
        if (FAILED(hr))
            return hr;

        // Blurring inside the tree (rather than with a window-wide blur) lets the blur slide with the body.
        popup.root = g_compositor.CreateContainerVisual();
        popup.shadow = g_compositor.CreateSpriteVisual();
        popup.body = g_compositor.CreateContainerVisual();
        popup.backdrop = g_compositor.CreateSpriteVisual();
        UpdateAcrylic(popup);
        popup.shape = g_compositor.CreateRoundedRectangleGeometry();
        popup.body.Clip(g_compositor.CreateGeometricClip(popup.shape));
        popup.backShape = g_compositor.CreateRoundedRectangleGeometry();
        popup.backdrop.Clip(g_compositor.CreateGeometricClip(popup.backShape));
        popup.root.Children().InsertAtTop(popup.shadow);
        popup.root.Children().InsertAtTop(popup.backdrop);
        popup.root.Children().InsertAtTop(popup.body);
        popup.target.Root(popup.root);
        return S_OK;
    }

    // Gives the body its size (DIPs) and puts its top left at a screen point (pixels); the window
    // goes around it, or, given `area` (screen pixels, holding the body), around that: then the body
    // can change size and place inside the window with no change to the window. The caller sizes its
    // own layers in body.
    void PlacePopup(PopupWindow &popup, POINT topLeft, float width, float height, HWND insertAfter, RECT const *area = nullptr)
    {
        popup.width = width;
        popup.height = height;
        int w = int(Px(width)), h = int(Px(height)), margin = int(Px(ShadowMargin));
        RECT window = area ? *area : RECT{topLeft.x, topLeft.y, topLeft.x + w, topLeft.y + h};
        popup.inset = {topLeft.x - window.left, topLeft.y - window.top};
        UINT flags = SWP_NOACTIVATE | (insertAfter ? 0 : SWP_NOZORDER);
        RECT now;
        GetWindowRect(popup.hwnd, &now);
        RECT wanted{window.left - margin, window.top - margin, window.right + margin, window.bottom + margin};
        if (!EqualRect(&now, &wanted) || insertAfter)
            SetWindowPos(popup.hwnd, insertAfter, wanted.left, wanted.top, wanted.right - wanted.left, wanted.bottom - wanted.top, flags);

        winrt::Windows::Foundation::Numerics::float2 size{float(w), float(h)};
        popup.root.Size({float(wanted.right - wanted.left), float(wanted.bottom - wanted.top)});
        for (auto visual : {popup.shadow.as<wuc::Visual>(), popup.body.as<wuc::Visual>()})
        {
            visual.Size(size);
            visual.Offset({float(margin + popup.inset.x), float(margin + popup.inset.y), 0});
        }
        // The blur, beside the body rather than in it: it spans the whole window and never moves in it,
        // only its clip (the body's shape) does. Within an `area`, a new body size then neither resizes
        // nor moves it. Resized, the host backdrop went on showing the blur from before, frozen; held
        // still inside a moving body, it went on sampling the desktop where the body had been, shifted
        // by the body's move until the menu was clicked (the user saw both while choosing the menu's size).
        popup.backdrop.Size({float(window.right - window.left), float(window.bottom - window.top)});
        popup.backdrop.Offset({float(margin), float(margin), 0});
        for (auto shape : {popup.shape, popup.backShape})
        {
            shape.Size(size);
            shape.CornerRadius({Px(8), Px(8)});
        }
        popup.backShape.Offset({float(popup.inset.x), float(popup.inset.y)});
        if (popup.shadowDpi != g_dpi)
        {
            CastShadow(popup.shadow);
            popup.shadowDpi = g_dpi;
        }
    }

    // Shows the window and slides the body in from `from` DIPs away (below, for the menu rising from
    // the taskbar) while it fades in: fast at first, settling slowly. The window's edge cuts off what
    // is still outside it, so the body seems to come out from under that edge.
    void OpenPopup(PopupWindow &popup, D2D1_POINT_2F from, bool activate)
    {
        popup.generation++;
        // Start from the closed pose so the first frame is not the fully open body.
        popup.root.StopAnimation(L"Offset.X");
        popup.root.StopAnimation(L"Offset.Y");
        popup.root.StopAnimation(L"Opacity");
        popup.root.Offset({Px(from.x), Px(from.y), 0});
        popup.root.Opacity(0);
        ShowWindow(popup.hwnd, activate ? SW_SHOW : SW_SHOWNOACTIVATE);
        Animate(popup.root, L"Offset.X", 0, OpenTime, Bezier(0.1f, 0.9f, 0.2f, 1.f));
        Animate(popup.root, L"Offset.Y", 0, OpenTime, Bezier(0.1f, 0.9f, 0.2f, 1.f));
        Animate(popup.root, L"Opacity", 1, FadeInTime, g_compositor.CreateLinearEasingFunction());
    }

    // Slides the body `to` DIPs away while it fades out, then hides the window and calls `hidden`,
    // unless the window was reopened in the meantime: that newer animation owns it then.
    void ClosePopup(PopupWindow &popup, D2D1_POINT_2F to, std::function<void()> hidden = {})
    {
        int generation = ++popup.generation;
        popup.closing = g_compositor.CreateScopedBatch(wuc::CompositionBatchTypes::Animation);
        Animate(popup.root, L"Offset.X", Px(to.x), CloseTime, Bezier(0.32f, 0, 0.67f, 0));
        Animate(popup.root, L"Offset.Y", Px(to.y), CloseTime, Bezier(0.32f, 0, 0.67f, 0));
        Animate(popup.root, L"Opacity", 0, CloseTime, g_compositor.CreateLinearEasingFunction());
        popup.closing.End();
        popup.closing.Completed([&popup, generation, hidden = std::move(hidden)](auto &&, auto &&)
                                {
        if (generation != popup.generation) return;
        ShowWindow(popup.hwnd, SW_HIDE);
        if (hidden) hidden(); });
    }

    // ---- Hover highlights ----

    // The account and bar buttons' highlights are circles (matching the picture, the power button's
    // ring, and so the others beside it: user's wish), the search box's back arrow's a pill (in the
    // pill of the box); the rest are rounded boxes.
    float HoverRadius(int key, D2D1_RECT_F r)
    {
        if (key == KeySearchBack)
            return (r.bottom - r.top) / 2;
        return key == KeyAccount || (key >= KeyBar && key < KeyBar + BarCount) ? (r.right - r.left) / 2 : 6;
    }

    wuc::CompositionSurfaceBrush ShapeFor(D2D1_RECT_F r, float radius = 6)
    {
        float w = r.right - r.left, h = r.bottom - r.top;
        uint64_t id = (uint64_t(w * 100) << 40) | (uint64_t(h * 100) << 16) | uint64_t(radius * 100);
        if (auto it = g_shapes.find(id); it != g_shapes.end())
            return it->second;
        auto surface = CreateSurface(w, h);
        DrawSurface(surface, w, h, [&]
                    { g_dc->FillRoundedRectangle(D2D1::RoundedRect({0, 0, w, h}, radius, radius), Brush(0x17FFFFFF)); });
        return g_shapes.emplace(id, g_compositor.CreateSurfaceBrush(surface)).first->second;
    }

    void PlaceHover(int key, wuc::SpriteVisual const &layer)
    {
        auto r = KeyRect(key);
        layer.Offset({Px(r.left), Px(r.top), 0});
    }

    // What scrolls with the apps side: the pins, the list rows, the list's title-row buttons.
    bool IsAppsKey(int key)
    {
        return (key >= KeyPinned && key < KeyFolder) || key >= KeyResult || (key >= KeySort && key <= KeyAllApps && !g_allApps);
    }

    wuc::ContainerVisual HoverContainer(int key)
    {
        if (key >= KeyFolder && key < KeyFolderAdd)
            return g_folderHovers; // fading with the folders
        return IsAppsKey(key) ? g_appsHovers : IsContentKey(key) ? g_contentHovers
                                                                 : g_barHovers;
    }

    wuc::SpriteVisual HoverLayerFor(int key)
    {
        if (auto it = g_hoverLayers.find(key); it != g_hoverLayers.end())
            return it->second;
        auto r = KeyRect(key);
        auto layer = g_compositor.CreateSpriteVisual();
        layer.Brush(ShapeFor(r, HoverRadius(key, r)));
        layer.Size({Px(r.right - r.left), Px(r.bottom - r.top)});
        layer.Opacity(0);
        PlaceHover(key, layer);
        HoverContainer(key).Children().InsertAtTop(layer);
        g_hoverLayers.emplace(key, layer);
        return layer;
    }

    void FadeTo(int key, float to)
    {
        if (key < KeyPinned || key == KeySplitter || key == KeyScrollbar || key == KeyFolderScrollbar || IsSectionGrip(key))
            return; // these draw their own highlight
        if (auto it = g_highlights.find(key); it != g_highlights.end() && it->second == to)
            return;
        g_highlights[key] = to;
        if (!g_compositor)
            return; // --render draws highlights itself
        // An ease-out like the WPF version's: quick to react, settling gently.
        Animate(HoverLayerFor(key), L"Opacity", to, HoverTime, Bezier(0.5f, 1.f, 0.89f, 1.f));
    }

    void DropHovers(bool contentOnly)
    {
        for (auto it = g_hoverLayers.begin(); it != g_hoverLayers.end();)
        {
            if (contentOnly && !IsContentKey(it->first))
            {
                ++it;
                continue;
            }
            // From the container it is in, not the one its key would get now: the every-apps button's
            // moves to the fixed layer as the list opens (IsAppsKey), and removing it from there threw.
            if (auto parent = it->second.Parent())
                parent.Children().Remove(it->second);
            it = g_hoverLayers.erase(it);
        }
        std::erase_if(g_highlights, [contentOnly](auto const &entry)
                      { return !contentOnly || IsContentKey(entry.first); });
        if (!contentOnly || IsContentKey(g_keyFocus))
            g_keyFocus = KeyNone; // its key may point at another now
    }

    // ---- Text fields ----

    ComPtr<IDWriteTextLayout> FieldLayout(TextField const &f)
    {
        ComPtr<IDWriteTextLayout> layout;
        g_dwrite->CreateTextLayout(f.text.c_str(), UINT32(f.text.size()), f.format(), 100000, f.area.bottom - f.area.top, &layout);
        return layout;
    }

    float CaretX(TextField const &f, IDWriteTextLayout *layout, size_t position)
    {
        if (!layout || f.text.empty())
            return 0;
        float x, y;
        DWRITE_HIT_TEST_METRICS m;
        layout->HitTestTextPosition(UINT32(position), FALSE, &x, &y, &m);
        return x;
    }

    size_t CaretFromPoint(TextField &f, float x)
    {
        if (f.text.empty())
            return 0;
        f.place();
        auto layout = FieldLayout(f);
        BOOL trailing, inside;
        DWRITE_HIT_TEST_METRICS m;
        layout->HitTestPoint(x - f.area.left + f.scroll, (f.area.bottom - f.area.top) / 2, &trailing, &inside, &m);
        return std::min(f.text.size(), size_t(m.textPosition + (trailing ? m.length : 0)));
    }

    void KeepCaretVisible(TextField &f)
    {
        f.place(); // the area may have moved, or narrowed
        auto layout = FieldLayout(f);
        float x = CaretX(f, layout.Get(), f.caret), width = f.area.right - f.area.left;
        if (x - f.scroll > width)
            f.scroll = x - width;
        if (x - f.scroll < 0)
            f.scroll = x;
    }

    // The caret and the selection span 19 rows in the middle of the field's area: rows 7 to 26 of the
    // search box, as in Windows.
    constexpr float CaretH = 19;
    float CaretTop(TextField const &f) { return f.area.top + std::round((f.area.bottom - f.area.top - CaretH) / 2); }

    // The caret is a visual of its own and the compositor blinks it, so blinking draws nothing; only
    // moving it does. Restarting shows it at once, as text boxes do after every keystroke.
    void RestartCaretBlink()
    {
        Invalidate(); // for its new place
        if (!g_caretVisual)
            return;
        g_caretVisual.StopAnimation(L"Opacity");
        g_caretVisual.Opacity(1);
        UINT blink = GetCaretBlinkTime();
        if (!g_visible || blink == INFINITE || blink == 0)
            return;
        // On for one blink time, off for the next; the near-instant ramps make it a hard switch.
        auto linear = g_compositor.CreateLinearEasingFunction();
        auto animation = g_compositor.CreateScalarKeyFrameAnimation();
        animation.InsertKeyFrame(0.f, 1.f, linear);
        animation.InsertKeyFrame(0.4999f, 1.f, linear);
        animation.InsertKeyFrame(0.5f, 0.f, linear);
        animation.InsertKeyFrame(0.9999f, 0.f, linear);
        animation.InsertKeyFrame(1.f, 1.f, linear);
        animation.Duration(std::chrono::milliseconds(2 * blink));
        animation.IterationBehavior(wuc::AnimationIterationBehavior::Forever);
        g_caretVisual.StartAnimation(L"Opacity", animation);
    }

    // Name starts with the query, then a word starts with it, then anywhere; like the WPF version.
    int Rank(std::wstring const &name, std::wstring const &query)
    {
        int found = 0;
        int i = FindNLSStringEx(LOCALE_NAME_USER_DEFAULT, FIND_FROMSTART | LINGUISTIC_IGNORECASE | LINGUISTIC_IGNOREDIACRITIC,
                                name.c_str(), int(name.size()), query.c_str(), int(query.size()), &found, nullptr, nullptr, 0);
        if (i < 0)
            return -1;
        if (i == 0)
            return 0;
        return IsCharAlphaNumericW(name[i - 1]) ? 2 : 1;
    }

    void UpdateHot();

    // A name's heading in the letter-grouped list: its first letter in capitals, as the user's language
    // writes them (i -> İ in Turkish); "#" for a name that starts with anything else.
    wchar_t HeadingOf(std::wstring const &name)
    {
        if (name.empty() || !IsCharAlphaW(name[0]))
            return L'#';
        wchar_t upper = name[0];
        LCMapStringEx(LOCALE_NAME_USER_DEFAULT, LCMAP_UPPERCASE | LCMAP_LINGUISTIC_CASING, &name[0], 1, &upper, 1, nullptr, nullptr, 0);
        return upper;
    }

    // Every app, in the order the every-apps list is set to (g_config.appsSort). The app list comes
    // sorted by name in the user's language, so A-Z only adds the letter headings, in its order.
    void ListAllApps()
    {
        for (int i = 0; i < int(g_apps.size()); i++)
            // Newest first lists programs only: what has no install date (a folder, a document, a web
            // link) is not one.
            if (g_config.appsSort != 1 || g_apps[i].added)
                g_results.push_back(i);
        if (g_config.appsSort == 1)
            std::stable_sort(g_results.begin(), g_results.end(), [](int a, int b)
                             { return g_apps[a].added > g_apps[b].added; });
        else
        {
            std::vector<int> rows;
            wchar_t heading = 0;
            for (int i : g_results)
            {
                if (wchar_t letter = HeadingOf(g_apps[i].name); letter != heading)
                {
                    heading = letter;
                    rows.push_back(-int(letter) - 1);
                }
                rows.push_back(i);
            }
            g_results = std::move(rows);
        }
    }

    void RevealApps(bool upward);
    bool g_searching = false; // the search results shown, as of the last QueryChanged

    void QueryChanged()
    {
        // Into the search results they unroll upward from the box; out of them the grid unrolls
        // downward, as it comes back from the every-apps list.
        if (SearchView() != g_searching)
        {
            g_searching = SearchView();
            RevealApps(g_searching);
        }
        g_results.clear();
        size_t first = g_query.find_first_not_of(L" \t"), last = g_query.find_last_not_of(L" \t");
        if (first != std::wstring::npos)
        {
            std::wstring query = g_query.substr(first, last - first + 1);
            std::vector<std::pair<int, int>> ranked;
            for (int i = 0; i < int(g_apps.size()); i++)
                if (int r = Rank(g_apps[i].name, query); r >= 0)
                    ranked.push_back({r, i});
            std::stable_sort(ranked.begin(), ranked.end(), [](auto const &a, auto const &b)
                             { return a.first < b.first; });
            for (auto const &[rank, index] : ranked)
                g_results.push_back(index);
        }
        else if (g_allApps)
            ListAllApps();
        g_scroll = ResultsUpward() ? MaxScroll() : 0; // at the list's start: its bottom when it runs upward
        g_selected = 0;
        // Favorite and result keys now point at different things.
        DropHovers(true);
        if (IsContentKey(g_hot) || g_hot == KeySearch)
            g_hot = KeyNone;
        UpdateHot();
        KeepCaretVisible(g_search);
        RestartCaretBlink();
    }

    // Esc, or the box's back arrow: the search goes, back to what was there before it.
    void ClearSearch()
    {
        g_search.text.clear();
        g_search.caret = g_search.anchor = 0;
        g_search.scroll = 0;
        QueryChanged();
    }

    size_t SelStart(TextField const &f) { return std::min(f.caret, f.anchor); }
    size_t SelEnd(TextField const &f) { return std::max(f.caret, f.anchor); }

    void ReplaceSelection(TextField &f, std::wstring_view text)
    {
        size_t start = SelStart(f);
        f.text.replace(start, SelEnd(f) - start, text);
        f.caret = f.anchor = start + text.size();
        f.changed();
    }

    void MoveCaret(TextField &f, size_t position, bool extend)
    {
        f.caret = std::min(position, f.text.size());
        if (!extend)
            f.anchor = f.caret;
        KeepCaretVisible(f);
        RestartCaretBlink();
    }

    size_t PrevChar(TextField const &f, size_t p) { return p >= 2 && IS_LOW_SURROGATE(f.text[p - 1]) ? p - 2 : p ? p - 1
                                                                                                                 : 0; }
    size_t NextChar(TextField const &f, size_t p)
    {
        return p + 1 < f.text.size() && IS_HIGH_SURROGATE(f.text[p]) ? p + 2 : std::min(p + 1, f.text.size());
    }

    size_t PrevWord(TextField const &f, size_t p)
    {
        while (p > 0 && f.text[p - 1] == L' ')
            p--;
        while (p > 0 && f.text[p - 1] != L' ')
            p--;
        return p;
    }

    size_t NextWord(TextField const &f, size_t p)
    {
        while (p < f.text.size() && f.text[p] != L' ')
            p++;
        while (p < f.text.size() && f.text[p] == L' ')
            p++;
        return p;
    }

    void CopySelection(TextField const &f)
    {
        if (SelStart(f) == SelEnd(f) || !OpenClipboard(g_hwnd))
            return;
        std::wstring text = f.text.substr(SelStart(f), SelEnd(f) - SelStart(f));
        EmptyClipboard();
        size_t bytes = (text.size() + 1) * sizeof(wchar_t);
        if (HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes))
        {
            memcpy(GlobalLock(memory), text.c_str(), bytes);
            GlobalUnlock(memory);
            if (!SetClipboardData(CF_UNICODETEXT, memory))
                GlobalFree(memory);
        }
        CloseClipboard();
    }

    void Paste(TextField &f)
    {
        if (!OpenClipboard(g_hwnd))
            return;
        std::wstring text;
        if (HANDLE data = GetClipboardData(CF_UNICODETEXT))
            if (auto p = static_cast<wchar_t const *>(GlobalLock(data)))
            {
                text = p;
                GlobalUnlock(data);
            }
        CloseClipboard();
        std::replace_if(text.begin(), text.end(), [](wchar_t c)
                        { return c == L'\r' || c == L'\n' || c == L'\t'; }, L' ');
        ReplaceSelection(f, text);
    }

    // The editing keys, the same in every field: the caret and the selection, deleting, the clipboard.
    // False for any other key.
    bool EditKey(TextField &f, WPARAM key, bool shift, bool ctrl)
    {
        switch (key)
        {
        case VK_LEFT:
            if (!shift && f.caret != f.anchor)
                MoveCaret(f, SelStart(f), false);
            else
                MoveCaret(f, ctrl ? PrevWord(f, f.caret) : PrevChar(f, f.caret), shift);
            return true;
        case VK_RIGHT:
            if (!shift && f.caret != f.anchor)
                MoveCaret(f, SelEnd(f), false);
            else
                MoveCaret(f, ctrl ? NextWord(f, f.caret) : NextChar(f, f.caret), shift);
            return true;
        case VK_HOME:
            MoveCaret(f, 0, shift);
            return true;
        case VK_END:
            MoveCaret(f, f.text.size(), shift);
            return true;
        case VK_BACK:
            if (f.caret == f.anchor)
                f.anchor = ctrl ? PrevWord(f, f.caret) : PrevChar(f, f.caret);
            if (f.caret != f.anchor)
                ReplaceSelection(f, L"");
            return true;
        case VK_DELETE:
            if (f.caret == f.anchor)
                f.anchor = ctrl ? NextWord(f, f.caret) : NextChar(f, f.caret);
            if (f.caret != f.anchor)
                ReplaceSelection(f, L"");
            return true;
        case 'A':
            if (ctrl)
            {
                f.anchor = 0;
                MoveCaret(f, f.text.size(), true);
            }
            return ctrl;
        case 'C':
            if (ctrl)
                CopySelection(f);
            return ctrl;
        case 'X':
            if (ctrl)
            {
                CopySelection(f);
                ReplaceSelection(f, L"");
            }
            return ctrl;
        case 'V':
            if (ctrl)
                Paste(f);
            return ctrl;
        }
        return false;
    }

    // ---- Painting ----

    ComPtr<ID2D1LinearGradientBrush> Gradient(D2D1_POINT_2F from, D2D1_POINT_2F to, std::initializer_list<D2D1_GRADIENT_STOP> stops)
    {
        ComPtr<ID2D1GradientStopCollection> collection;
        ComPtr<ID2D1LinearGradientBrush> brush;
        g_dc->CreateGradientStopCollection(stops.begin(), UINT32(stops.size()), &collection);
        if (collection)
            g_dc->CreateLinearGradientBrush({from, to}, collection.Get(), &brush);
        return brush;
    }

    // The Windows search magnifier: a dark glass in a ring that runs from green at the top right to
    // blue at the bottom left, and a blue handle. Not in any icon font, so it is drawn here.
    void DrawSearchIcon(D2D1_POINT_2F c)
    {
        constexpr float ring = 6, width = 2;
        auto glass = Gradient({c.x, c.y - ring}, {c.x, c.y + ring},
                              {{0, Argb(0xFF474C4C)}, {1, Argb(0xFF3E4242)}});
        auto rim = Gradient({c.x + 5, c.y - 5}, {c.x - 5, c.y + 5},
                            {{0, Argb(0xFF60DF74)}, {0.25f, Argb(0xFF3AA88A)}, {0.45f, Argb(0xFF1590C0)}, {0.65f, Argb(0xFF0078D4)}, {1, Argb(0xFF0078D4)}});
        if (!glass || !rim)
            return;
        g_dc->FillEllipse(D2D1::Ellipse(c, ring - width / 2, ring - width / 2), glass.Get());
        g_dc->DrawEllipse(D2D1::Ellipse(c, ring, ring), rim.Get(), width);
        ComPtr<ID2D1StrokeStyle> round;
        g_d2d->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND), nullptr, 0, &round);
        // Measured off Windows: a handle a little thicker than the ring, from where it leaves the ring
        // to 7 right of and 8 below the ring's centre.
        g_dc->DrawLine({c.x + 4.5f, c.y + 5.f}, {c.x + 7.2f, c.y + 8.f}, Brush(0xFF0078D4), 2.2f, round.Get());
    }

    void DrawBackArrow(D2D1_POINT_2F c);

    // A field's text and selection, or its faint placeholder while empty; with the caret only for
    // --render (live, the caret is a visual of its own, PlaceCaret), and only in the focused field.
    void DrawField(TextField &f, wchar_t const *placeholder, bool focused)
    {
        f.place();
        float top = CaretTop(f), bottom = top + CaretH;
        g_dc->PushAxisAlignedClip({f.area.left, f.area.top, f.area.right + 1, f.area.bottom}, D2D1_ANTIALIAS_MODE_ALIASED);
        f.caretX = 0;
        if (f.text.empty() && placeholder)
            Text(placeholder, f.format(), f.area, 0xFF999999);
        else
        {
            auto layout = FieldLayout(f);
            if (SelStart(f) != SelEnd(f))
            {
                float x1 = CaretX(f, layout.Get(), SelStart(f)), x2 = CaretX(f, layout.Get(), SelEnd(f));
                g_dc->FillRectangle({f.area.left + x1 - f.scroll, top, f.area.left + x2 - f.scroll, bottom}, Brush(0x6660CDFF));
            }
            if (layout)
                g_dc->DrawTextLayout({f.area.left - f.scroll, f.area.top}, layout.Get(), Brush(0xFFFFFFFF));
            f.caretX = CaretX(f, layout.Get(), f.caret);
        }
        if (!g_compositor && focused)
        {
            float x = std::round(f.area.left + f.caretX - f.scroll);
            g_dc->FillRectangle({x, top, x + 1, bottom}, Brush(0xFFD2D2D2));
        }
        g_dc->PopAxisAlignedClip();
    }

    // Both are drawn at these points from the box's top left: the magnifier as Windows has it, the
    // arrow in the middle of its highlight (SearchBackRect), the box's; a pixel lower than the
    // magnifier's centre (the user saw it sit high in the highlight there).
    constexpr D2D1_POINT_2F MagnifierAt{23, 15}, BackArrowAt{23, SearchBoxH / 2};
    constexpr float SearchIconW = 44; // each icon's own surface: the box's height, to the text

    void DrawSearch()
    {
        // Live, the icons are visuals of their own (SearchIcons); only --render draws them here.
        if (!g_compositor)
        {
            if (g_query.empty())
                DrawSearchIcon({SearchBox.left + MagnifierAt.x, SearchBox.top + MagnifierAt.y});
            else
                DrawBackArrow({SearchBox.left + BackArrowAt.x, SearchBox.top + BackArrowAt.y});
        }

        DrawField(g_search, L"Uygulama ara", !g_renaming);

        // The hint, right-aligned, dimmer than the placeholder; it stays while typing.
        ComPtr<IDWriteTextLayout> hint;
        float hintW = SearchBox.right - 14 - (SearchTextRight + SearchHintGap);
        if (hintW > 0 && SUCCEEDED(g_dwrite->CreateTextLayout(SearchHint, UINT32(wcslen(SearchHint)), g_fmtName.Get(), hintW,
                                                              SearchBox.bottom - SearchBox.top, &hint)))
        {
            hint->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
            g_dc->DrawTextLayout({SearchTextRight + SearchHintGap, SearchBox.top}, hint.Get(), Brush(0xFF6B6B6B),
                                 D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
    }

    // The back arrow in the magnifier's place, 16 across, a head whose strokes meet in a rounded tip;
    // coloured as the magnifier's ring (the user's wish): as thick, and from green at the top right
    // to blue at the bottom left, so the two read as one icon changing.
    void DrawBackArrow(D2D1_POINT_2F c)
    {
        static ComPtr<ID2D1StrokeStyle> round;
        if (!round)
            g_d2d->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                                                 D2D1_LINE_JOIN_ROUND),
                                     nullptr, 0, &round);
        float cx = c.x, cy = c.y;
        ComPtr<ID2D1PathGeometry> arrow;
        ComPtr<ID2D1GeometrySink> sink;
        if (FAILED(g_d2d->CreatePathGeometry(&arrow)) || FAILED(arrow->Open(&sink)))
            return;
        sink->BeginFigure({cx - 2, cy - 6}, D2D1_FIGURE_BEGIN_HOLLOW); // the head, open toward the shaft
        sink->AddLine({cx - 8, cy});
        sink->AddLine({cx - 2, cy + 6});
        sink->EndFigure(D2D1_FIGURE_END_OPEN);
        sink->BeginFigure({cx - 8, cy}, D2D1_FIGURE_BEGIN_HOLLOW); // the shaft
        sink->AddLine({cx + 8, cy});
        sink->EndFigure(D2D1_FIGURE_END_OPEN);
        sink->Close();
        auto rim = Gradient({cx + 8, cy - 6}, {cx - 8, cy + 6},
                            {{0, Argb(0xFF60DF74)}, {0.25f, Argb(0xFF3AA88A)}, {0.45f, Argb(0xFF1590C0)}, {0.65f, Argb(0xFF0078D4)}, {1, Argb(0xFF0078D4)}});
        if (rim)
            g_dc->DrawGeometry(arrow.Get(), rim.Get(), 2, round.Get());
    }

    // The magnifier gives way to the back arrow as typing starts, and comes back as the box empties:
    // the magnifier fades shrinking a little, the arrow fades in sliding a few DIPs from the right
    // (the user's choice of three). A hidden icon rests where its leaving took it (smaller, or to the
    // right), so each comes from there, and a quick type and delete turns around where it is.
    constexpr auto SearchIconTime = 450ms;                   // the user's pick, after trying 180 to 550
    constexpr float MagnifierAway = 0.8f, BackArrowAway = 6; // the hidden magnifier's scale; the hidden arrow's offset

    void PlaceSearchIcons()
    {
        if (!g_searchIcons)
            return;
        g_searchIcons.Offset({Px(SearchBox.left), Px(SearchBox.top), 0});
        int back = g_query.empty() ? 0 : 1;
        if (back == g_searchIconsBack)
            return;
        bool animate = g_visible && g_searchIconsBack >= 0;
        g_searchIconsBack = back;
        float scale = back ? MagnifierAway : 1, x = back ? 0 : Px(BackArrowAway);
        if (animate)
        {
            auto ease = Bezier(0.1f, 0.9f, 0.2f, 1.f);
            Animate(g_magnifierVisual, L"Opacity", 1.f - back, SearchIconTime, ease);
            Animate(g_magnifierVisual, L"Scale.X", scale, SearchIconTime, ease);
            Animate(g_magnifierVisual, L"Scale.Y", scale, SearchIconTime, ease);
            Animate(g_backArrowVisual, L"Opacity", float(back), SearchIconTime, ease);
            Animate(g_backArrowVisual, L"Offset.X", x, SearchIconTime, ease);
            return;
        }
        for (auto property : {L"Opacity", L"Scale.X", L"Scale.Y"})
            g_magnifierVisual.StopAnimation(property);
        for (auto property : {L"Opacity", L"Offset.X"})
            g_backArrowVisual.StopAnimation(property);
        g_magnifierVisual.Opacity(1.f - back);
        g_magnifierVisual.Scale({scale, scale, 1});
        g_backArrowVisual.Opacity(float(back));
        g_backArrowVisual.Offset({x, 0, 0});
    }

    // The icons' surfaces, at the scale now; the scale's centre is the magnifier's.
    void DrawSearchIcons()
    {
        for (bool magnifier : {true, false})
        {
            auto visual = magnifier ? g_magnifierVisual : g_backArrowVisual;
            // They slide and scale by fractions of a pixel, so a margin against the atlas (PaddedBrush).
            visual.Brush(PaddedBrush(SearchIconW, SearchBoxH, [magnifier]
                                     {
            if (magnifier) DrawSearchIcon(MagnifierAt);
            else DrawBackArrow(BackArrowAt); }));
            visual.Size({Px(SearchIconW), Px(SearchBoxH)});
        }
        g_magnifierVisual.CenterPoint({Px(MagnifierAt.x), Px(MagnifierAt.y), 0});
    }

    void PlaceCaret()
    {
        if (!g_caretVisual)
            return;
        // In the focused field: the search box, or a section's name, which is only seen in the
        // scrolling view.
        auto &f = Focused();
        f.place();
        float x = std::round(f.area.left + f.caretX - f.scroll), top = CaretTop(f);
        bool seen = !g_renaming || (top >= SplitTop && top + CaretH <= SplitBottom);
        g_caretVisual.IsVisible(seen && x >= f.area.left && x <= f.area.right);
        g_caretVisual.Offset({Px(x), Px(top), 0});
    }

    void DrawAllAppsButton();

    // A column's title with its number of pins after it, dimmer; or, with none, a line saying so below.
    void DrawColumnTitle(wchar_t const *title, size_t count, wchar_t const *empty, float x, float right, float scroll)
    {
        float y = TitleY - scroll;
        ComPtr<IDWriteTextLayout> layout;
        if (FAILED(g_dwrite->CreateTextLayout(title, UINT32(wcslen(title)), g_fmtTitle.Get(), right - x, 20, &layout)))
            return;
        DWRITE_TEXT_METRICS m{};
        layout->GetMetrics(&m);
        g_dc->DrawTextLayout({x, y}, layout.Get(), Brush(0xFFFFFFFF), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        // Smaller, centred in the title's box: that puts it on about the title's baseline.
        Text(std::to_wstring(count), g_fmtName.Get(), {x + m.width + 8, y, right, y + 20}, 0xFFA6A6A6);
        if (!count)
            Text(empty, g_fmtRow.Get(), {x, GridY - scroll, right, GridY + 30 - scroll}, 0xFFA6A6A6);
    }

    void DrawResults();
    void DrawAllAppsHeader();
    void DrawPinnedGrid();
    void DrawFolderColumn();

    bool ScrollbarHot(int bar = 0) { return g_hot == BarKey(bar) || (g_scrollDragging && g_scrollDragBar == bar); }

    // The thumb in a box ScrollThumbW wide and as tall as it is, centred: 2 wide at rest, 6 when hot.
    constexpr float ScrollThumbW = 6;
    void DrawThumb(float height, bool hot)
    {
        float cx = ScrollThumbW / 2, half = hot ? 3.f : 1.f;
        g_dc->FillRoundedRectangle(D2D1::RoundedRect({cx - half, 0, cx + half, height}, half, half), Brush(hot ? 0xC5FFFFFF : 0x8BFFFFFF));
    }

    // As Windows 11's: a thin line at rest; under the pointer or dragged, a wider thumb on a faint track.
    // Live, the thumb is a layer of its own that glides with the content (PlaceThumb); here only the track.
    void DrawScrollbar(int bar = 0)
    {
        if (!ScrollbarShown(bar))
            return;
        auto track = ScrollTrack(bar), thumb = ScrollThumb(bar);
        float cx = (track.left + track.right) / 2;
        if (ScrollbarHot(bar))
            g_dc->FillRoundedRectangle(D2D1::RoundedRect({cx - 4, track.top, cx + 4, track.bottom}, 4, 4), Brush(0x0FFFFFFF));
        if (g_compositor)
            return;
        D2D1::Matrix3x2F at;
        g_dc->GetTransform(&at);
        g_dc->SetTransform(D2D1::Matrix3x2F::Translation(cx - ScrollThumbW / 2, thumb.top) * at);
        DrawThumb(thumb.bottom - thumb.top, ScrollbarHot(bar));
        g_dc->SetTransform(at);
    }

    // What scrolls on the apps side: the search results, or the pinned apps or the every-apps list.
    // Live, it is drawn into the apps layer (DrawAppsLayer); only --render draws it with the rest.
    void DrawAppsSide()
    {
        if (ShowingList())
            DrawResults();
        else
            DrawPinnedGrid();
    }

    // The two columns; the apps side's content is in its own layer, its scrollbar stays here.
    void DrawHome()
    {
        if (!g_compositor)
        {
            g_dc->PushAxisAlignedClip({0, AppsViewTop(), MenuW, SplitBottom}, D2D1_ANTIALIAS_MODE_ALIASED);
            DrawAppsSide();
            g_dc->PopAxisAlignedClip();
        }
        if (g_allApps)
            DrawAllAppsHeader();
        DrawScrollbar();
        DrawFolderColumn();
    }

    // A pinned app's icon and name in its tile. The icon sits at a fixed height, so icons stay on one
    // line whether a name wraps or not. With `highlightTiles` the tile is a faint box, as the sections
    // are (the user's wish); in a section only when the sections are not shown as boxes themselves
    // (`sectionStyle` 1: boxes in a box were too many). Drawn with the tile, it goes with it when dragged.
    void DrawTile(Tile &tile, D2D1_RECT_F r)
    {
        if (g_config.highlightTiles && (!tile.group || g_config.sectionStyle != 1))
            g_dc->FillRoundedRectangle(D2D1::RoundedRect(r, 6, 6), Brush(0x0AFFFFFF));
        auto d = Dims(tile.group != 0);
        float icon = d.icon, left = std::round((r.left + r.right - icon) / 2);
        float top = r.top + d.iconTop;
        float label = r.top + d.labelTop;
        Icon(tile, {left, top, left + icon, top + icon});
        if (d.names)
            Text(tile.name, g_fmtCell.Get(), {r.left + 4, label, r.right - 4, label + 2 * NameLineH}, 0xFFFFFFFF);
    }

    // A section's name row: its name, as the column's title is (bold, lined up with the icons).
    float SectionNameX(float left)
    {
        auto d = Dims(true);
        return std::round(left + (d.w - d.icon) / 2 - 1);
    }
    // The name's top: its row's.
    float SectionNameY(D2D1_RECT_F r) { return r.top; }

    // `line`: false for a section at the very top with the names off, where its line, above it, would
    // only underline the column's title (the user saw it).
    void DrawSectionHeader(D2D1_RECT_F r, std::wstring_view name, bool line = true)
    {
        float y = SectionNameY(r), left = SectionNameX(r.left);
        if (g_config.sectionNames)
            Text(name, g_fmtTitle.Get(), {left, y, r.right - 8, y + SectionNameH}, 0xFFFFFFFF);
        // With the line chosen (config `sectionStyle` 2): under the name across the section, from the
        // first icon's left edge to the last column's icon's right, as the line between the columns is
        // drawn; with the names off, in the middle of the parting above the section.
        if (g_config.sectionStyle == 2 && line)
        {
            float right = r.right - (left - r.left);
            float line = g_config.sectionNames ? std::round(y + SectionNameH + 5) : std::round(r.top - SectionPad - (TileGap() + 8) / 2);
            g_dc->FillRectangle({left, line, right, line + 1}, Brush(0x29FFFFFF));
        }
    }

    // ---- Renaming a section ----
    //
    // Its name row turns into a text field in place (the search box's look: a faint fill, a light rim),
    // the name where it was, all of it selected: when a section is made (so it is named as it starts,
    // as Explorer does a new folder), and from "Yeniden adlandır" on its name's right-click menu. The
    // user's choices, 16.10.2026: no click on the name. Enter, or a click or a wheel anywhere else,
    // keeps the name (an empty one keeps the old name: every section has one); Esc, the old one.
    // Meanwhile the keys are the field's, not the search's.
    constexpr float NameBoxPad = 12, NameBoxMaxW = 320;
    void SaveConfig();

    D2D1_RECT_F SectionHeaderRect(int section)
    {
        for (auto const &[s, header] : PinLayout().headers)
            if (s == section)
                return Snapped(header, g_scroll);
        return {};
    }

    // The name's text, as DrawSectionHeader puts it; and the box around it, as tall as the search box.
    D2D1_RECT_F NameTextRect(D2D1_RECT_F header)
    {
        float x = SectionNameX(header.left), y = SectionNameY(header);
        return {x, y, std::min(header.right - 8, x - NameBoxPad + NameBoxMaxW) - NameBoxPad, y + SectionNameH};
    }

    D2D1_RECT_F NameBoxRect()
    {
        if (!g_renaming)
            return {};
        auto text = NameTextRect(SectionHeaderRect(g_renaming));
        float top = std::round((text.top + text.bottom - SearchBoxH) / 2);
        return {text.left - NameBoxPad, top, text.right + NameBoxPad, top + SearchBoxH};
    }

    void DrawNameBox()
    {
        auto box = NameBoxRect();
        g_dc->FillRoundedRectangle(D2D1::RoundedRect(box, 16, 16), Brush(SearchFill()));
        D2D1_RECT_F rim{box.left + 0.5f, box.top + 0.5f, box.right - 0.5f, box.bottom - 0.5f};
        g_dc->DrawRoundedRectangle(D2D1::RoundedRect(rim, 15.5f, 15.5f), Brush(0x14FFFFFF), 1);
        DrawField(g_nameField, nullptr, true);
    }

    // The section's name row under `p`, on the pinned grid; 0 for none.
    // Its name row, or anywhere else in its box but a tile (the tiles have their own menu): so a
    // section has its menu with its names off too.
    int SectionAt(D2D1_POINT_2F p)
    {
        if (ShowingList() || p.y < SplitTop || p.y >= SplitBottom || InFolderColumn(p))
            return 0;
        for (auto const &[section, header] : PinLayout().headers)
            if (Inside(Snapped(header, g_scroll), p))
                return section;
        for (auto const &[section, box] : PinLayout().regions)
            if (section && Inside(Snapped(box, g_scroll), p))
                return section;
        return 0;
    }

    void BeginRename(int section)
    {
        if (ShowingList() || !g_config.sectionNames)
            return; // with the names off there is none to edit
        auto header = SectionHeaderRect(section);
        if (header.bottom <= header.top)
            return;
        // Into view, whole.
        if (header.top < SplitTop)
            g_scroll -= SplitTop - header.top;
        else if (header.bottom > SplitBottom)
            g_scroll += header.bottom - SplitBottom;
        g_scroll = std::round(std::clamp(g_scroll, 0.f, MaxScroll()));
        g_renaming = section;
        g_nameField.text = SectionName(section);
        g_nameField.anchor = 0;
        g_nameField.caret = g_nameField.text.size();
        g_nameField.scroll = 0;
        KeepCaretVisible(g_nameField);
        RestartCaretBlink();
        Invalidate();
    }

    void EndRename(bool keep)
    {
        if (!g_renaming)
            return;
        int section = g_renaming;
        g_renaming = 0;
        g_selecting = false;
        if (keep)
        {
            auto &text = g_nameField.text;
            size_t first = text.find_first_not_of(L" \t"), last = text.find_last_not_of(L" \t");
            // Nothing but spaces: the name stays as it was (every section has one).
            if (first != std::wstring::npos)
            {
                std::wstring name = text.substr(first, last - first + 1);
                auto group = std::find_if(g_config.pinGroups.begin(), g_config.pinGroups.end(), [&](auto const &g)
                                          { return g.id == section; });
                if (group == g_config.pinGroups.end())
                    g_config.pinGroups.push_back({section, name});
                else
                    group->name = name;
                SaveConfig();
            }
        }
        KeepCaretVisible(g_search);
        RestartCaretBlink();
        Invalidate();
    }

    // What each field's text is set in and where it is, and what follows an edit.
    void SetUpFields()
    {
        g_search.format = []
        { return g_fmtSearch.Get(); };
        g_search.place = []
        {
            PlaceSearchBox();
            g_search.area = {SearchTextLeft, SearchBox.top, SearchTextRight, SearchBox.bottom};
        };
        g_search.changed = QueryChanged;
        g_nameField.format = []
        { return g_fmtTitle.Get(); };
        g_nameField.place = []
        {
            if (g_renaming)
                g_nameField.area = NameTextRect(SectionHeaderRect(g_renaming));
        };
        g_nameField.changed = []
        {
            KeepCaretVisible(g_nameField);
            RestartCaretBlink();
            Invalidate();
        };
    }

    // "+ Yeni bölüm", while dragging: where letting go starts a section. Faint, as a drop target; with
    // the app over it, darker, as a button pressed. The sections are shown the same way meanwhile,
    // without the words (DragRegion): each one's box from LayOutSections, round its name and tiles.
    constexpr float RegionRadius = 8;
    constexpr uint32_t RegionFill = 0x0AFFFFFF;

    void DrawNewZone(D2D1_RECT_F r, bool pressed = false)
    {
        // A fill alone, no rim line (the user's wish).
        g_dc->FillRoundedRectangle(D2D1::RoundedRect(r, RegionRadius, RegionRadius), Brush(pressed ? 0x33000000 : RegionFill));
        ComPtr<IDWriteTextLayout> layout;
        std::wstring text = std::wstring(L"+  ") + NewSectionName;
        if (SUCCEEDED(g_dwrite->CreateTextLayout(text.c_str(), UINT32(text.size()), g_fmtRow.Get(), r.right - r.left, r.bottom - r.top, &layout)))
        {
            layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            g_dc->DrawTextLayout({r.left, r.top}, layout.Get(), Brush(0xFFC5C5C5));
        }
    }

    // A section's grip: a pill as the column line's handle, with six dots.
    void DrawSectionGrip(D2D1_RECT_F r, bool hot)
    {
        float radius = SectionGripH / 2;
        g_dc->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), Brush(hot ? 0xFF555555 : 0xFF454545));
        D2D1_RECT_F edge{r.left + 0.5f, r.top + 0.5f, r.right - 0.5f, r.bottom - 0.5f};
        g_dc->DrawRoundedRectangle(D2D1::RoundedRect(edge, radius - 0.5f, radius - 0.5f), Brush(0x17FFFFFF), 1);
        float cx = std::round((r.left + r.right) / 2), cy = std::round((r.top + r.bottom) / 2);
        for (float dx : {-5.f, 0.f, 5.f})
            for (float dy : {-2.5f, 2.5f})
                g_dc->FillEllipse(D2D1::Ellipse({cx + dx, cy + dy}, 1.3f, 1.3f), Brush(0xFFE0E0E0));
    }

    // Which pin a grid slot shows. While one is dragged the others close up around the slot it would
    // drop into, which stays empty (-1): the pins in order without the dragged one, it put in there.
    int PinAtSlot(int slot, bool folders = false)
    {
        if (!g_pinDragging || g_dragFolders != folders)
            return slot;
        if (slot == g_dragSlot)
            return -1;
        int k = slot < g_dragSlot ? slot : slot - 1;
        return k < g_dragPin ? k : k + 1;
    }

    void DrawPinnedGrid()
    {
        // Windows puts the title at 64, a DIP left of its first 32 DIP icon (33 + (96 - 32) / 2): the
        // title lines up with the icons. So it follows the icons' left edge as their size changes.
        float titleX = std::round(GridLeft() + (CellWidth() - CellIcon()) / 2 - 1);
        DrawColumnTitle(L"Uygulamalar", g_pinned.size(), L"Sabitlenmiş uygulama yok.", titleX, FolderSplit() - 16, g_scroll);
        DrawAllAppsButton();
        // While a pin is dragged (and the tiles settle after), each tile and section name is a layer of
        // its own that slides to its new place; see "Reordering pins". Only --render draws them here then.
        if (g_compositor && (g_pinDragging || g_settling) && !g_dragFolders)
            return;
        bool dragged = g_pinDragging && !g_dragFolders;
        auto const &layout = dragged ? LayOutSections(DragEntries(g_dragTarget), true) : PinLayout();
        // The sections' boxes: while dragging (only --render draws them here then), or always with
        // the boxes chosen (config `sectionStyle`).
        if (dragged || g_config.sectionStyle == 1)
            for (auto const &[section, area] : layout.regions)
                if (section)
                    g_dc->FillRoundedRectangle(D2D1::RoundedRect(Snapped(area, g_scroll), RegionRadius, RegionRadius), Brush(RegionFill));
        for (auto const &[section, header] : layout.headers)
        {
            auto r = Snapped(header, g_scroll);
            if (r.bottom < g_drawTop || r.top > g_drawBottom)
                continue;
            if (section == g_renaming && !dragged)
                DrawNameBox();
            // Nothing above a section in the top row (no apps out of sections): no line over it.
            else
                DrawSectionHeader(r, SectionName(section), g_config.sectionNames || header.top > GridY + SectionPad + 0.5f);
        }
        if (dragged && layout.newZone.bottom > 0)
            DrawNewZone(Snapped(layout.newZone, g_scroll), layout.newZonePressed);
        // While the settings panel is open, each section's grip to move it by (see "Moving a section").
        if (g_setOpen && !dragged)
            for (int k = 0; k < int(layout.regions.size()); k++)
                if (layout.regions[k].first)
                    DrawSectionGrip(SectionGripRect(Snapped(layout.regions[k].second, g_scroll)), g_hot == KeySectionGrip + k);
        for (int i = 0; i < int(g_pinned.size()); i++)
        {
            auto r = Snapped(layout.cells[i], g_scroll);
            if (r.bottom < g_drawTop || r.top > g_drawBottom)
                continue;
            if (!dragged || i != g_dragPin)
                DrawTile(g_pinned[i], r);
        }
    }

    // A pinned folder's row: its icon, and its name unless the column is narrowed to the icons alone.
    void DrawFolderRow(Tile &tile, D2D1_RECT_F r)
    {
        bool names = FolderNames();
        float icon = FolderIcon();
        float left = names ? r.left + 12 : (r.left + r.right - icon) / 2, top = (r.top + r.bottom - icon) / 2;
        Icon(tile, {left, top, left + icon, top + icon});
        if (names)
            Text(tile.name, g_fmtFolder.Get(), {r.left + 24 + icon, r.top, r.right - 12, r.bottom}, 0xFFFFFFFF);
    }

    // The "+" row: a thin plus where a folder's icon would be, and "Klasör ekle" where its name would
    // be, dimmer than the folders' own.
    void DrawFolderAdd(D2D1_RECT_F r)
    {
        bool names = FolderNames();
        float icon = FolderIcon();
        float left = names ? r.left + 12 : (r.left + r.right - icon) / 2;
        float cx = std::round(left + icon / 2) + 0.5f, cy = std::round((r.top + r.bottom) / 2) + 0.5f;
        float arm = std::round(std::clamp(icon * 0.3f, 5.f, 9.f));
        auto grey = Brush(0xFFC5C5C5);
        g_dc->DrawLine({cx - arm, cy}, {cx + arm, cy}, grey, 1);
        g_dc->DrawLine({cx, cy - arm}, {cx, cy + arm}, grey, 1);
        if (names)
            Text(L"Klasör ekle", g_fmtFolder.Get(), {r.left + 24 + icon, r.top, r.right - 12, r.bottom}, 0xFFA6A6A6);
    }

    // The folder list: its title and rows. Live, it is drawn into its own layer (DrawFolderLayer).
    void DrawFolderList()
    {
        // Narrowed to the icons alone, the list drops its title and names and centres the icons.
        if (FolderNames())
            DrawColumnTitle(L"Klasörler", g_folders.size(), L"", FolderLeft() + 12, FolderRight, g_folderScroll); // with none, the "+" row says it
        // While a folder is dragged (and the rows settle after), each row is a layer of its own, as the
        // grid's tiles are; see "Reordering pins". Only --render draws them here then. The rows end
        // where the "+" row is, which they pass under while it is held at the bottom.
        if (g_compositor && (g_pinDragging || g_settling) && g_dragFolders)
            return;
        // Live the layer clips (at the "+" row too), and the rows are drawn past the view to glide in;
        // --render clips here.
        float top = g_compositor ? g_drawTop : SplitTop, bottom = g_compositor ? g_drawBottom : FolderAddRect().top;
        g_dc->PushAxisAlignedClip({FolderLeft(), top, FolderRight, bottom}, D2D1_ANTIALIAS_MODE_ALIASED);
        for (int slot = 0; slot < int(g_folders.size()); slot++)
        {
            auto r = FolderRect(slot);
            if (r.bottom < top || r.top > bottom)
                continue;
            if (int i = PinAtSlot(slot, true); i >= 0)
                DrawFolderRow(g_folders[i], r);
        }
        g_dc->PopAxisAlignedClip();
    }

    // The folder column's parts on the front layer: the line, the "+" row, the line's handle.
    void DrawFolderColumn()
    {
        if (!g_config.folders)
            return;
        // The line between the columns: light, unlike the bands' separators, which darken; a darkening
        // all but vanishes on the dark content area.
        float split = FolderSplit();
        g_dc->FillRectangle({split, SplitTop, split + 1, SplitBottom}, Brush(0x29FFFFFF));

        if (!g_compositor)
            DrawFolderList();
        DrawFolderAdd(FolderAddRect());
        DrawScrollbar(1);

        // While the settings panel is open the line can be moved: a handle with arrows both ways says so.
        if (g_setOpen)
        {
            auto r = SplitHandleRect();
            bool hot = g_hot == KeySplitter || g_splitDragging;
            g_dc->FillRoundedRectangle(D2D1::RoundedRect(r, SplitHandleH / 2, SplitHandleH / 2), Brush(hot ? 0xFF555555 : 0xFF454545));
            D2D1_RECT_F edge{r.left + 0.5f, r.top + 0.5f, r.right - 0.5f, r.bottom - 0.5f};
            g_dc->DrawRoundedRectangle(D2D1::RoundedRect(edge, SplitHandleH / 2 - 0.5f, SplitHandleH / 2 - 0.5f), Brush(0x17FFFFFF), 1);
            float cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
            auto white = Brush(0xFFFFFFFF);
            for (float side : {-1.f, 1.f}) // "<" left of the middle, ">" right of it
            {
                float tip = cx + side * 9, back = cx + side * 5;
                g_dc->DrawLine({back, cy - 4}, {tip, cy}, white, 1.25f);
                g_dc->DrawLine({tip, cy}, {back, cy + 4}, white, 1.25f);
            }
        }
    }

    // The "+" glyph of the every-apps button; its highlight is a hover layer, lit while the list is open.
    void DrawAllAppsButton()
    {
        if (!AllAppsButtonShown())
            return;
        // A subtle filled button with a chevron: after the text going in, before it going back.
        auto r = AllAppsButtonRect();
        g_dc->FillRoundedRectangle(D2D1::RoundedRect(r, 4, 4), Brush(0x0FFFFFFF));
        D2D1_RECT_F edge{r.left + 0.5f, r.top + 0.5f, r.right - 0.5f, r.bottom - 0.5f};
        g_dc->DrawRoundedRectangle(D2D1::RoundedRect(edge, 3.5f, 3.5f), Brush(0x12FFFFFF), 1);
        bool back = g_allApps;
        float textLeft = r.left + ButtonPad + (back ? ChevronW + ChevronGap : 0);
        Text(AllAppsButtonText(), g_fmtName.Get(), {textLeft, r.top, r.right, r.bottom}, 0xFFFFFFFF);
        float cx = back ? r.left + ButtonPad : r.right - ButtonPad - ChevronW, cy = (r.top + r.bottom) / 2;
        float tip = back ? cx : cx + ChevronW, tail = back ? cx + ChevronW : cx;
        auto white = Brush(0xFFFFFFFF);
        g_dc->DrawLine({tail, cy - 4}, {tip, cy}, white, 1.25f);
        g_dc->DrawLine({tip, cy}, {tail, cy + 4}, white, 1.25f);
    }

    std::vector<AppRef>::iterator PinOfApp(Tile const &app);

    // The every-apps list's title row (title, count, sort buttons, "‹ Geri"): fixed, on the front layer
    // (DrawHome), the list scrolling under it.
    void DrawAllAppsHeader()
    {
        DrawColumnTitle(AllAppsTitle, g_apps.size(), L"", 32, FolderSplit() - 56, 0);
        for (int i = 0; i < SortCount; i++)
        {
            // The chosen one filled, as a selected segment; hover adds its own layer on top.
            auto r = SortButtonRect(i);
            bool chosen = g_config.appsSort == i;
            if (chosen)
                g_dc->FillRoundedRectangle(D2D1::RoundedRect(r, 4, 4), Brush(0x1FFFFFFF));
            ComPtr<IDWriteTextLayout> layout;
            if (SUCCEEDED(g_dwrite->CreateTextLayout(SortNames[i], UINT32(wcslen(SortNames[i])), g_fmtName.Get(), r.right - r.left, r.bottom - r.top, &layout)))
            {
                layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                g_dc->DrawTextLayout({r.left, r.top}, layout.Get(), Brush(chosen ? 0xFFFFFFFF : 0xFFC5C5C5));
            }
        }
        DrawAllAppsButton();
    }

    void DrawResults()
    {
        if (g_results.empty())
        {
            Text(g_apps.empty() ? L"Uygulamalar yükleniyor…" : L"Sonuç yok", g_fmtRow.Get(),
                 {40, ListTop() - g_scroll, FolderSplit() - 16, ListTop() + RowH - g_scroll}, 0xFFA6A6A6);
            return;
        }
        int first = std::max(0, int((g_drawTop - ListTop() + g_scroll) / RowH));
        for (int slot = first; slot < int(g_results.size()); slot++)
        {
            int i = ResultSlot(slot); // ResultSlot is its own inverse
            auto r = ResultRect(i);
            if (r.top > g_drawBottom)
                break;
            if (IsHeading(i))
            {
                wchar_t letter = HeadingLetter(i);
                Text({&letter, 1}, g_fmtTitle.Get(), {r.left + 12, r.top + 12, r.right - 12, r.bottom}, 0xFFFFFFFF);
                // A line under it, the same as the one between the menu's columns.
                g_dc->FillRectangle({r.left + 12, r.bottom - 4, r.right - 12, r.bottom - 3}, Brush(0x29FFFFFF));
                continue;
            }
            // Enter acts on the selected result; in the every-apps list only once something is typed.
            if (i == g_selected && !g_query.empty())
                g_dc->FillRoundedRectangle(D2D1::RoundedRect(r, 6, 6), Brush(0x1CFFFFFF));
            auto &app = g_apps[g_results[i]];
            Icon(app, {r.left + 12, r.top + 8, r.left + 36, r.top + 32});
            Text(app.name, g_fmtRow.Get(), {r.left + 48, r.top, r.right - 52, r.bottom}, 0xFFFFFFFF);
            // In the every-apps list: a check in the accent for a pinned app, a "+" for any other.
            if (g_allApps)
            {
                bool pinned = PinOfApp(app) != g_config.pins.end();
                Text(pinned ? L"\xE73E" : L"\xE710", g_fmtBarGlyph.Get(), {r.right - 44, r.top, r.right - 12, r.bottom},
                     pinned ? g_accent : 0xFFC5C5C5);
            }
        }
    }

    // Loaded at the size the current DPI needs, so the common scales get a stored size that fits exactly.
    ID2D1Bitmap1 *AccountPicture()
    {
        if (g_accountPictureDpi != g_dpi)
        {
            g_accountPictureDpi = g_dpi;
            g_accountPicture.Reset();
            std::vector<uint8_t> pixels;
            int w = 0, h = 0;
            if (LoadAccountPicture(int(Px(Avatar)), pixels, w, h))
            {
                auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,
                                                     D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
                g_resources->CreateBitmap(D2D1::SizeU(w, h), pixels.data(), w * 4, props, &g_accountPicture);
            }
        }
        return g_accountPicture.Get();
    }

    void DrawAccount()
    {
        auto r = AccountRect();
        float left = r.left + 4, top = r.top + 4;
        auto circle = D2D1::Ellipse({left + Avatar / 2, top + Avatar / 2}, Avatar / 2, Avatar / 2);
        ComPtr<ID2D1BitmapBrush1> fill;
        if (auto picture = AccountPicture())
            g_dc->CreateBitmapBrush(picture, D2D1::BitmapBrushProperties1(D2D1_EXTEND_MODE_CLAMP, D2D1_EXTEND_MODE_CLAMP, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC), &fill);
        if (fill)
        {
            // Fill the circle, cropping the longer side (the pictures are square anyway).
            auto size = g_accountPicture->GetPixelSize();
            float scale = Avatar / std::min(size.width, size.height);
            fill->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale) *
                               D2D1::Matrix3x2F::Translation(left - (size.width * scale - Avatar) / 2, top - (size.height * scale - Avatar) / 2));
            g_dc->FillEllipse(circle, fill.Get());
        }
        else
        {
            g_dc->FillEllipse(circle, Brush(0xFF3A3A3A));
            Text(L"\xE77B", g_fmtGlyph.Get(), {left, top, left + Avatar, top + Avatar}, 0xFFFFFFFF);
        }
    }

    // An icon-font glyph centred in `box`, optionally with the orange dot Windows puts on update
    // items: at `dot` from the glyph's centre, with the glyph cut away in a thin gap around it, as
    // Windows does (measured off its power button: 3 across, 5 right and 5 up, a 1.5 gap).
    void DrawGlyph(wchar_t glyph, IDWriteTextFormat *format, D2D1_RECT_F box, bool badge, D2D1_POINT_2F dot = {5, -5})
    {
        if (!badge)
        {
            Text({&glyph, 1}, format, box, 0xFFFFFFFF);
            return;
        }
        D2D1_POINT_2F centre{(box.left + box.right) / 2 + dot.x, (box.top + box.bottom) / 2 + dot.y};
        constexpr float radius = 3, gap = 1.5f;
        ComPtr<ID2D1RectangleGeometry> area;
        ComPtr<ID2D1EllipseGeometry> hole;
        ComPtr<ID2D1PathGeometry> mask;
        ComPtr<ID2D1GeometrySink> sink;
        g_d2d->CreateRectangleGeometry(box, &area);
        g_d2d->CreateEllipseGeometry(D2D1::Ellipse(centre, radius + gap, radius + gap), &hole);
        g_d2d->CreatePathGeometry(&mask);
        if (area && hole && mask && SUCCEEDED(mask->Open(&sink)))
        {
            area->CombineWithGeometry(hole.Get(), D2D1_COMBINE_MODE_EXCLUDE, nullptr, sink.Get());
            sink->Close();
            g_dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), mask.Get()), nullptr);
            Text({&glyph, 1}, format, box, 0xFFFFFFFF);
            g_dc->PopLayer();
        }
        else
            Text({&glyph, 1}, format, box, 0xFFFFFFFF);
        g_dc->FillEllipse(D2D1::Ellipse(centre, radius, radius), Brush(0xFFFB8528));
    }

    void DrawBar()
    {
        DrawAccount();
        // The power button always sits in a thin ring (user's choice), its hover filling the circle.
        auto power = BarRect(PowerButton);
        float ring = (power.right - power.left) / 2 - 0.5f;
        g_dc->DrawEllipse(D2D1::Ellipse({(power.left + power.right) / 2, (power.top + power.bottom) / 2}, ring, ring), Brush(0x2EFFFFFF), 1);
        for (int i = 0; i < BarCount; i++)
            if (g_barShown[i])
                DrawGlyph(BarActions[i].glyph[0], g_fmtBarGlyph.Get(), BarRect(i), i == PowerButton && g_updatesPending);
    }

    D2D1_ROUNDED_RECT Body() { return D2D1::RoundedRect({0.5f, 0.5f, MenuW - 0.5f, MenuH - 0.5f}, 8, 8); }

    // Tints a whole body (the menu or the flyout). In the live window the visual tree's rounded clip
    // shapes the edge, and filling right to it matters: a fill rounded on its own left a thin ring
    // where the blurred backdrop showed through untinted. --render has no clip, so it rounds here.
    void FillBody(float width, float height, uint32_t color, float opacity)
    {
        if (g_compositor)
            g_dc->FillRectangle({0, 0, width, height}, Brush(color, opacity));
        else
            g_dc->FillRoundedRectangle(D2D1::RoundedRect({0, 0, width, height}, 8, 8), Brush(color, opacity));
    }

    // The body's own colour where the acrylic is not used: the menu set to no transparency, and
    // --render, which has no desktop behind it. It is the acrylic's tint, solid.
    void FillSolidBody(float width, float height)
    {
        if (!g_compositor || g_config.transparency == 0)
            FillBody(width, height, TintedGrey(0x2C), 1);
    }

    // Bottom layer, redrawn only when the settings change. The acrylic sits behind it.
    void DrawBack()
    {
        FillSolidBody(MenuW, MenuH);
        // The bar is the body darkened by a fifth, as in Windows (measured over a dark and a bright
        // background alike), so it stands apart from the middle over anything.
        g_dc->PushAxisAlignedClip({0, ContentBottom, MenuW, MenuH}, D2D1_ANTIALIAS_MODE_ALIASED);
        FillBody(MenuW, MenuH, 0xFF000000, 0.2f);
        g_dc->PopAxisAlignedClip();
        // Its separator as in Windows: a faint darkening of whatever is behind, not a solid line.
        g_dc->FillRectangle({0, ContentBottom, MenuW, ContentBottom + 1}, Brush(0x24000000));

        // The search box lets the backdrop through too: a two-thirds grey fill and a faint light rim.
        g_dc->FillRoundedRectangle(D2D1::RoundedRect(SearchBox, 16, 16), Brush(SearchFill()));
        D2D1_RECT_F rim{SearchBox.left + 0.5f, SearchBox.top + 0.5f, SearchBox.right - 0.5f, SearchBox.bottom - 0.5f};
        g_dc->DrawRoundedRectangle(D2D1::RoundedRect(rim, 15.5f, 15.5f), Brush(0x14FFFFFF), 1);
    }

    // Top layer: everything that changes with typing, scrolling or the caret.
    void DrawFront()
    {
        DrawSearch();
        g_dc->PushAxisAlignedClip({0, SplitTop, MenuW, SplitBottom}, D2D1_ANTIALIAS_MODE_ALIASED); // the scrolling view
        DrawHome();
        g_dc->PopAxisAlignedClip();
        DrawBar();
        if (g_config.border)
            g_dc->DrawRoundedRectangle(Body(), Brush(0xFF3F3F3F), 1);
    }

    // Only for --render: the live window gets highlights from composition layers instead.
    void DrawHovers()
    {
        for (auto const &[key, opacity] : g_highlights)
        {
            if (opacity <= 0)
                continue;
            if (IsContentKey(key))
                g_dc->PushAxisAlignedClip({0, SplitTop, MenuW, SplitBottom}, D2D1_ANTIALIAS_MODE_ALIASED);
            auto r = KeyRect(key);
            float radius = HoverRadius(key, r);
            g_dc->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), Brush(0x17FFFFFF, opacity));
            if (IsContentKey(key))
                g_dc->PopAxisAlignedClip();
        }
    }

    // All layers flattened into the current target, for --render. (No blur: that comes from the desktop.)
    void DrawAll()
    {
        g_dc->BeginDraw();
        g_dc->SetTransform(D2D1::Matrix3x2F::Identity());
        g_dc->Clear(D2D1::ColorF(0, 0, 0, 0));
        PlaceSearchBox();
        DrawBack();
        DrawHovers();
        DrawFront();
        if (HRESULT hr = g_dc->EndDraw(); FAILED(hr))
            Log(L"EndDraw failed: " + std::to_wstring(hr));
    }

    float AppsLayerWidth() { return FolderSplit(); }

    // Where the glide is now, as the compositor plays it: the same curve, worked out by the clock.
    // The same for the folder list, which glides the same way in its own layer (DrawFolderLayer).
    Glide g_folderGlide;
    float g_folderDrawnScroll = 0;

    float GlideNow(Glide const &glide, float drawn)
    {
        if (!glide.start)
            return drawn;
        float t = std::min(1.f, float(GetTickCount64() - glide.start) / float(GlideTime.count()));
        // Ease-out cubic, which the compositor's curve below matches: 1 - (1 - t)^3.
        float eased = 1 - (1 - t) * (1 - t) * (1 - t);
        return glide.from + (glide.to - glide.from) * eased;
    }

    float GlideNow() { return GlideNow(g_glide, g_drawnScroll); }

    // The scroll is about to move by the wheel: glide there from where the content shows now.
    void GlideTo(Glide &glide, float from, float scroll)
    {
        // The layer reaches ScrollOverscan past the visible area, so the glide can start no further away.
        from = std::clamp(from, scroll - ScrollOverscan, scroll + ScrollOverscan);
        glide = {from, scroll, 0, true};
    }

    void GlideTo(float from) { GlideTo(g_glide, from, g_scroll); }

    // Puts a scrolled layer's content (and its highlights) where the scroll now is: gliding there from
    // where the content showed (`glide` pending: the wheel), or at once (anything else).
    void PlaceScrolled(Glide &glide, float &drawn, float scroll, wuc::Visual const &content, float base, wuc::Visual const &hovers)
    {
        if (glide.pending)
        {
            // Drawn where the scroll is going; shown, at first, where it was, by the difference.
            glide.pending = false;
            glide.start = GetTickCount64();
            float shift = Px(glide.to - glide.from);
            auto ease = Bezier(0.33f, 1.f, 0.68f, 1.f); // ease-out cubic, as GlideNow works it out
            for (auto [visual, rest] : {std::pair{content, base}, std::pair{hovers, 0.f}})
            {
                auto animation = g_compositor.CreateScalarKeyFrameAnimation();
                animation.InsertKeyFrame(0.f, rest + shift);
                animation.InsertKeyFrame(1.f, rest, ease);
                animation.Duration(GlideTime);
                visual.StartAnimation(L"Offset.Y", animation);
            }
        }
        else if (drawn != scroll)
        {
            glide.start = 0;
            for (auto [visual, rest] : {std::pair{content, base}, std::pair{hovers, 0.f}})
            {
                visual.StopAnimation(L"Offset.Y");
                auto at = visual.Offset();
                visual.Offset({at.x, rest, 0});
            }
        }
        drawn = scroll;
    }

    // The scrollbar thumb's layer: redrawn when its length or look changes, then put where the scroll
    // is, gliding there from `from` (a scroll) along with the content when that glides.
    // As Windows 11's, a scrollbar shows while its list scrolls or the pointer is on it, and fades
    // away a moment after: ScrollbarIdle after the last scroll, unless the pointer is still on it.
    constexpr auto ScrollbarIdle = 1000ms, ScrollbarFadeIn = 100ms, ScrollbarFadeOut = 300ms;

    void ShowScrollbar(int bar)
    {
        auto &thumb = g_thumbs[bar];
        if (!thumb.shown)
        {
            thumb.shown = true;
            Animate(thumb.visual, L"Opacity", 1, ScrollbarFadeIn, g_compositor.CreateLinearEasingFunction());
        }
        SetTimer(g_hwnd, TimerHideScrollbar + bar, UINT(ScrollbarIdle.count()), nullptr); // restarts it
    }

    void HideScrollbar(int bar)
    {
        KillTimer(g_hwnd, TimerHideScrollbar + bar);
        if (ScrollbarHot(bar))
            return ShowScrollbar(bar); // still on it: again later
        auto &thumb = g_thumbs[bar];
        if (!thumb.shown)
            return;
        thumb.shown = false;
        Animate(thumb.visual, L"Opacity", 0, ScrollbarFadeOut, g_compositor.CreateLinearEasingFunction());
    }

    // Hidden at once (the menu opening).
    void ResetScrollbars()
    {
        for (int bar : {0, 1})
        {
            auto &thumb = g_thumbs[bar];
            KillTimer(g_hwnd, TimerHideScrollbar + bar);
            thumb.shown = false;
            thumb.scroll = BarScroll(bar);
            thumb.visual.StopAnimation(L"Opacity");
            thumb.visual.Opacity(0);
        }
    }

    void PlaceThumb(int bar, float const *from)
    {
        auto &thumbLayer = g_thumbs[bar];
        auto visual = thumbLayer.visual;
        if (!ScrollbarShown(bar))
        {
            visual.IsVisible(false);
            return;
        }
        visual.IsVisible(true);
        if (BarScroll(bar) != thumbLayer.scroll || ScrollbarHot(bar))
            ShowScrollbar(bar);
        thumbLayer.scroll = BarScroll(bar);
        auto track = ScrollTrack(bar), thumb = ScrollThumb(bar);
        float h = thumb.bottom - thumb.top;
        bool hot = ScrollbarHot(bar);
        if (h != thumbLayer.drawnH || hot != thumbLayer.drawnHot || !thumbLayer.surface)
        {
            thumbLayer.surface = CreateSurface(ScrollThumbW, h);
            DrawSurface(thumbLayer.surface, ScrollThumbW, h, [h, hot]
                        { DrawThumb(h, hot); });
            visual.Brush(g_compositor.CreateSurfaceBrush(thumbLayer.surface));
            visual.Size({Px(ScrollThumbW), Px(h)});
            thumbLayer.drawnH = h;
            thumbLayer.drawnHot = hot;
        }
        winrt::Windows::Foundation::Numerics::float3 at{Px((track.left + track.right - ScrollThumbW) / 2), Px(thumb.top), 0};
        if (from)
        {
            auto glide = g_compositor.CreateVector3KeyFrameAnimation();
            glide.InsertKeyFrame(0.f, {at.x, Px(ScrollThumbAt(*from, bar).top), 0});
            glide.InsertKeyFrame(1.f, at, Bezier(0.33f, 1.f, 0.68f, 1.f));
            glide.Duration(GlideTime);
            visual.StartAnimation(L"Offset", glide);
        }
        else if (at != thumbLayer.at)
        {
            // Moved some other way (dragged, a new list, the column line): there at once.
            visual.StopAnimation(L"Offset");
            visual.Offset(at);
        }
        thumbLayer.at = at;
    }

    // The apps side's content, into its own layer, then placed: as it now is, or gliding to it.
    void DrawAppsLayer()
    {
        float w = AppsLayerWidth(), h = ContentBottom - ContentTop + 2 * ScrollOverscan;
        SIZE px{LONG(Px(w)), LONG(Px(h))};
        if (px.cx != g_appsSurfacePx.cx || px.cy != g_appsSurfacePx.cy)
        {
            g_appsSurface.as<ABI::Windows::UI::Composition::ICompositionDrawingSurfaceInterop>()->Resize(px);
            g_appsSprite.Size({float(px.cx), float(px.cy)});
            g_appsSurfacePx = px;
        }
        DrawSurface(g_appsSurface, w, h, []
                    {
        D2D1::Matrix3x2F at;
        g_dc->GetTransform(&at);
        g_dc->SetTransform(D2D1::Matrix3x2F::Translation(0, ScrollOverscan - ContentTop) * at);
        g_drawTop = ContentTop - ScrollOverscan;
        g_drawBottom = ContentBottom + ScrollOverscan;
        DrawAppsSide();
        g_drawTop = ContentTop;
        g_drawBottom = ContentBottom; });
        g_appsClip.RightInset(Px(MenuW - w));

        float base = Px(ContentTop - ScrollOverscan);
        bool gliding = g_glide.pending;
        float from = g_glide.from;
        if (g_glide.pending)
        {
            // Drawn where the scroll is going; shown, at first, where it was, by the difference.
            g_glide.pending = false;
            g_glide.start = GetTickCount64();
            float shift = Px(g_glide.to - g_glide.from);
            auto ease = Bezier(0.33f, 1.f, 0.68f, 1.f); // ease-out cubic, as GlideNow works it out
            for (auto [visual, rest] : {std::pair{g_appsSprite.as<wuc::Visual>(), base}, std::pair{g_appsHovers.as<wuc::Visual>(), 0.f}})
            {
                auto glide = g_compositor.CreateScalarKeyFrameAnimation();
                glide.InsertKeyFrame(0.f, rest + shift);
                glide.InsertKeyFrame(1.f, rest, ease);
                glide.Duration(GlideTime);
                visual.StartAnimation(L"Offset.Y", glide);
            }
        }
        else if (g_drawnScroll != g_scroll)
        {
            // Any other scroll (the scrollbar, the keyboard, a new list) goes there at once.
            g_glide.start = 0;
            g_appsSprite.StopAnimation(L"Offset.Y");
            g_appsHovers.StopAnimation(L"Offset.Y");
            g_appsSprite.Offset({0, base, 0});
            g_appsHovers.Offset({0, 0, 0});
        }
        g_drawnScroll = g_scroll;
        PlaceThumb(0, gliding ? &from : nullptr);
    }

    // The folder list into its layer: a surface over the folder column's part of the scrolling view and
    // ScrollOverscan past it both ways, so that it can glide on the wheel as the apps side does. While
    // the "+" row is held at the bottom, the layer's clip ends at it (fixed, so the rows glide under it).
    float g_folderSpriteX = -1;
    wuc::InsetClip g_folderClip{nullptr};

    void DrawFolderLayer()
    {
        g_folderSprite.IsVisible(g_config.folders);
        if (!g_config.folders)
        {
            PlaceThumb(1, nullptr); // hides it
            return;
        }
        float left = FolderSplit() + 1, w = MenuW - left, h = SplitBottom - SplitTop + 2 * ScrollOverscan;
        float top = SplitTop - ScrollOverscan;
        SIZE px{LONG(Px(w)), LONG(Px(h))};
        if (px.cx != g_folderSurfacePx.cx || px.cy != g_folderSurfacePx.cy || !g_folderSurface)
        {
            if (!g_folderSurface)
            {
                g_folderSurface = CreateSurface(w, h);
                g_folderSprite.Brush(g_compositor.CreateSurfaceBrush(g_folderSurface));
            }
            else
                g_folderSurface.as<ABI::Windows::UI::Composition::ICompositionDrawingSurfaceInterop>()->Resize(px);
            g_folderSprite.Size({float(px.cx), float(px.cy)});
            g_folderSurfacePx = px;
        }
        DrawSurface(g_folderSurface, w, h, [left, top]
                    {
        D2D1::Matrix3x2F at;
        g_dc->GetTransform(&at);
        g_dc->SetTransform(D2D1::Matrix3x2F::Translation(-left, -top) * at);
        g_drawTop = top;
        g_drawBottom = top + SplitBottom - SplitTop + 2 * ScrollOverscan;
        DrawFolderList();
        g_drawTop = ContentTop;
        g_drawBottom = ContentBottom; });
        g_folderClip.BottomInset(Px(MenuH - std::min(SplitBottom, FolderAddRect().top)));
        if (Px(left) != g_folderSpriteX)
        {
            // The column line moved (or the layer is new): there at once.
            g_folderSpriteX = Px(left);
            g_folderGlide.pending = false;
            g_folderSprite.StopAnimation(L"Offset.Y");
            g_folderSprite.Offset({g_folderSpriteX, Px(top), 0});
            g_folderDrawnScroll = g_folderScroll;
        }
        // Its scrollbar's thumb glides along with it.
        bool gliding = g_folderGlide.pending;
        float from = g_folderGlide.from;
        PlaceScrolled(g_folderGlide, g_folderDrawnScroll, g_folderScroll, g_folderSprite, Px(top), g_folderHovers);
        PlaceThumb(1, gliding ? &from : nullptr);
    }

    // Each column's ends fade while there is more beyond them; the folders' bottom fade sits above
    // the "+" row while that is held at the bottom, the rows passing under it.
    void UpdateFades()
    {
        SetFade(g_appsFade, AppsViewTop(), SplitBottom, g_scroll > 0, g_scroll < MaxScroll());
        bool below = g_folderScroll < MaxFolderScroll();
        SetFade(g_folderFade, SplitTop, below ? FolderAddRect().top : SplitBottom, g_folderScroll > 0, below);
    }

    void PlaceWindow();
    void DrawSettings();
    extern bool g_resizePending, g_setOpen;

    void Render()
    {
        if (g_resizePending)
        {
            // A new menu size from the settings panel: laid out, placed and drawn in one go, so the
            // layers it makes anew never reach the screen empty. The panel then shows the columns it gives.
            g_resizePending = false;
            PlaceWindow();
            if (g_setOpen)
                DrawSettings();
        }
        g_dirty = false;
        PlaceSearchBox();
        if (g_backDirty)
        {
            UpdateAcrylic(g_main);
            DrawSurface(g_backSurface, MenuW, MenuH, DrawBack);
            g_backDirty = false;
        }
        DrawSurface(g_frontSurface, MenuW, MenuH, DrawFront);
        DrawAppsLayer();
        DrawFolderLayer();
        UpdateFades();
        for (auto const &[key, layer] : g_hoverLayers)
            PlaceHover(key, layer);
        PlaceCaret();
        PlaceSearchIcons();
        PumpIcons(); // icons the drawing found missing
    }

    // ---- Composition setup ----

    // (Re)creates everything sized in pixels; called at start and when the DPI or the menu size changes.
    void CreateLayers()
    {
        DropHovers(false);
        g_shapes.clear();
        g_layersDpi = g_dpi;
        g_layersW = MenuW;
        g_layersH = MenuH;
        winrt::Windows::Foundation::Numerics::float2 size{Px(MenuW), Px(MenuH)};

        g_backSurface = CreateSurface(MenuW, MenuH);
        g_frontSurface = CreateSurface(MenuW, MenuH);
        g_backVisual.Brush(g_compositor.CreateSurfaceBrush(g_backSurface));
        g_frontVisual.Brush(g_compositor.CreateSurfaceBrush(g_frontSurface));
        for (auto visual : {g_backVisual.as<wuc::Visual>(), g_frontVisual.as<wuc::Visual>(), g_contentHovers.as<wuc::Visual>(),
                            g_barHovers.as<wuc::Visual>()})
            visual.Size(size);
        g_caretVisual.Size({std::max(1.f, Px(1)), Px(19)}); // 19 tall, as in Windows
        DrawSearchIcons();
        g_searchIconsBack = -1; // placed anew, at the new scale, without a change to play
        // Highlights in the scrolling area must not spill over the search band or the bottom bar.
        g_contentHovers.Clip(g_compositor.CreateInsetClip(0, Px(SplitTop), 0, Px(MenuH - SplitBottom)));

        // The apps layer: clipped to the apps side of the content area (its width is set as it is drawn).
        g_appsSurface = CreateSurface(MenuW, ContentBottom - ContentTop + 2 * ScrollOverscan);
        g_appsSurfacePx = {}; // sized to the apps side on the next draw
        g_appsSprite.Brush(g_compositor.CreateSurfaceBrush(g_appsSurface));
        g_appsSprite.Offset({0, Px(ContentTop - ScrollOverscan), 0});
        g_appsLayer.Size(size);
        g_appsHovers.Size(size);
        g_appsClip = g_compositor.CreateInsetClip(0, Px(AppsViewTop()), 0, Px(MenuH - SplitBottom));
        g_appsLayer.Clip(g_appsClip);
        // The folder list's layer: its surface is made on the next draw, at the new scale.
        for (auto visual : {g_folderLayer.as<wuc::Visual>(), g_folderHovers.as<wuc::Visual>(), g_folderTilesHost.as<wuc::Visual>()})
            visual.Size(size);
        g_folderClip = g_compositor.CreateInsetClip(0, Px(SplitTop), 0, Px(MenuH - SplitBottom));
        g_folderLayer.Clip(g_folderClip);
        g_folderSurface = nullptr;
        g_folderSurfacePx = {};
        g_folderSpriteX = -1; // placed anew on the next draw
        for (auto &thumb : g_thumbs)
        {
            thumb.drawnH = -1; // redrawn at the new scale
            thumb.at = {-1, -1, 0};
        }
        g_backDirty = true;
        Invalidate();
    }

    HRESULT CreateComposition()
    {
        DispatcherQueueOptions options{sizeof options, DQTYPE_THREAD_CURRENT, DQTAT_COM_STA};
        HRESULT hr = CreateDispatcherQueueController(options,
                                                     reinterpret_cast<ABI::Windows::System::IDispatcherQueueController **>(winrt::put_abi(g_queue)));
        if (FAILED(hr))
            return hr;

        g_compositor = wuc::Compositor();
        auto interop = g_compositor.as<ABI::Windows::UI::Composition::ICompositorInterop>();
        return interop->CreateGraphicsDevice(g_d2dDevice.Get(),
                                             reinterpret_cast<ABI::Windows::UI::Composition::ICompositionGraphicsDevice **>(winrt::put_abi(g_graphics)));
    }

    LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);

    // The menu's window. Its body, above the blurred backdrop: the tint, the highlights, the content.
    HRESULT CreateMenu(HINSTANCE instance)
    {
        if (HRESULT hr = CreatePopup(g_main, L"MyStartMenu", L"Başlat Menüm", WndProc, 0, nullptr, instance); FAILED(hr))
            return hr;
        g_hwnd = g_main.hwnd;
        g_dpi = GetDpiForWindow(g_hwnd);
        g_backVisual = g_compositor.CreateSpriteVisual();
        g_contentHovers = g_compositor.CreateContainerVisual();
        g_barHovers = g_compositor.CreateContainerVisual();
        // The apps side scrolls in a layer of its own: its highlights under its content.
        g_appsLayer = g_compositor.CreateLayerVisual();
        g_appsHovers = g_compositor.CreateContainerVisual();
        g_appsSprite = g_compositor.CreateSpriteVisual();
        g_dragTiles = g_compositor.CreateContainerVisual(); // the pins' tiles while one is dragged
        g_appsLayer.Children().InsertAtTop(g_appsHovers);
        g_appsLayer.Children().InsertAtTop(g_appsSprite);
        g_appsLayer.Children().InsertAtTop(g_dragTiles);
        g_frontVisual = g_compositor.CreateSpriteVisual();
        for (auto &thumb : g_thumbs)
        {
            thumb.visual = g_compositor.CreateSpriteVisual(); // over the tracks, which the front layer draws
            thumb.visual.Opacity(0);                          // until a scroll shows it
        }
        g_caretVisual = g_compositor.CreateSpriteVisual();
        g_caretVisual.Brush(g_compositor.CreateColorBrush(winrt::Windows::UI::Color{255, 0xD2, 0xD2, 0xD2}));
        g_searchIcons = g_compositor.CreateContainerVisual();
        g_magnifierVisual = g_compositor.CreateSpriteVisual();
        g_backArrowVisual = g_compositor.CreateSpriteVisual();
        g_searchIcons.Children().InsertAtTop(g_magnifierVisual);
        g_searchIcons.Children().InsertAtTop(g_backArrowVisual);
        // The folder list's layer: its highlights, its rows, the rows while one is dragged.
        g_folderLayer = g_compositor.CreateLayerVisual();
        g_folderHovers = g_compositor.CreateContainerVisual();
        g_folderSprite = g_compositor.CreateSpriteVisual();
        g_folderTilesHost = g_compositor.CreateContainerVisual();
        g_folderTiles = g_compositor.CreateContainerVisual();
        g_folderTilesHost.Children().InsertAtTop(g_folderTiles);
        for (auto visual : {g_folderHovers.as<wuc::Visual>(), g_folderSprite.as<wuc::Visual>(), g_folderTilesHost.as<wuc::Visual>()})
            g_folderLayer.Children().InsertAtTop(visual);
        for (auto visual : {g_backVisual.as<wuc::Visual>(), g_contentHovers.as<wuc::Visual>(), g_barHovers.as<wuc::Visual>(),
                            g_appsLayer.as<wuc::Visual>(), g_folderLayer.as<wuc::Visual>(), g_frontVisual.as<wuc::Visual>(),
                            g_thumbs[0].visual.as<wuc::Visual>(), g_thumbs[1].visual.as<wuc::Visual>(), g_searchIcons.as<wuc::Visual>(),
                            g_caretVisual.as<wuc::Visual>()})
            g_main.body.Children().InsertAtTop(visual);
        // The ends' fades; without them (the effect failing) the columns simply end sharp.
        try
        {
            g_appsLayer.Effect(FadeBrush(g_appsFade));
            g_folderLayer.Effect(FadeBrush(g_folderFade));
        }
        catch (winrt::hresult_error const &error)
        {
            Log(L"edge fade failed: " + std::to_wstring(error.code()) + L" " + std::wstring(error.message()));
            g_appsLayer.Effect(nullptr);
            g_folderLayer.Effect(nullptr);
            g_appsFade = {};
            g_folderFade = {};
        }
        CreateLayers();
        return S_OK;
    }

    // ---- Showing and hiding ----

    // Replaces a tile list, keeping the icons of the apps that are still in it.
    void SetTiles(std::vector<Tile> &tiles, std::vector<AppRef> const &apps)
    {
        std::unordered_map<std::wstring, Tile> old;
        for (auto &tile : tiles)
            old.emplace(tile.id, std::move(tile));
        tiles.clear();
        tiles.reserve(apps.size());
        for (auto const &app : apps)
        {
            if (auto it = old.find(app.id); it != old.end())
            {
                tiles.push_back(std::move(it->second));
                tiles.back().name = app.name;
                old.erase(it);
            }
            else
                tiles.push_back({app.id, app.name});
            tiles.back().added = app.added;
        }
    }

    void SetApps(std::vector<AppRef> const &apps) { SetTiles(g_apps, apps); }

    // Loads the pins' icons (from the cache, or asks the helper for them) before the menu
    // opens, so its first frame already has them.
    void PrefetchIcons()
    {
        if (!g_hwnd)
            return;
        for (auto &tile : g_pinned)
            IconOf(tile, int(std::round(Dims(tile.group != 0).icon * g_dpi / 96.f)));
        for (auto &tile : g_folders)
            IconOf(tile, int(std::round(FolderIcon() * g_dpi / 96.f)));
        PumpIcons();
    }

    // The app list is only needed once the user types. The helper refreshes it at start and whenever
    // apps change (WatchAppChanges); until it is done, the list from the last run is used.
    void FetchApps()
    {
        if (g_fetchingApps)
        {
            g_fetchAgain = true; // something changed while it ran; run again after
            return;
        }
        g_fetchingApps = StartHelper(L"--apps \"" + AppListFile() + L"\"", g_hwnd, WM_APPS_READY);
        g_appsFetchedAt = GetTickCount64();
    }

    // The pins are the menu's own (config `pins`), not Windows': Windows lets no other program take a
    // pin off its Start menu, so a list kept in step with it could never lose one. Only the first time,
    // with no list in the config yet, do they start as a copy of Windows' Start pins.
    bool g_seedingPins = false; // the helper is reading Windows' pins for that first copy

    void UpdateHot();
    void SaveConfig();

    // The pinned apps in the order shown: those in no section first, then each section's apps
    // together, the sections in the order they first come in (the file may have been edited by hand).
    std::vector<AppRef> GroupedApps(std::vector<AppRef> const &pins)
    {
        std::vector<AppRef> apps;
        for (auto const &pin : pins)
            if (!pin.folder && !pin.group)
                apps.push_back(pin);
        std::unordered_set<int> placed;
        for (auto const &pin : pins)
        {
            if (pin.folder || !pin.group || placed.contains(pin.group))
                continue;
            placed.insert(pin.group);
            for (auto const &member : pins)
                if (!member.folder && member.group == pin.group)
                    apps.push_back(member);
        }
        return apps;
    }

    // The apps to the grid, the folders to the list; each keeps the list's order.
    void SplitPins(std::vector<AppRef> const &pins)
    {
        std::vector<AppRef> apps = GroupedApps(pins), folders;
        for (auto const &pin : pins)
            if (pin.folder)
                folders.push_back(pin);
        SetTiles(g_pinned, apps);
        for (size_t i = 0; i < apps.size(); i++)
            g_pinned[i].group = apps[i].group;
        SetTiles(g_folders, folders);
        g_pinsVersion++;
    }

    void SetPins(std::vector<AppRef> const &pins)
    {
        SplitPins(pins);
        ClampScrolls();
        PrefetchIcons();
        if (g_visible)
            UpdateHot();
        Invalidate();
    }

    // The menu's list from now on: saved with the settings, and shown.
    void AdoptPins(std::vector<AppRef> pins)
    {
        g_config.pins = std::move(pins);
        g_config.hasPins = true;
        SaveConfig();
        SetPins(g_config.pins);
    }

    // At start: the saved list; or, the first time, Windows' pins as the helper last read them, or
    // as it reads them now (WM_PINS_READY).
    void LoadPins()
    {
        if (g_config.hasPins)
            return SetPins(g_config.pins);
        if (auto last = ReadPinList(); !last.empty())
            return AdoptPins(std::move(last));
        g_seedingPins = StartHelper(L"--pins \"" + PinListFile() + L"\"", g_hwnd, WM_PINS_READY);
    }

    void PlaceWindow();

    void ApplyConfig(Config config)
    {
        if (config.iconSize != g_config.iconSize || config.folderIconSize != g_config.folderIconSize ||
            config.tileSize != g_config.tileSize || config.tileGap != g_config.tileGap ||
            config.folderWidth != g_config.folderWidth || config.appNames != g_config.appNames)
            DropHovers(true); // sized for the old cells and rows
        bool resized = config.menuWidthStep != g_config.menuWidthStep || config.menuHeightStep != g_config.menuHeightStep;
        g_config = std::move(config);
        // A size an older version saved in DIPs: the nearest choice, saved as one from now on.
        if (g_config.menuWidthStep < 0 || g_config.menuHeightStep < 0)
        {
            if (g_config.menuWidthStep < 0)
                g_config.menuWidthStep = NearestStep(float(g_config.menuWidth), MenuWidthOf, MenuWidthSteps);
            if (g_config.menuHeightStep < 0)
                g_config.menuHeightStep = NearestStep(float(g_config.menuHeight), MenuHeightOf, MenuHeightSteps);
            SaveConfig();
        }
        g_hooksEnabled = g_config.enabled;
        if (g_config.hasPins)
            SplitPins(g_config.pins); // the file may have been edited by hand
        if (!g_hwnd)
            LayoutMenu(); // the window, once there, lays out at its own DPI
        else if (resized && g_visible)
            PlaceWindow(); // otherwise on the next open
        if (FixSectionColumns())
            SaveConfig(); // every section in a column of its own choosing
        ClampScrolls();
        // Favourites are not shown for now. The list stays in the config file.
        PrefetchIcons();
        g_backDirty = true;
        Invalidate();
    }

    void ReloadConfigIfChanged()
    {
        uint64_t stamp = ConfigStamp();
        if (stamp == g_configStamp)
            return;
        g_configStamp = stamp;
        ApplyConfig(LoadConfig());
    }

    // The shell's own flyouts are windows of its own hosts: a taskbar button's jump list and the
    // notification centre of ShellExperienceHost, search (Win+S) of SearchHost, quick settings of
    // ShellHost (all logged, 18.10.2026), the clipboard and emoji panels of TextInputHost, widgets of
    // Widgets. While one has the foreground Windows refuses it to us, every way below included: the
    // menu opened behind it, then without the keyboard. Each closes on Esc.
    bool ShellFlyoutInFront()
    {
        HWND front = GetForegroundWindow();
        if (!front)
            return false;
        DWORD pid = 0;
        GetWindowThreadProcessId(front, &pid);
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!process)
            return false;
        wchar_t path[MAX_PATH];
        DWORD size = MAX_PATH;
        wchar_t const *name = QueryFullProcessImageNameW(process, 0, path, &size) ? wcsrchr(path, L'\\') : nullptr;
        CloseHandle(process);
        if (!name)
            return false;
        for (wchar_t const *host : {L"ShellExperienceHost.exe", L"SearchHost.exe", L"StartMenuExperienceHost.exe",
                                    L"ShellHost.exe", L"TextInputHost.exe", L"Widgets.exe", L"WidgetBoard.exe"})
            if (_wcsicmp(name + 1, host) == 0)
                return true;
        return false;
    }

    // Windows only lets a background process take the foreground right after it produced input.
    bool TakeForeground()
    {
        SendMaskKey();
        if (SetForegroundWindow(g_hwnd))
            return true;
        DWORD current = GetCurrentThreadId();
        DWORD other = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
        if (other && other != current)
        {
            AttachThreadInput(current, other, TRUE);
            SetForegroundWindow(g_hwnd);
            AttachThreadInput(current, other, FALSE);
        }
        return GetForegroundWindow() == g_hwnd;
    }

    // Set once a shell flyout was closed: the foreground is held until the half second is up. The
    // closing flyout hands the foreground back to the window before it (the desktop, logged) just after
    // the menu took it, which closed the menu (the user saw it, "sometimes"); meanwhile losing it
    // closes nothing (WM_ACTIVATE) and the next step takes it again.
    bool g_frontHolding = false;

    // One step towards the foreground; true when done: the menu has it (and holds nothing), or no
    // longer wants it. A shell flyout in front is closed with Esc, as Windows' own Start menu closes
    // it; another may be under it (search, then the clock opened over it), so the next step looks again.
    bool FrontStep()
    {
        if (!g_visible || g_closing)
            return true;
        if (ShellFlyoutInFront())
        {
            g_frontHolding = true;
            SendEscape();
            return false;
        }
        if (GetForegroundWindow() != g_hwnd && !TakeForeground())
            return false;
        return !g_frontHolding;
    }

    void BringToFront()
    {
        static int tries;
        tries = 16; // every 30 ms for half a second
        g_frontHolding = false;
        if (!FrontStep())
            SetTimer(g_hwnd, TimerTakeForeground, 30, [](HWND hwnd, UINT, UINT_PTR id, DWORD)
                     {
                if (FrontStep() || --tries <= 0)
                {
                    g_frontHolding = false;
                    KillTimer(hwnd, id);
                } });
    }

    // The menu's size is being changed (the settings panel's sliders): its window spans all the room
    // the menu could take, so that a new size changes only the visual tree, which the compositor shows
    // whole in one frame. Resizing the window each time showed frames of the old body in the new
    // window, a strip of it empty: the menu flickered. Back to the menu's own size when it next opens.
    bool g_menuSizing = false;
    bool g_resizePending = false; // a new size waits for the next Render

    void PlaceWindow()
    {
        g_dpi = GetDpiForWindow(g_hwnd);
        RECT work;
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        DWORD centered = 1, size = sizeof centered;
        RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced",
                     L"TaskbarAl", RRF_RT_REG_DWORD, nullptr, &centered, &size);
        LayoutMenu();
        ClampScrolls();
        int w = int(Px(MenuW)), h = int(Px(MenuH)), gap = int(Px(ScreenGap));
        int x = centered ? work.left + (work.right - work.left - w) / 2 : work.left + gap;
        // Where Windows' own menu would start, from this one's left, for the fixed bar (BarSpanLeft).
        int windowsX = centered ? work.left + (work.right - work.left - int(Px(WindowsMenuW))) / 2 : x;
        g_barShift = (windowsX - x) * 96.f / g_dpi;
        RECT room{work.left + gap, work.top + gap, work.right - gap, work.bottom - gap};
        PlacePopup(g_main, {x, work.bottom - h - gap}, MenuW, MenuH, nullptr, g_menuSizing ? &room : nullptr);
        if (g_layersDpi != g_dpi || g_layersW != MenuW || g_layersH != MenuH)
            CreateLayers();
    }

    void UpdateHotFromCursor()
    {
        POINT p;
        GetCursorPos(&p);
        ScreenToClient(g_hwnd, &p);
        g_mouse = ClientToBody(g_main, p.x, p.y);
        UpdateHot();
    }

    // ---- Start button highlight ----
    //
    // While Windows' own menu is open, the taskbar shows its Start button as active. Explorer keeps
    // that state to itself, so we draw the same highlight in a tiny window of our own over the button:
    // always on top, never activated, and click-through, so the button underneath works as before.
    // (Being over the button rather than under its logo, it lightens the logo by the same 6%.)

    HWND g_highlightHwnd{};
    wuc::Desktop::DesktopWindowTarget g_highlightTarget{nullptr};
    wuc::SpriteVisual g_highlight{nullptr};
    wuc::CompositionScopedBatch g_highlightFade{nullptr};
    RECT g_highlightDrawn{}; // the rectangle (pixels) its surface was drawn for
    UINT g_highlightDpi = 0;
    int g_highlightGeneration = 0;
    // The window spans the button's row across its monitor, and the highlight slides within it as the
    // taskbar slides the button (icons coming and going on a centred taskbar); the compositor plays
    // the slide. A window the highlight's size, moved to the button's new place, jumped there ahead of
    // the button, as UI Automation gives the new place at once.
    RECT g_highlightSpan{};
    bool g_highlightShown = false;
    constexpr auto StartSlideTime = 250ms;

    HRESULT CreateStartHighlight(HINSTANCE instance)
    {
        WNDCLASSEXW wc{sizeof wc};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = instance;
        wc.lpszClassName = L"MyStartMenuStartHighlight";
        RegisterClassExW(&wc);
        g_highlightHwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOPMOST | WS_EX_NOACTIVATE |
                                              WS_EX_LAYERED | WS_EX_TRANSPARENT,
                                          wc.lpszClassName, L"", WS_POPUP,
                                          0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
        if (!g_highlightHwnd)
            return HRESULT_FROM_WIN32(GetLastError());
        SetLayeredWindowAttributes(g_highlightHwnd, 0, 255, LWA_ALPHA); // layered + transparent: clicks go through
        auto desktop = g_compositor.as<ABI::Windows::UI::Composition::Desktop::ICompositorDesktopInterop>();
        HRESULT hr = desktop->CreateDesktopWindowTarget(g_highlightHwnd, TRUE,
                                                        reinterpret_cast<ABI::Windows::UI::Composition::Desktop::IDesktopWindowTarget **>(winrt::put_abi(g_highlightTarget)));
        if (FAILED(hr))
            return hr;
        g_highlight = g_compositor.CreateSpriteVisual();
        g_highlight.Opacity(0);
        g_highlightTarget.Root(g_highlight);
        return S_OK;
    }

    // Measured off Windows at 100%: the button's rectangle less 2 on the sides and 4 above and below,
    // corners of 4, a 6% white fill and a slightly brighter edge that is brightest along the top.
    void DrawStartHighlight(float width, float height)
    {
        g_dc->FillRoundedRectangle(D2D1::RoundedRect({0, 0, width, height}, 4, 4), Brush(0x0FFFFFFF));
        auto edge = Gradient({0, 0}, {0, 3}, {{0, Argb(0x13FFFFFF)}, {1, Argb(0x07FFFFFF)}});
        if (edge)
            g_dc->DrawRoundedRectangle(D2D1::RoundedRect({0.5f, 0.5f, width - 0.5f, height - 0.5f}, 3.5f, 3.5f), edge.Get(), 1);
    }

    // The highlight's rectangle (pixels) at the button's present place; empty if no button is known
    // (hidden taskbar, Explorer restarting).
    RECT StartHighlightRect()
    {
        RECT button = StartButtonRect();
        if (button.right <= button.left)
            return {};
        RECT r{button.left + LONG(Px(2)), button.top + LONG(Px(4)), button.right - LONG(Px(2)), button.bottom - LONG(Px(4))};
        return r.right > r.left && r.bottom > r.top ? r : RECT{};
    }

    void PlaceStartHighlight(RECT r, bool slide)
    {
        int w = r.right - r.left, h = r.bottom - r.top;
        MONITORINFO monitor{sizeof monitor};
        GetMonitorInfoW(MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST), &monitor);
        RECT span{monitor.rcMonitor.left, r.top, monitor.rcMonitor.right, r.bottom};
        // Above the taskbar, which is itself always on top.
        if (!EqualRect(&span, &g_highlightSpan))
        {
            g_highlightSpan = span;
            SetWindowPos(g_highlightHwnd, HWND_TOPMOST, span.left, span.top, span.right - span.left, span.bottom - span.top,
                         SWP_NOACTIVATE | SWP_SHOWWINDOW);
            slide = false; // another row or screen: nothing to slide from
        }
        else
            SetWindowPos(g_highlightHwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        if (w != g_highlightDrawn.right - g_highlightDrawn.left || h != g_highlightDrawn.bottom - g_highlightDrawn.top ||
            g_highlightDpi != g_dpi)
        {
            g_highlightDpi = g_dpi;
            float dw = w * 96.f / g_dpi, dh = h * 96.f / g_dpi;
            auto surface = CreateSurface(dw, dh);
            DrawSurface(surface, dw, dh, [&]
                        { DrawStartHighlight(dw, dh); });
            g_highlight.Brush(g_compositor.CreateSurfaceBrush(surface));
            g_highlight.Size({float(w), float(h)});
        }
        g_highlightDrawn = r;
        float x = float(r.left - span.left);
        if (slide)
            Animate(g_highlight, L"Offset.X", x, StartSlideTime, Bezier(0.1f, 0.9f, 0.2f, 1.f));
        else
        {
            g_highlight.StopAnimation(L"Offset.X");
            g_highlight.Offset({x, 0, 0});
        }
    }

    void ShowStartHighlight(bool show)
    {
        if (!g_highlightHwnd)
            return;
        int generation = ++g_highlightGeneration;
        g_highlightShown = show;
        if (!show)
        {
            g_highlightFade = g_compositor.CreateScopedBatch(wuc::CompositionBatchTypes::Animation);
            Animate(g_highlight, L"Opacity", 0, HoverTime, g_compositor.CreateLinearEasingFunction());
            g_highlightFade.End();
            g_highlightFade.Completed([generation](auto &&, auto &&)
                                      {
            if (generation == g_highlightGeneration) ShowWindow(g_highlightHwnd, SW_HIDE); });
            return;
        }
        RECT r = StartHighlightRect();
        if (r.right <= r.left)
            return;
        PlaceStartHighlight(r, false);
        Animate(g_highlight, L"Opacity", 1, HoverTime, g_compositor.CreateLinearEasingFunction());
    }

    // A mouse press anywhere while the menu is open (Input's hook). The menu closes when it loses the
    // foreground (WM_ACTIVATE); opened without it, as over a taskbar button's jump list, it never
    // would, and stayed open whatever was clicked (the user saw it). Then a press outside the menu and
    // its own windows (the flyout, the settings panel) closes it.
    void HideMenu();
    void PressedOutside(POINT p)
    {
        if (!g_visible || g_closing || GetForegroundWindow() == g_hwnd)
            return;
        HWND hit = WindowFromPoint(p);
        if (hit && GetAncestor(hit, GA_ROOTOWNER) == g_hwnd)
            return;
        HideMenu();
    }

    // The button moved while the menu is open (Input's tracker, which polls it every frame meanwhile).
    void StartButtonMoved()
    {
        if (!g_highlightShown)
            return;
        if (RECT r = StartHighlightRect(); r.right > r.left)
            PlaceStartHighlight(r, true);
    }

    void PlaceAppsClip();

    void ShowMenu()
    {
        if (g_visible && !g_closing)
            return;
        g_closing = false;
        ReloadConfigIfChanged();
        // Changes are watched for; this only covers one that slipped by (an hour is plenty).
        if (GetTickCount64() - g_appsFetchedAt > 3600 * 1000)
            FetchApps();
        g_updatesPending = UpdatesPendingAtShutdown();
        ReadBarShown();                                 // Windows' Start settings may have changed while the menu was hidden
        UpdateRecycleBin();                             // and the Recycle Bin filled or emptied
        if (uint32_t tint = ReadTint(); tint != g_tint) // and the accent colour changed
        {
            g_tint = tint;
            g_backDirty = true;
        }

        g_query.clear();
        g_search.caret = g_search.anchor = 0;
        g_search.scroll = g_scroll = g_folderScroll = 0;
        g_allApps = false;
        PlaceAppsClip(); // as the home view's, should the list have been left open
        g_results.clear();
        g_searching = false;
        g_searchIconsBack = -1; // the magnifier at once, not played back from a search left open
        g_selected = 0;
        g_pressed = KeyNone;
        g_hot = KeyNone;
        DropHovers(false);

        g_menuSizing = false;
        PlaceWindow();
        ResetScrollbars(); // hidden until something scrolls
        g_visible = true;
        Render();

        // Always on top while open, as Windows' own menu is: a taskbar button's jump list, itself on
        // top, hid the menu opened over it (the user saw it). Raised on every open, to the top of that band.
        SetWindowPos(g_hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        OpenPopup(g_main, {0, SlideDistance}, true);
        BringToFront();
        MenuOpen(true); // the Start button followed, presses reported (PressedOutside)
        ShowStartHighlight(true);

        RestartCaretBlink();
        UpdateHotFromCursor();
    }

    void CloseFlyout();
    void OpenSettings();
    void CloseSettings();

    void HideMenu()
    {
        if (!g_visible || g_closing)
            return;
        g_closing = true;
        EndRename(true); // a name being edited is kept, as a text box keeps its text when left
        if (g_caretVisual)
            g_caretVisual.StopAnimation(L"Opacity"); // nothing blinks while hidden
        CloseFlyout();
        CloseSettings();
        MenuOpen(false);
        ShowStartHighlight(false);
        ClosePopup(g_main, {0, SlideDistance}, []
                   {
        g_closing = false;
        g_visible = false; });
    }

    void Toggle()
    {
        if (g_visible && !g_closing)
            HideMenu();
        else
            ShowMenu();
    }

    // ---- Flyout: the small menu above the account and power buttons ----
    //
    // A little window of its own, built like the menu: blurred backdrop, tint, hover highlights,
    // content, and the same rise from below when it opens. It never takes activation, so the menu
    // keeps the foreground and the keyboard; the menu passes arrows, Enter and Esc on to it.

    constexpr float FlyPadding = 4, FlyRowH = 36, FlyGlyph = 16, FlyMinW = 160, FlyGap = 8;

    // A disabled item is drawn faint, as Windows' are, and takes no hover, click or key.
    struct FlyoutItem
    {
        std::wstring text;
        wchar_t glyph;
        std::function<void()> run;
        bool badge = false, disabled = false;
    };
    enum class FlyoutAlign
    {
        Left,
        Center
    };

    PopupWindow g_fly;
    wuc::ContainerVisual g_flyHovers{nullptr};
    wuc::SpriteVisual g_flyBack{nullptr}, g_flyFront{nullptr};
    std::vector<FlyoutItem> g_flyItems;
    std::vector<wuc::SpriteVisual> g_flyRows; // the hover highlight of each item
    float g_flyW = 0, g_flyH = 0;
    int g_flyHot = -1, g_flyPressed = -1;
    bool g_flyOpen = false, g_flyTracking = false;
    float g_flyFrom = 0; // where it came in from and goes back to (DIPs, down is positive)

    D2D1_RECT_F FlyoutRow(int i)
    {
        float y = FlyPadding + i * FlyRowH;
        return {FlyPadding, y, g_flyW - FlyPadding, y + FlyRowH};
    }

    float FlyoutTextLeft()
    {
        bool glyphs = std::any_of(g_flyItems.begin(), g_flyItems.end(), [](auto const &item)
                                  { return item.glyph != 0; });
        return FlyPadding + 12 + (glyphs ? FlyGlyph + 12 : 0);
    }

    void SizeFlyout()
    {
        float text = 0;
        for (auto const &item : g_flyItems)
        {
            ComPtr<IDWriteTextLayout> layout;
            DWRITE_TEXT_METRICS m{};
            if (SUCCEEDED(g_dwrite->CreateTextLayout(item.text.c_str(), UINT32(item.text.size()), g_fmtRow.Get(), 10000, FlyRowH, &layout)))
                layout->GetMetrics(&m);
            text = std::max(text, m.widthIncludingTrailingWhitespace);
        }
        g_flyW = std::max(FlyMinW, std::ceil(FlyoutTextLeft() + text + 24 + FlyPadding));
        g_flyH = 2 * FlyPadding + g_flyItems.size() * FlyRowH;
    }

    // Lighter than the menu, like the Windows flyouts, and as see-through as the menu is set to be.
    void DrawFlyoutBack()
    {
        FillSolidBody(g_flyW, g_flyH);
    }

    void DrawFlyoutFront()
    {
        float textLeft = FlyoutTextLeft();
        for (int i = 0; i < int(g_flyItems.size()); i++)
        {
            auto r = FlyoutRow(i);
            bool faint = g_flyItems[i].disabled;
            if (faint)
                g_dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                       D2D1::IdentityMatrix(), 0.36f),
                                nullptr);
            if (wchar_t glyph = g_flyItems[i].glyph)
                DrawGlyph(glyph, g_fmtBarGlyph.Get(), {r.left + 12, r.top, r.left + 12 + FlyGlyph, r.bottom}, g_flyItems[i].badge,
                          {5.5f, -5.5f}); // measured off the Windows power flyout
            Text(g_flyItems[i].text, g_fmtRow.Get(), {textLeft, r.top, r.right - 12, r.bottom}, 0xFFFFFFFF);
            if (faint)
                g_dc->PopLayer();
        }
        // No outline, whatever the menu's border setting: the user wants the flyout without one.
    }

    void FlyoutFade(int row, float to)
    {
        if (row >= 0 && row < int(g_flyRows.size()))
            Animate(g_flyRows[row], L"Opacity", to, HoverTime, Bezier(0.5f, 1.f, 0.89f, 1.f));
    }

    void SetFlyoutHot(int row)
    {
        if (row == g_flyHot)
            return;
        FlyoutFade(g_flyHot, 0);
        g_flyHot = row;
        FlyoutFade(row, 1);
    }

    void CloseFlyout()
    {
        if (!g_flyOpen)
            return;
        g_flyOpen = false;
        g_flyHot = g_flyPressed = -1;
        ClosePopup(g_fly, {0, g_flyFrom});
    }

    void RunFlyoutItem(int row)
    {
        if (row < 0 || row >= int(g_flyItems.size()) || g_flyItems[row].disabled)
            return;
        auto run = g_flyItems[row].run;
        CloseFlyout();
        run();
    }

    // Fills the flyout with `items` and opens it where `place` puts its top left (given its size in
    // pixels) and sets `from` to how far below (+) or above (-) it starts, in flyout heights.
    void ShowFlyout(std::vector<FlyoutItem> items, std::function<POINT(int w, int h, float &from)> place)
    {
        g_flyItems = std::move(items);
        g_flyOpen = true;
        g_flyHot = g_flyPressed = -1;
        SizeFlyout();

        int w = int(Px(g_flyW)), h = int(Px(g_flyH));
        float from = 0.5f;
        POINT p = place(w, h, from);
        g_flyFrom = from * g_flyH;
        PlacePopup(g_fly, p, g_flyW, g_flyH, HWND_TOP);
        UpdateAcrylic(g_fly);

        for (auto visual : {g_flyBack.as<wuc::Visual>(), g_flyHovers.as<wuc::Visual>(), g_flyFront.as<wuc::Visual>()})
            visual.Size({float(w), float(h)});

        auto back = CreateSurface(g_flyW, g_flyH), front = CreateSurface(g_flyW, g_flyH);
        DrawSurface(back, g_flyW, g_flyH, DrawFlyoutBack);
        DrawSurface(front, g_flyW, g_flyH, DrawFlyoutFront);
        g_flyBack.Brush(g_compositor.CreateSurfaceBrush(back));
        g_flyFront.Brush(g_compositor.CreateSurfaceBrush(front));

        g_flyHovers.Children().RemoveAll();
        g_flyRows.clear();
        for (int i = 0; i < int(g_flyItems.size()); i++)
        {
            auto row = FlyoutRow(i);
            auto layer = g_compositor.CreateSpriteVisual();
            layer.Brush(ShapeFor(row));
            layer.Size({Px(row.right - row.left), Px(row.bottom - row.top)});
            layer.Offset({Px(row.left), Px(row.top), 0});
            layer.Opacity(0);
            g_flyHovers.Children().InsertAtTop(layer);
            g_flyRows.push_back(layer);
        }

        OpenPopup(g_fly, {0, g_flyFrom}, false);
    }

    RECT WorkArea()
    {
        MONITORINFO monitor{sizeof monitor};
        GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST), &monitor);
        return monitor.rcWork;
    }

    // Opens above a bottom-bar button: centred on it, or starting at its left edge.
    void OpenFlyout(int anchor, FlyoutAlign align, std::vector<FlyoutItem> items)
    {
        ShowFlyout(std::move(items), [&](int w, int h, float &)
                   {
        auto r = KeyRect(anchor);
        POINT p = BodyToScreen(g_main, align == FlyoutAlign::Center ? (r.left + r.right) / 2 : r.left, r.top - FlyGap);
        if (align == FlyoutAlign::Center) p.x -= w / 2;
        p.y -= h;
        RECT work = WorkArea();
        p.x = std::clamp(p.x, work.left, work.right - w);
        return p; });
    }

    // A context menu: at the pointer, opening down and to the right, or up and to the left where
    // the screen runs out, as Windows' own do.
    void OpenFlyoutAtPointer(std::vector<FlyoutItem> items)
    {
        POINT cursor;
        GetCursorPos(&cursor);
        RECT work = WorkArea();
        ShowFlyout(std::move(items), [&](int w, int h, float &from)
                   {
        // The body's top left is the point; the window's shadow margin is outside it.
        POINT p = cursor;
        if (p.x + w > work.right) p.x -= w;
        from = -0.25f;  // drops into place below the pointer
        if (p.y + h > work.bottom)
        {
            p.y -= h;
            from = 0.25f;  // or rises into place above it
        }
        return p; });
    }

    // Arrows, Enter and Esc while the flyout is open. False for keys it does not use.
    bool FlyoutKey(WPARAM key)
    {
        int count = int(g_flyItems.size());
        // The next item that is not disabled, going `step` from `row` round the list.
        auto next = [&](int row, int step)
        {
            for (int i = 0; i < count; i++)
            {
                row = (row + step + count) % count;
                if (!g_flyItems[row].disabled)
                    return row;
            }
            return g_flyHot;
        };
        switch (key)
        {
        case VK_ESCAPE:
            CloseFlyout();
            return true;
        case VK_DOWN:
            SetFlyoutHot(next(g_flyHot < 0 ? -1 : g_flyHot, 1));
            return true;
        case VK_UP:
            SetFlyoutHot(next(g_flyHot < 0 ? count : g_flyHot, -1));
            return true;
        case VK_RETURN:
            RunFlyoutItem(g_flyHot);
            return true;
        }
        return false;
    }

    LRESULT CALLBACK FlyoutProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
    {
        switch (message)
        {
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_NCHITTEST:
        {
            // The shadow margin is not part of the flyout: let the pointer through to the menu below.
            POINT p{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            ScreenToClient(hwnd, &p);
            return InBody(g_fly, ClientToBody(g_fly, p.x, p.y)) ? HTCLIENT : HTTRANSPARENT;
        }
        case WM_MOUSEMOVE:
        {
            if (!g_flyOpen)
                return 0;
            if (!g_flyTracking)
            {
                TRACKMOUSEEVENT track{sizeof track, TME_LEAVE, hwnd, 0};
                g_flyTracking = TrackMouseEvent(&track);
            }
            auto p = ClientToBody(g_fly, GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
            int row = -1;
            for (int i = 0; i < int(g_flyItems.size()); i++)
                if (Inside(FlyoutRow(i), p) && !g_flyItems[i].disabled)
                    row = i;
            if (row != g_flyHot)
                g_flyPressed = -1;
            SetFlyoutHot(row);
            return 0;
        }
        case WM_MOUSELEAVE:
            g_flyTracking = false;
            if (g_flyOpen)
                SetFlyoutHot(-1);
            return 0;
        case WM_LBUTTONDOWN:
            if (!g_flyOpen)
                return 0;
            g_flyPressed = g_flyHot;
            FlyoutFade(g_flyHot, 0.55f);
            return 0;
        case WM_LBUTTONUP:
            if (g_flyOpen && g_flyPressed >= 0 && g_flyPressed == g_flyHot)
                RunFlyoutItem(g_flyPressed);
            g_flyPressed = -1;
            return 0;
        case WM_PAINT:
            ValidateRect(hwnd, nullptr);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        }
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }

    std::vector<FlyoutItem> AccountItems()
    {
        return {
            // The menu's own settings panel; the menu stays open (user's choice: not a right click on
            // empty space, not the tray, which took the focus and so closed the menu).
            {L"Menü ayarları", L'\xE771', []
             { OpenSettings(); }},
            // No "lock" here: the power menu has it.
            {L"Hesap ayarları", L'\xE77B', []
             { HideMenu(); OpenTarget(L"ms-settings:yourinfo"); }},
            {L"Oturumu kapat", L'\xF3B1', []
             { HideMenu(); RunPowerAction(PowerAction::SignOut); }},
        };
    }

    // Takes the pin off the menu's own list; Windows' Start menu is not touched.
    void Unpin(std::wstring const &id)
    {
        std::erase_if(g_config.pins, [&](AppRef const &pin)
                      { return pin.id == id; });
        SaveConfig();
        SetPins(g_config.pins);
    }

    // The pinned app or folder under a key; null for any other key.
    Tile *PinOf(int key)
    {
        if (key >= KeyPinned && key < KeyFolder && key - KeyPinned < int(g_pinned.size()))
            return &g_pinned[key - KeyPinned];
        if (key >= KeyFolder && key < KeyBar && key - KeyFolder < int(g_folders.size()))
            return &g_folders[key - KeyFolder];
        return nullptr;
    }

    // The context menu of a section: renaming it (with the names shown), and breaking it up. It is
    // moved by its grip.
    void DissolveSection(int section);
    std::vector<FlyoutItem> SectionItems(int section)
    {
        std::vector<FlyoutItem> items;
        if (g_config.sectionNames)
            items.push_back({L"Yeniden adlandır", L'\xE8AC', [section]
                             { BeginRename(section); }});
        items.push_back({L"Grubu dağıt", L'\xE711', [section]
                         { DissolveSection(section); }});
        return items;
    }

    // The context menu of a pin, as in the Windows menu.
    // The Recycle Bin's adds "Empty Recycle Bin", faint while it is empty, as in Explorer. The helper
    // empties it (--empty-recycle-bin: the shell's machinery stays out of the menu), Windows asking
    // first; the menu closes meanwhile and asks again on its next open.
    std::vector<FlyoutItem> PinnedItems(Tile const &pin)
    {
        std::wstring id = pin.id;
        std::vector<FlyoutItem> items;
        if (IsRecycleBin(id))
        {
            UpdateRecycleBin();
            items.push_back({L"Geri Dönüşüm Kutusunu boşalt", L'\xE74D', []
                             {
                                 if (StartHelper(L"--empty-recycle-bin", g_hwnd, WM_NULL))
                                     HideMenu();
                             },
                             false, !g_recycleBinFull});
        }
        items.push_back({L"Başlangıç'tan kaldır", L'\xE77A', [id]
                         { Unpin(id); }});
        return items;
    }

    // The pin of an app from the app list, if it has one. Pins made from search carry the app list's
    // id. Pins copied from Windows' Start menu name a desktop app by its shortcut's path instead, which
    // the app list never shows; for those the name has to do. That goes for pinned folders too: Windows
    // leaves a shortcut to each in the Start menu's Programs, so they are in the app list as well.
    std::vector<AppRef>::iterator PinOfApp(Tile const &app)
    {
        return std::find_if(g_config.pins.begin(), g_config.pins.end(), [&](AppRef const &pin)
                            {
        if (pin.id == app.id) return true;
        bool shortcut = pin.id.size() > 4 && _wcsicmp(pin.id.c_str() + pin.id.size() - 4, L".lnk") == 0;
        return shortcut && _wcsicmp(pin.name.c_str(), app.name.c_str()) == 0; });
    }

    // At the end of the apps in no section; it shows once the search is cleared.
    void Pin(Tile const &app)
    {
        if (PinOfApp(app) != g_config.pins.end())
            return;
        g_config.pins.push_back({app.id, app.name, false});
        g_config.hasPins = true;
        SaveConfig();
        SetPins(g_config.pins);
    }

    // ---- Adding a folder ----
    //
    // The "+" row after the folders opens the shell's folder picker. It loads the shell's machinery,
    // so the helper shows it (--pick-folder); the menu closes meanwhile, the picker taking the
    // foreground, and opens again when it is done, the new folder at the list's end. A folder is
    // pinned by its path; already pinned, it is not added again.
    bool g_pickingFolder = false;

    void PickFolder()
    {
        if (g_pickingFolder)
            return;
        g_pickingFolder = StartHelper(L"--pick-folder \"" + PickedFolderFile() + L"\"", g_hwnd, WM_FOLDER_PICKED);
        if (g_pickingFolder)
            HideMenu();
    }

    void FolderPicked()
    {
        g_pickingFolder = false;
        bool added = false;
        for (auto folder : ReadPickedFolder())
        {
            bool pinned = std::any_of(g_config.pins.begin(), g_config.pins.end(),
                                      [&](AppRef const &pin)
                                      { return _wcsicmp(pin.id.c_str(), folder.id.c_str()) == 0; });
            if (pinned)
                continue;
            folder.folder = true;
            g_config.pins.push_back(folder);
            added = true;
        }
        DeleteFileW(PickedFolderFile().c_str());
        if (added)
        {
            g_config.hasPins = true;
            SaveConfig();
            SetPins(g_config.pins);
        }
        ShowMenu();
        if (added)
        {
            g_folderScroll = MaxFolderScroll(); // down to it
            Invalidate();
        }
    }

    // The context menu of a search result: pin it, or take its pin off if it has one.
    std::vector<FlyoutItem> ResultItems(int result)
    {
        Tile const &app = g_apps[g_results[result]];
        if (auto pin = PinOfApp(app); pin != g_config.pins.end())
        {
            std::wstring id = pin->id;
            return {{L"Başlangıç'tan kaldır", L'\xE77A', [id]
                     { Unpin(id); }}};
        }
        std::wstring id = app.id;
        return {{L"Başlangıç'a sabitle", L'\xE718', [id]
                 {
                     // Looked up again: the app list may have been refreshed while the flyout was open.
                     auto it = std::find_if(g_apps.begin(), g_apps.end(), [&](Tile const &tile)
                                            { return tile.id == id; });
                     if (it != g_apps.end())
                         Pin(*it);
                 }}};
    }

    // The same items, glyphs and order as the Windows menu's power flyout.
    // The update items appear only while updates wait for a shutdown, each above its plain twin.
    std::vector<FlyoutItem> PowerItems(bool updates)
    {
        auto run = [](PowerAction action)
        { return [action]
          { HideMenu(); RunPowerAction(action); }; };
        std::vector<FlyoutItem> items{
            {L"Kilitle", L'\xE72E', run(PowerAction::Lock)},
            {L"Uyku", L'\xE708', run(PowerAction::Sleep)},
        };
        if (updates)
            items.push_back({L"Güncelleştir ve kapat", L'\xE7E8', run(PowerAction::UpdateAndShutdown), true});
        items.push_back({L"Bilgisayarı kapat", L'\xE7E8', run(PowerAction::Shutdown)});
        if (updates)
            items.push_back({L"Güncelleştir ve yeniden başlat", L'\xE777', run(PowerAction::UpdateAndRestart), true});
        items.push_back({L"Yeniden Başlat", L'\xE777', run(PowerAction::Restart)});
        return items;
    }

    // ---- Settings panel: opens beside the menu from the button at the search box's end ----
    //
    // Built like the flyout: a window of its own that never takes activation, so the menu stays open
    // and in the foreground while the settings change, and every change shows in it at once. It
    // sits in the screen's bottom right corner, above the clock, and rises from the taskbar as the menu
    // does. It stays put whatever the menu does: beside the menu it moved with every change of the
    // menu's size, while the user was following it.

    constexpr float SetW = 288, SetPad = 16;
    constexpr float SetTitleY = 14;
    constexpr float SetSliderBelow = 30;                      // from a slider's label to its track
    constexpr float ToggleW = 40, ToggleH = 20, KnobBox = 16; // KnobBox: the knob's own square, room for its largest size
    constexpr auto KnobTime = 200ms;

    // Everything the panel has, in the order it shows. The headers are the drawers' (see SetBlock).
    enum SetPart
    {
        SetNone,
        SetSlider,
        SetTint,
        SetBorder,
        SetWidthSlider,
        SetHeightSlider,
        SetFolders,
        SetFolderSlider,
        SetFixedBar,
        SetTileSlider,
        SetGapSlider,
        SetIconSlider,
        SetAppNames,
        SetHighlightTiles,
        SetCenterUngrouped,
        SetSectionNames,
        SetSectionStyle,
        SetSectionColumns,
        SetSectionScale,
        SetSizeHeader,
        SetTilesHeader,
        SetSectionsHeader,
        SetPartCount
    };
    // The menu's width and height are choices of a few (segmented, as WinUI's): a row of buttons, the
    // one chosen in the accent.
    bool IsMenuSizeSlider(SetPart part) { return part == SetWidthSlider || part == SetHeightSlider; }
    // A row of buttons, one chosen: the menu's size, the tiles' gap (the user's wish: a few that look
    // right, not a slider), and the sections' look and columns.
    bool IsChoice(SetPart part)
    {
        return IsMenuSizeSlider(part) || part == SetGapSlider || part == SetSectionStyle || part == SetSectionColumns ||
               part == SetSectionScale || part == SetTint;
    }
    bool IsSlider(SetPart part) { return part == SetSlider || part == SetTileSlider || part == SetIconSlider || part == SetFolderSlider; }
    bool IsHeader(SetPart part) { return part >= SetSizeHeader && part <= SetSectionsHeader; }

    // The panel's blocks, top to bottom: what always shows (the title, the transparency, the border),
    // then a drawer for each kind of setting, as WinUI's expanders: a card whose header row (its name,
    // a chevron) opens it with a click, its settings coming down out from under it. One is open at a
    // time (the user's choice, 17.10.2026: the panel stays short); opening one closes the other. The
    // panel is held to the screen's bottom, so it grows and shrinks at its top: the window is always as
    // tall as the panel with its tallest drawer open, the body's top lower by what is closed
    // (SetShift). Each block is a visual of its own; the compositor slides them, and unrolls a drawer.
    enum SetBlock
    {
        BlockMain,
        BlockSize,
        BlockTiles,
        BlockSections,
        BlockCount
    };
    constexpr int CardCount = BlockCount - 1;
    constexpr wchar_t const *CardNames[CardCount] = {L"Pencere", L"Uygulamalar", L"Gruplar"};
    constexpr float SetHeaderH = 44, SetCardPad = 12, ChevronBox = 16, SetBlockGap = 8;
    constexpr float CardLeft = 8, CardRight = SetW - 8;
    constexpr auto DrawerTime = 250ms;

    struct SetItem
    {
        SetPart part;
        SetBlock block;
        bool toggle;
    };
    constexpr SetItem SetItems[] = {
        // Everything about the apps' grid in its own drawer (the user's wish, 17.10.2026: before, their
        // sizes were with the menu's and the centring with the groups'); the window's look with its
        // size, under "Pencere" (the transparency and the border were above the drawers, always shown).
        {SetWidthSlider, BlockSize, false},
        {SetHeightSlider, BlockSize, false},
        {SetSlider, BlockSize, false},
        {SetTint, BlockSize, false},
        {SetFolders, BlockSize, true},
        {SetFolderSlider, BlockSize, false},
        {SetBorder, BlockSize, true},
        {SetFixedBar, BlockSize, true},
        {SetTileSlider, BlockTiles, false},
        {SetGapSlider, BlockTiles, false},
        {SetIconSlider, BlockTiles, false},
        {SetAppNames, BlockTiles, true},
        {SetHighlightTiles, BlockTiles, true},
        {SetCenterUngrouped, BlockTiles, true},
        {SetSectionNames, BlockSections, true},
        {SetSectionStyle, BlockSections, false},
        {SetSectionColumns, BlockSections, false},
        {SetSectionScale, BlockSections, false},
    };

    // Where each item is in its block (its label's top; a toggle's row's), and each block's height: the
    // main block's whole, a drawer's under its header. A slider or a choice takes 56 (label, track or
    // buttons, room), a toggle's row 40; a row next to a slider or choice is 8 further off.
    struct SetLayout
    {
        float y[SetPartCount]{};
        float height[BlockCount]{};
    };
    constexpr SetLayout LayOutSettings()
    {
        SetLayout l;
        for (int b = 0; b < BlockCount; b++)
        {
            float y = 0;
            SetItem const *last = nullptr;
            for (auto const &item : SetItems)
            {
                if (item.block != b)
                    continue;
                if (!last)
                    y = b == BlockMain ? 44.f : item.toggle ? 2.f
                                                            : 8.f;
                else
                    y += last->toggle ? (item.toggle ? 40 : 48) : (item.toggle ? 48 : 56);
                l.y[item.part] = y;
                last = &item;
            }
            l.height[b] = !last ? 44.f : last->toggle ? y + 44
                                                      : y + 56; // the main block may be the title alone
        }
        return l;
    }
    constexpr SetLayout SetLayoutOf = LayOutSettings();
    constexpr float LargestDrawer()
    {
        float h = 0;
        for (int b = BlockSize; b < BlockCount; b++)
            h = std::max(h, SetLayoutOf.height[b]);
        return h;
    }
    constexpr float SetH = SetLayoutOf.height[BlockMain] + CardCount * (SetBlockGap + SetHeaderH) + LargestDrawer() + SetBlockGap;

    SetBlock BlockOf(SetPart part)
    {
        if (IsHeader(part))
            return SetBlock(BlockSize + (part - SetSizeHeader));
        for (auto const &item : SetItems)
            if (item.part == part)
                return item.block;
        return BlockMain;
    }
    // An item's text and track run from here to as far from the right; in a drawer, the card's padding.
    float ItemLeft(SetPart part) { return BlockOf(part) == BlockMain ? SetPad : CardLeft + SetCardPad; }
    float ItemRight(SetPart part) { return SetW - ItemLeft(part); }

    // A toggle switch's row: the setting it flips, its text, and its own visuals (see ShowToggle), in
    // its block's visual.
    struct SetToggle
    {
        SetPart part;
        bool Config::*value;
        wchar_t const *label;
        wuc::SpriteVisual hover{nullptr}, trackOn{nullptr}, trackOff{nullptr}, knobOn{nullptr}, knobOff{nullptr};
        wuc::ContainerVisual knob{nullptr}; // holds both knob looks, so they slide together
        wuc::CompositionDrawingSurface knobOnSurface{nullptr}, knobOffSurface{nullptr};
    };
    SetToggle g_setToggles[] = {
        {SetFolders, &Config::folders, L"Klasörler"},
        {SetBorder, &Config::border, L"Dış çerçeve çizgisi"},
        {SetFixedBar, &Config::fixedBar, L"Alt çubuğu sabitle"},
        {SetAppNames, &Config::appNames, L"Uygulama adları"},
        {SetHighlightTiles, &Config::highlightTiles, L"Uygulama arka planı"},
        {SetSectionNames, &Config::sectionNames, L"Grup adları"},
        {SetCenterUngrouped, &Config::centerUngrouped, L"Uygulamaları ortala"},
    };
    // The row lights up under the pointer, its text and switch inset by the pad.
    float TogglePad(SetToggle const &t) { return BlockOf(t.part) == BlockMain ? 12.f : 8.f; }
    D2D1_RECT_F ToggleRow(SetToggle const &t)
    {
        float left = ItemLeft(t.part) - TogglePad(t), y = SetLayoutOf.y[t.part];
        return {left, y, SetW - left, y + 40};
    }

    // A drawer: a card under everything, its header's highlight, text and chevron, and its settings
    // in a visual of their own under the header; the card's visual clips them to what is open of it.
    struct SetCard
    {
        wuc::ContainerVisual visual{nullptr}, content{nullptr};
        wuc::SpriteVisual header{nullptr}, body{nullptr}, hover{nullptr}, chevron{nullptr};
        wuc::ShapeVisual card{nullptr};
        wuc::CompositionRoundedRectangleGeometry shape{nullptr};
        wuc::InsetClip clip{nullptr};
        wuc::CompositionDrawingSurface headerSurface{nullptr}, contentSurface{nullptr};
    };
    SetCard g_setCards[CardCount];

    PopupWindow g_set;
    wuc::SpriteVisual g_setBack{nullptr}, g_setMainSprite{nullptr};
    wuc::ContainerVisual g_setMain{nullptr};
    wuc::CompositionDrawingSurface g_setBackSurface{nullptr}, g_setMainSurface{nullptr};
    bool g_setOpen = false, g_setTracking = false, g_setDragging = false;
    int g_setDrawer = -1;       // the open drawer, -1 for none; all closed at start, then as last left
    D2D1_POINT_2F g_setMouse{}; // the pointer, as last seen over the panel

    // What of a drawer is open: all of its height, or nothing.
    float DrawerShown(int card) { return g_setDrawer == card ? SetLayoutOf.height[BlockSize + card] : 0; }
    // A block's top in the body: stacked up from the panel's bottom, as the drawers are now.
    float BlockY(int block)
    {
        float y = SetH - SetBlockGap;
        for (int card = CardCount - 1; card >= 0; card--)
        {
            y -= SetHeaderH + DrawerShown(card);
            if (block == BlockSize + card)
                return y;
            y -= SetBlockGap;
        }
        return y - SetLayoutOf.height[BlockMain];
    }
    // How far down the body's top is now: all the room the closed drawers leave.
    float SetShift() { return BlockY(BlockMain); }
    // Where an item's block puts its own coordinates' origin: a drawer's under its header.
    float BlockOrigin(SetBlock block) { return BlockY(block) + (block == BlockMain ? 0 : SetHeaderH); }
    D2D1_POINT_2F InBlock(SetPart part, D2D1_POINT_2F p) { return {p.x, p.y - BlockOrigin(BlockOf(part))}; }
    constexpr D2D1_RECT_F HeaderRect{CardLeft, 0, CardRight, SetHeaderH}; // in the card
    SetPart g_setHot = SetNone, g_setPressed = SetNone;
    uint32_t g_accent = 0xFF60CDFF; // Windows' accent, its lighter shade as dark mode uses it on controls

    // The accent palette runs from lightest to darkest; dark mode fills controls with the second one.
    uint32_t AccentColor()
    {
        BYTE palette[32];
        DWORD size = sizeof palette;
        if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Accent", L"AccentPalette",
                         RRF_RT_REG_BINARY, nullptr, palette, &size) == ERROR_SUCCESS &&
            size >= 8)
            return 0xFF000000 | palette[4] << 16 | palette[5] << 8 | palette[6];
        return 0xFF60CDFF;
    }

    void SaveConfig()
    {
        SaveSettings(g_config);
        g_configStamp = ConfigStamp();
    }

    D2D1_RECT_F ToggleRect(SetToggle const &t)
    {
        auto row = ToggleRow(t);
        float cy = (row.top + row.bottom) / 2, pad = TogglePad(t);
        return {row.right - pad - ToggleW, cy - ToggleH / 2, row.right - pad, cy + ToggleH / 2};
    }

    // Each slider's label; its track is SetSliderBelow under it.
    float SliderLabelY(SetPart slider) { return SetLayoutOf.y[slider]; }
    float SliderY(SetPart slider) { return SliderLabelY(slider) + SetSliderBelow; }

    // The choices: under their label, a row of buttons.
    constexpr float ChoiceTop = 22, ChoiceH = 26;
    constexpr wchar_t const *SectionStyleNames[] = {L"Yok", L"Çerçeve", L"Çizgi"};
    constexpr wchar_t const *TileGapNames[] = {L"Sıkı", L"Normal", L"Ferah", L"Çok ferah"};
    constexpr wchar_t const *SectionColumnNames[] = {L"1", L"2", L"3", L"4"};
    constexpr wchar_t const *SectionScaleNames[] = {L"Normal", L"3/4", L"2/3", L"1/2"}; // SectionScales
    int ChoiceCount(SetPart part)
    {
        if (part == SetSectionScale)
            return int(std::size(SectionScaleNames));
        if (part == SetWidthSlider)
            return MenuWidthSteps;
        if (part == SetHeightSlider)
            return MenuHeightSteps;
        if (part == SetSectionColumns)
            return int(std::size(SectionColumnNames));
        if (part == SetTint)
            return int(std::size(TintChoices));
        return part == SetGapSlider ? int(std::size(TileGaps)) : int(std::size(SectionStyleNames));
    }
    int ChoiceValue(SetPart part)
    {
        if (part == SetWidthSlider)
            return g_config.menuWidthStep;
        if (part == SetHeightSlider)
            return g_config.menuHeightStep;
        if (part == SetSectionStyle)
            return g_config.sectionStyle;
        if (part == SetSectionColumns)
            return SectionColumnCount() - 1;
        if (part == SetSectionScale)
            return std::clamp(g_config.sectionScale, 0, int(std::size(SectionScaleNames)) - 1);
        if (part == SetTint)
            return TintIndex();
        return Nearest(TileGaps, g_config.tileGap);
    }
    void SetChoiceValue(SetPart part, int k)
    {
        if (part == SetWidthSlider)
            g_config.menuWidthStep = k;
        else if (part == SetHeightSlider)
            g_config.menuHeightStep = k;
        else if (part == SetSectionStyle)
            g_config.sectionStyle = k;
        else if (part == SetSectionColumns)
            g_config.sectionColumns = k + 1;
        else if (part == SetSectionScale)
            g_config.sectionScale = k;
        else if (part == SetTint)
            g_config.tint = TintSetting(k);
        else
            g_config.tileGap = TileGaps[k];
    }
    wchar_t const *ChoiceName(SetPart part, int k)
    {
        if (part == SetWidthSlider)
            return MenuWidthNames[k];
        if (part == SetHeightSlider)
            return MenuHeightNames[k];
        if (part == SetSectionColumns)
            return SectionColumnNames[k];
        if (part == SetSectionScale)
            return SectionScaleNames[k];
        if (part == SetTint)
            return TintChoices[k].name;
        return part == SetSectionStyle ? SectionStyleNames[k] : TileGapNames[k];
    }
    int g_setChoiceHot = -1; // the button under the pointer in the choice g_setHot is
    // The track's ends.
    float TrackLeft(SetPart slider) { return ItemLeft(slider); }
    float TrackRight(SetPart slider) { return ItemRight(slider); }
    float ChoiceLabelY(SetPart part) { return SetLayoutOf.y[part]; }
    D2D1_RECT_F ChoiceRect(SetPart part)
    {
        float y = ChoiceLabelY(part) + ChoiceTop;
        return {ItemLeft(part), y, ItemRight(part), y + ChoiceH};
    }
    // Each button as wide as its name and an even share of the room left (equal widths cut "Daha geniş").
    float TextWidth(std::wstring_view text, IDWriteTextFormat *format);
    D2D1_RECT_F ChoiceButton(SetPart part, int k)
    {
        auto r = ChoiceRect(part);
        int n = ChoiceCount(part);
        if (part == SetTint) // the colours: round swatches, an even share each
        {
            float w = (r.right - r.left) / n;
            return {r.left + k * w, r.top, r.left + (k + 1) * w, r.bottom};
        }
        float names = 0;
        for (int j = 0; j < n; j++)
            names += TextWidth(ChoiceName(part, j), g_fmtName.Get());
        float share = (r.right - r.left - names) / n, x = r.left;
        for (int j = 0; j < k; j++)
            x += TextWidth(ChoiceName(part, j), g_fmtName.Get()) + share;
        float w = TextWidth(ChoiceName(part, k), g_fmtName.Get()) + share;
        return {std::round(x), r.top, k == n - 1 ? r.right : std::round(x + w), r.bottom};
    }
    int ChoiceAt(SetPart part, float x)
    {
        for (int k = 0; k < ChoiceCount(part) - 1; k++)
            if (x < ChoiceButton(part, k).right)
                return k;
        return ChoiceCount(part) - 1;
    }

    // Where the thumb's centre runs.
    float SliderLeft(SetPart slider) { return TrackLeft(slider) + 10; }
    float SliderRight(SetPart slider) { return TrackRight(slider) - 10; }

    // The size sliders (the menu's width and height, the two icon sizes): the setting each moves, its
    // range and step in DIPs. The menu's goes up to what the screen has room for, and follows a drag
    // DIP by DIP (the user's wish: in steps of 10 it went by jumps); the wheel moves it MenuSizeStep.
    struct SizeSlider
    {
        int Config::*value;
        int min, max, step;
        wchar_t const *label;
    };
    SizeSlider SizeSliderOf(SetPart slider)
    {
        if (slider == SetWidthSlider)
            return {&Config::menuWidth, MenuWidthMin, int(MenuRoom().width), 1, L"Menü genişliği"};
        if (slider == SetHeightSlider)
            return {&Config::menuHeight, MenuHeightMin, int(MenuRoom().height), 1, L"Menü yüksekliği"};
        if (slider == SetFolderSlider)
            return {&Config::folderIconSize, FolderIconMin, FolderIconMax, 1, L"Klasör simgeleri"};
        if (slider == SetTileSlider)
            return {&Config::tileSize, TileSizeMin, TileSizeMax, TileSizeStep, L"Uygulama boyutu"};
        return {&Config::iconSize, IconSizeMin, IconSizeMax, 1, L"Simge boyutu"};
    }

    // The value as the menu has it: a menu size larger than the screen has room for is shown as the largest.
    int SizeValue(SizeSlider const &size) { return std::clamp(g_config.*size.value, size.min, size.max); }

    wchar_t const *SliderLabel(SetPart slider) { return slider == SetSlider ? L"Saydamlık" : SizeSliderOf(slider).label; }

    // The number of steps along the track: transparency in 5% steps, sizes in their own; the last
    // step of the menu's may be shorter, to end at the screen's room.
    int SliderSteps(SetPart slider)
    {
        if (slider == SetSlider)
            return 20;
        auto size = SizeSliderOf(slider);
        return std::max(1, (size.max - size.min + size.step - 1) / size.step);
    }

    // Where the slider's value is along its track, 0 to 1.
    float SliderPosition(SetPart slider)
    {
        if (slider == SetSlider)
            return float(g_config.transparency);
        auto size = SizeSliderOf(slider);
        return size.max > size.min ? float(SizeValue(size) - size.min) / (size.max - size.min) : 0.f;
    }

    std::wstring SliderText(SetPart slider)
    {
        if (slider != SetSlider)
            return std::to_wstring(SizeValue(SizeSliderOf(slider))) + L" px";
        int percent = int(std::round(g_config.transparency * 100));
        return percent ? L"%" + std::to_wstring(percent) : L"Kapalı";
    }

    float ThumbX(SetPart slider) { return SliderLeft(slider) + SliderPosition(slider) * (SliderRight(slider) - SliderLeft(slider)); }

    // The step under x, as a whole number of steps.
    int SliderStepAt(SetPart slider, float x)
    {
        float t = std::clamp((x - SliderLeft(slider)) / (SliderRight(slider) - SliderLeft(slider)), 0.f, 1.f);
        return int(std::round(t * SliderSteps(slider)));
    }

    SetToggle *ToggleOf(SetPart part)
    {
        for (auto &t : g_setToggles)
            if (t.part == part)
                return &t;
        return nullptr;
    }

    // What is under p (body DIPs, as the blocks are now: what is in a closed drawer is not there).
    SetPart SettingsPartAt(D2D1_POINT_2F p)
    {
        for (int card = 0; card < CardCount; card++)
            if (Inside(HeaderRect, {p.x, p.y - BlockY(BlockSize + card)}))
                return SetPart(SetSizeHeader + card);
        for (auto const &item : SetItems)
        {
            if (item.block != BlockMain && g_setDrawer != item.block - BlockSize)
                continue;
            auto q = InBlock(item.part, p);
            if (q.y < 0 || q.y > SetLayoutOf.height[item.block])
                continue;
            if (item.toggle)
            {
                if (Inside(ToggleRow(*ToggleOf(item.part)), q))
                    return item.part;
            }
            else if (IsChoice(item.part))
            {
                if (Inside(ChoiceRect(item.part), q))
                    return item.part;
            }
            else if (float y = SliderY(item.part); Inside({TrackLeft(item.part), y - 14, TrackRight(item.part), y + 14}, q))
                return item.part;
        }
        return SetNone;
    }

    void DrawSettingsBack() { FillSolidBody(SetW, SetH); }

    // In the bottom right corner of the menu's screen, as far from the screen's edges as the menu is.
    // Its bottom lines up with the menu's, which stays put as the menu's size changes.
    void PlaceSettings()
    {
        RECT work;
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0); // as PlaceWindow places the menu
        int w = int(Px(SetW)), h = int(Px(SetH)), gap = int(Px(ScreenGap));
        PlacePopup(g_set, {work.right - gap - w, work.bottom - gap - h}, SetW, SetH, HWND_TOP);
    }

    // The knob's centre: at the toggle's right end when on, its left end when off.
    float KnobX(SetToggle const &t)
    {
        auto toggle = ToggleRect(t);
        return g_config.*t.value ? toggle.right - ToggleH / 2 : toggle.left + ToggleH / 2;
    }

    // Into its own KnobBox square: dark on the accent when on, light and a little smaller when off;
    // a size larger under the pointer.
    void DrawKnob(SetToggle const &t, bool on)
    {
        float radius = (g_setHot == t.part ? 7.f : 6.f) - (on ? 0 : 1);
        g_dc->FillEllipse(D2D1::Ellipse({KnobBox / 2, KnobBox / 2}, radius, radius), Brush(on ? 0xFF000000 : 0xCDFFFFFF));
    }

    // The toggle's track in a ToggleW x ToggleH box: filled with the accent when on, outlined when off.
    void DrawTrack(bool on)
    {
        D2D1_RECT_F box{0, 0, ToggleW, ToggleH};
        if (on)
        {
            g_dc->FillRoundedRectangle(D2D1::RoundedRect(box, ToggleH / 2, ToggleH / 2), Brush(g_accent));
            return;
        }
        D2D1_RECT_F edge{0.5f, 0.5f, ToggleW - 0.5f, ToggleH - 0.5f};
        g_dc->FillRoundedRectangle(D2D1::RoundedRect(edge, ToggleH / 2 - 0.5f, ToggleH / 2 - 0.5f), Brush(0x19000000));
        g_dc->DrawRoundedRectangle(D2D1::RoundedRect(edge, ToggleH / 2 - 0.5f, ToggleH / 2 - 0.5f), Brush(0x9AFFFFFF), 1);
    }

    // A drawer's chevron, pointing down, in its own ChevronBox square (it turns to point up, open).
    void DrawChevron()
    {
        constexpr float c = ChevronBox / 2;
        auto white = Brush(0xFFFFFFFF);
        g_dc->DrawLine({c - 4.5f, c - 2.25f}, {c, c + 2.25f}, white, 1.25f);
        g_dc->DrawLine({c, c + 2.25f}, {c + 4.5f, c - 2.25f}, white, 1.25f);
    }

    constexpr D2D1_POINT_2F ChevronAt{CardRight - SetCardPad - ChevronBox, (SetHeaderH - ChevronBox) / 2}; // in the card

    // Text from `left` to `right` at y, with a value right-aligned after it in grey.
    void LabelAndValue(wchar_t const *label, std::wstring const &value, float left, float right, float y)
    {
        Text(label, g_fmtRow.Get(), {left, y, right, y + 20}, 0xFFFFFFFF);
        if (value.empty())
            return;
        ComPtr<IDWriteTextLayout> layout;
        if (SUCCEEDED(g_dwrite->CreateTextLayout(value.c_str(), UINT32(value.size()), g_fmtRow.Get(), right - left, 20, &layout)))
        {
            layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
            g_dc->DrawTextLayout({left, y}, layout.Get(), Brush(0xFFC5C5C5));
        }
    }

    // A choice: its label, for the menu's size what it gives (the width with its columns of tiles), and
    // its buttons, the chosen one filled with the accent, the one under the pointer a little lighter.
    // The colours: a round swatch each, in the colour itself (grey a mid grey, Windows' its accent with
    // Windows' four panes on it); the chosen one ringed in white, the one under the pointer faintly.
    void DrawTintChoice()
    {
        auto r = ChoiceRect(SetTint);
        int chosen = TintIndex();
        LabelAndValue(L"Renk", chosen >= 0 ? TintChoices[chosen].name : L"", r.left, r.right, ChoiceLabelY(SetTint));
        for (int k = 0; k < int(std::size(TintChoices)); k++)
        {
            auto b = ChoiceButton(SetTint, k);
            D2D1_POINT_2F c{std::round((b.left + b.right) / 2), std::round((b.top + b.bottom) / 2)};
            uint32_t color = TintChoices[k].color;
            if (color == 0)
                color = 0xFF8A8A8A;
            else if (color == 1)
                color = AccentShade(3) ? AccentShade(3) : g_accent;
            g_dc->FillEllipse(D2D1::Ellipse(c, 9, 9), Brush(color));
            g_dc->DrawEllipse(D2D1::Ellipse(c, 8.5f, 8.5f), Brush(0x26FFFFFF), 1);
            if (TintChoices[k].color == 1)
                for (int q = 0; q < 4; q++)
                {
                    float x = c.x - 3.5f + (q % 2) * 4, y = c.y - 3.5f + (q / 2) * 4;
                    g_dc->FillRectangle({x, y, x + 3, y + 3}, Brush(0xFFFFFFFF));
                }
            if (k == chosen)
                g_dc->DrawEllipse(D2D1::Ellipse(c, 12, 12), Brush(0xFFFFFFFF), 2);
            else if (g_setHot == SetTint && g_setChoiceHot == k)
                g_dc->DrawEllipse(D2D1::Ellipse(c, 11.5f, 11.5f), Brush(0x66FFFFFF), 1);
        }
    }

    void DrawChoice(SetPart part)
    {
        if (part == SetTint)
            return DrawTintChoice();
        auto r = ChoiceRect(part);
        wchar_t const *label = part == SetWidthSlider ? L"Genişlik" : part == SetHeightSlider ? L"Yükseklik"
                                                                  : part == SetGapSlider      ? L"Uygulama aralığı"
                                                                  : part == SetSectionColumns ? L"Grup sütunları"
                                                                  : part == SetSectionScale   ? L"Grup ölçeği"
                                                                                              : L"Grup vurgusu";
        std::wstring value = part == SetWidthSlider
                                 ? std::to_wstring(int(MenuWidthOf(g_config.menuWidthStep))) + L" px · " + std::to_wstring(Columns()) + L" sütun"
                             : part == SetHeightSlider ? std::to_wstring(int(MenuHeightOf(g_config.menuHeightStep))) + L" px"
                                                       : L"";
        LabelAndValue(label, value, r.left, r.right, ChoiceLabelY(part));
        g_dc->FillRoundedRectangle(D2D1::RoundedRect(r, 4, 4), Brush(0x0FFFFFFF));
        D2D1_RECT_F rim{r.left + 0.5f, r.top + 0.5f, r.right - 0.5f, r.bottom - 0.5f};
        g_dc->DrawRoundedRectangle(D2D1::RoundedRect(rim, 3.5f, 3.5f), Brush(0x12FFFFFF), 1);
        for (int k = 0; k < ChoiceCount(part); k++)
        {
            auto b = ChoiceButton(part, k);
            bool chosen = ChoiceValue(part) == k, hot = g_setHot == part && g_setChoiceHot == k;
            D2D1_RECT_F inner{b.left + 2, b.top + 2, b.right - 2, b.bottom - 2};
            if (chosen)
                g_dc->FillRoundedRectangle(D2D1::RoundedRect(inner, 3, 3), Brush(g_accent));
            else if (hot)
                g_dc->FillRoundedRectangle(D2D1::RoundedRect(inner, 3, 3), Brush(0x14FFFFFF));
            ComPtr<IDWriteTextLayout> name;
            wchar_t const *text = ChoiceName(part, k);
            if (SUCCEEDED(g_dwrite->CreateTextLayout(text, UINT32(wcslen(text)), g_fmtName.Get(), b.right - b.left, b.bottom - b.top, &name)))
            {
                name->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                g_dc->DrawTextLayout({b.left, b.top}, name.Get(), Brush(chosen ? 0xFF000000 : 0xFFFFFFFF), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
        }
    }

    // Windows 11's dark slider, at its size.
    void DrawSlider(SetPart slider)
    {
        float y = SliderY(slider), left = TrackLeft(slider), right = TrackRight(slider);
        LabelAndValue(SliderLabel(slider), SliderText(slider), left, right, SliderLabelY(slider));

        // Tick marks under the size tracks, as WinUI draws a slider's ticks: every 4 DIPs of an icon
        // size, the tiles' at each of their steps. Each on a whole pixel so it stays a crisp line.
        if (slider != SetSlider)
        {
            auto range = SizeSliderOf(slider);
            for (int size = range.min; size <= range.max && range.max > range.min; size += std::max(4, range.step))
            {
                float t = float(size - range.min) / (range.max - range.min);
                float x = std::round(SliderLeft(slider) + t * (SliderRight(slider) - SliderLeft(slider)) - 0.5f);
                g_dc->FillRectangle({x, y + 11, x + 1, y + 15}, Brush(0x8BFFFFFF));
            }
        }

        float thumb = ThumbX(slider);
        g_dc->FillRoundedRectangle(D2D1::RoundedRect({left, y - 2, right, y + 2}, 2, 2), Brush(0x8BFFFFFF));
        g_dc->FillRoundedRectangle(D2D1::RoundedRect({left, y - 2, thumb, y + 2}, 2, 2), Brush(g_accent));
        g_dc->FillEllipse(D2D1::Ellipse({thumb, y}, 10, 10), Brush(0xFF454545));
        g_dc->DrawEllipse(D2D1::Ellipse({thumb, y}, 9.5f, 9.5f), Brush(0x17FFFFFF), 1);
        bool hot = g_setHot == slider;
        float inner = hot && g_setDragging ? 5.f : hot ? 7.f
                                                       : 6.f;
        g_dc->FillEllipse(D2D1::Ellipse({thumb, y}, inner, inner), Brush(g_accent));
    }

    // A block's settings, in its own coordinates (the main one with the panel's title).
    void DrawSettingsBlock(SetBlock block)
    {
        if (block == BlockMain)
            Text(L"Ayarlar", g_fmtTitle.Get(), {SetPad, SetTitleY, SetW - SetPad, SetTitleY + 20}, 0xFFFFFFFF);
        for (auto const &item : SetItems)
        {
            if (item.block != block)
                continue;
            if (IsChoice(item.part))
                DrawChoice(item.part);
            else if (IsSlider(item.part))
                DrawSlider(item.part);
            else if (auto *t = ToggleOf(item.part))
            {
                auto row = ToggleRow(*t);
                float pad = TogglePad(*t);
                Text(t->label, g_fmtRow.Get(), {row.left + pad, row.top, row.right - 2 * pad - ToggleW, row.bottom}, 0xFFFFFFFF);
                // Live, the toggle is visuals of its own so it can slide and fade (ShowToggle); only --render draws it here.
                if (!g_compositor)
                {
                    auto toggle = ToggleRect(*t);
                    bool on = g_config.*t->value;
                    auto base = D2D1::Matrix3x2F::Identity();
                    g_dc->GetTransform(&base);
                    g_dc->SetTransform(D2D1::Matrix3x2F::Translation(toggle.left, toggle.top) * base);
                    DrawTrack(on);
                    g_dc->SetTransform(D2D1::Matrix3x2F::Translation(KnobX(*t) - KnobBox / 2, (toggle.top + toggle.bottom - KnobBox) / 2) * base);
                    DrawKnob(*t, on);
                    g_dc->SetTransform(base);
                }
            }
        }
    }

    // A drawer's header: its name, for the size the menu's size now before the chevron.
    void DrawCardHeader(int card)
    {
        D2D1_RECT_F text{HeaderRect.left + SetCardPad, 0, ChevronAt.x - 8, SetHeaderH};
        Text(CardNames[card], g_fmtRow.Get(), text, 0xFFFFFFFF);
        if (card != 0)
            return; // the size's
        std::wstring size = std::to_wstring(int(MenuWidthOf(g_config.menuWidthStep))) + L" × " +
                            std::to_wstring(int(MenuHeightOf(g_config.menuHeightStep)));
        ComPtr<IDWriteTextLayout> layout;
        if (SUCCEEDED(g_dwrite->CreateTextLayout(size.c_str(), UINT32(size.size()), g_fmtRow.Get(), text.right - text.left,
                                                 text.bottom - text.top, &layout)))
        {
            layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
            layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            g_dc->DrawTextLayout({text.left, text.top}, layout.Get(), Brush(0xFFC5C5C5));
        }
    }

    // For --render: the whole panel flattened, as the drawers are now. Live, the cards and chevrons
    // are visuals of their own, so they can follow the drawers.
    void DrawSettingsFront()
    {
        auto at = [](float y)
        { g_dc->SetTransform(D2D1::Matrix3x2F::Translation(0, y)); };
        at(BlockY(BlockMain));
        DrawSettingsBlock(BlockMain);
        for (int card = 0; card < CardCount; card++)
        {
            float y = BlockY(BlockSize + card);
            at(y);
            D2D1_RECT_F box{CardLeft, 0, CardRight, SetHeaderH + DrawerShown(card)};
            g_dc->FillRoundedRectangle(D2D1::RoundedRect(box, 6, 6), Brush(0x0DFFFFFF));
            D2D1_RECT_F rim{box.left + 0.5f, box.top + 0.5f, box.right - 0.5f, box.bottom - 0.5f};
            g_dc->DrawRoundedRectangle(D2D1::RoundedRect(rim, 5.5f, 5.5f), Brush(0x12FFFFFF), 1);
            DrawCardHeader(card);
            g_dc->SetTransform(D2D1::Matrix3x2F::Rotation(g_setDrawer == card ? 180.f : 0.f, {ChevronBox / 2, ChevronBox / 2}) *
                               D2D1::Matrix3x2F::Translation(ChevronAt.x, y + ChevronAt.y));
            DrawChevron();
            if (g_setDrawer != card)
                continue;
            at(y + SetHeaderH);
            DrawSettingsBlock(SetBlock(BlockSize + card));
        }
        g_dc->SetTransform(D2D1::Matrix3x2F::Identity());
    }

    // The panel is small, so it is simply redrawn whole on every change.
    void DrawSettings()
    {
        DrawSurface(g_setBackSurface, SetW, SetH, DrawSettingsBack);
        DrawSurface(g_setMainSurface, SetW, SetLayoutOf.height[BlockMain], []
                    { DrawSettingsBlock(BlockMain); });
        for (int card = 0; card < CardCount; card++)
        {
            auto &c = g_setCards[card];
            DrawSurface(c.headerSurface, SetW, SetHeaderH, [card]
                        { DrawCardHeader(card); });
            DrawSurface(c.contentSurface, SetW, SetLayoutOf.height[BlockSize + card], [card]
                        { DrawSettingsBlock(SetBlock(BlockSize + card)); });
        }
        for (auto const &t : g_setToggles)
        {
            DrawSurface(t.knobOnSurface, KnobBox, KnobBox, [&t]
                        { DrawKnob(t, true); });
            DrawSurface(t.knobOffSurface, KnobBox, KnobBox, [&t]
                        { DrawKnob(t, false); });
        }
    }

    // Opens or closes the drawers: every block slides to its place, the open drawer's card grows as
    // its settings unroll and fade in, the closing one's the other way; the body's top edge and its
    // shadow move with the top block; the chevrons turn.
    void ShowDrawers(bool animate)
    {
        float shift = Px(SetShift()), margin = Px(ShadowMargin), w = Px(SetW), h = Px(SetH);
        auto ease = Bezier(0.1f, 0.9f, 0.2f, 1.f);
        auto set = [&](wuc::CompositionObject const &object, wchar_t const *property, float value, auto apply)
        {
            if (animate)
                Animate(object, property, value, DrawerTime, ease);
            else
            {
                object.StopAnimation(property);
                apply(value);
            }
        };
        set(g_setMain, L"Offset.Y", shift, [&](float v)
            { g_setMain.Offset({0, v, 0}); });
        set(g_set.shape, L"Offset.Y", shift, [&](float v)
            { g_set.shape.Offset({0, v}); });
        set(g_set.shape, L"Size.Y", h - shift, [&](float v)
            { g_set.shape.Size({w, v}); });
        set(g_set.backShape, L"Offset.Y", shift, [&](float v)
            { g_set.backShape.Offset({0, v}); }); // the panel has no inset
        set(g_set.backShape, L"Size.Y", h - shift, [&](float v)
            { g_set.backShape.Size({w, v}); });
        set(g_set.shadow, L"Offset.Y", margin + shift, [&](float v)
            { g_set.shadow.Offset({margin, v, 0}); });
        set(g_set.shadow, L"Size.Y", h - shift, [&](float v)
            { g_set.shadow.Size({w, v}); });
        for (int card = 0; card < CardCount; card++)
        {
            auto &c = g_setCards[card];
            float shown = Px(DrawerShown(card)), full = Px(SetLayoutOf.height[BlockSize + card]), open = shown > 0 ? 1.f : 0.f;
            set(c.visual, L"Offset.Y", Px(BlockY(BlockSize + card)), [&](float v)
                { c.visual.Offset({0, v, 0}); });
            set(c.clip, L"BottomInset", full - shown, [&](float v)
                { c.clip.BottomInset(v); });
            // The card's rim on the pixel's middle, as the flattened drawing has it.
            set(c.shape, L"Size.Y", Px(SetHeaderH) + shown - 1, [&](float v)
                { c.shape.Size({Px(CardRight - CardLeft) - 1, v}); });
            set(c.content, L"Opacity", open, [&](float v)
                { c.content.Opacity(v); });
            set(c.chevron, L"RotationAngleInDegrees", 180 * open, [&](float v)
                { c.chevron.RotationAngleInDegrees(v); });
        }
    }

    void SetSettingsHot(SetPart part);

    void ToggleDrawer(int card)
    {
        g_setDrawer = g_setDrawer == card ? -1 : card;
        ShowDrawers(true);
        SetSettingsHot(SettingsPartAt(g_setMouse)); // the blocks moved from under the pointer
    }

    // The toggle's on and off looks are separate layers, one fading in as the other fades out while
    // the knob slides across, as Windows' toggle does. Each animation starts from wherever it is, so
    // a quick second click turns everything around smoothly instead of jumping.
    void ShowToggle(SetToggle const &t, bool animate)
    {
        float on = g_config.*t.value ? 1.f : 0.f, x = Px(KnobX(t) - KnobBox / 2);
        std::pair<wuc::Visual, float> targets[] = {
            {t.trackOn, on}, {t.trackOff, 1 - on}, {t.knobOn, on}, {t.knobOff, 1 - on}};
        if (animate)
        {
            auto ease = Bezier(0.1f, 0.9f, 0.2f, 1.f);
            Animate(t.knob, L"Offset.X", x, KnobTime, ease);
            for (auto const &[visual, opacity] : targets)
                Animate(visual, L"Opacity", opacity, KnobTime, ease);
            return;
        }
        auto toggle = ToggleRect(t);
        t.knob.StopAnimation(L"Offset.X");
        t.knob.Offset({x, Px((toggle.top + toggle.bottom - KnobBox) / 2), 0});
        for (auto const &[visual, opacity] : targets)
        {
            visual.StopAnimation(L"Opacity");
            visual.Opacity(opacity);
        }
    }

    void SetSettingsHot(SetPart part)
    {
        if (part == g_setHot)
            return;
        auto fade = [part](SetPart row, wuc::SpriteVisual const &hover)
        {
            if (g_setHot == row || part == row)
                Animate(hover, L"Opacity", part == row ? 1.f : 0.f, HoverTime, Bezier(0.5f, 1.f, 0.89f, 1.f));
        };
        for (int card = 0; card < CardCount; card++)
            fade(SetPart(SetSizeHeader + card), g_setCards[card].hover);
        for (auto const &t : g_setToggles)
            fade(t.part, t.hover);
        g_setHot = part;
        DrawSettings();
    }

    // Live: the menu and the panel take the new transparency at once; it is saved when the drag ends.
    void SetTransparency(double value)
    {
        if (value == g_config.transparency)
            return;
        g_config.transparency = value;
        g_backDirty = true;
        Invalidate();
        UpdateAcrylic(g_set);
        DrawSettings();
    }

    // Live too: the grid relays out at once. Highlights were sized for the old cells, so they go.
    // The menu's size resizes the menu in its window (see g_menuSizing; the panel, in its corner, stays
    // put), drawn at once: its layers are made anew, and must not reach the screen still empty.
    void SetIconSize(SetPart slider, int size)
    {
        int &value = g_config.*SizeSliderOf(slider).value;
        if (size == value)
            return;
        value = size;
        if (IsMenuSizeSlider(slider))
        {
            // Done in Render, once for all the steps of a drag that came in since the last frame.
            g_menuSizing = true;
            g_resizePending = true;
        }
        else
        {
            DropHovers(true);
            ClampScrolls();
        }
        Invalidate();
        DrawSettings();
    }

    // A choice made: the menu shows it at once (a new size in Render, see g_menuSizing), and it is saved.
    void SetMenuStep(SetPart part, int step)
    {
        step = std::clamp(step, 0, ChoiceCount(part) - 1);
        if (step == ChoiceValue(part))
            return;
        SetChoiceValue(part, step);
        if (IsMenuSizeSlider(part))
        {
            g_menuSizing = true;
            g_resizePending = true;
        }
        else if (part == SetGapSlider || part == SetSectionColumns || part == SetSectionScale)
        {
            DropHovers(true); // sized for the old places
            ClampScrolls();
            if (part == SetSectionColumns)
                FixSectionColumns(); // those in a column gone, under the shortest
        }
        else if (part == SetTint)
        {
            g_tint = ReadTint();
            g_backDirty = true; // the menu's acrylic, search box and solid body
            UpdateAcrylic(g_set);
        }
        Invalidate();
        DrawSettings();
        SaveConfig();
    }

    void SetSliderStep(SetPart slider, int step)
    {
        step = std::clamp(step, 0, SliderSteps(slider));
        if (slider == SetSlider)
            SetTransparency(double(step) / SliderSteps(slider));
        else
        {
            auto size = SizeSliderOf(slider);
            SetIconSize(slider, std::min(size.min + step * size.step, size.max));
        }
    }

    int SliderStep(SetPart slider) { return int(std::round(SliderPosition(slider) * SliderSteps(slider))); }

    // A drag has ended (or was cut off): save.
    void EndSliderDrag()
    {
        g_setDragging = false;
        SaveConfig();
    }

    // Flips a toggle's setting; the menu shows it at once (its border, its bar's buttons' places, or
    // its grid with or without the names).
    void FlipToggle(SetToggle const &t)
    {
        g_config.*t.value = !(g_config.*t.value);
        if (t.value == &Config::appNames || t.value == &Config::sectionNames || t.value == &Config::centerUngrouped ||
            t.value == &Config::folders)
        {
            DropHovers(true); // sized and placed for the old cells
            ClampScrolls();
        }
        Invalidate();
        DrawSettings();
        ShowToggle(t, true);
        SaveConfig();
    }

    // The line between the menu's columns follows the pointer in whole DIPs, held where it was grabbed;
    // each step lays the home view out again (so a drag redraws the menu only when the line moves).
    void DragSplit(float x)
    {
        int width = int(std::round(std::clamp(FolderRight - (x - g_splitGrab) - 12, FolderMinW(), FolderMaxW)));
        if (width == g_config.folderWidth)
            return;
        g_config.folderWidth = width;
        DropHovers(true); // sized and placed for the old layout
        ClampScrolls();
        Invalidate();
    }

    // ---- Reordering pins ----
    //
    // Pressing a pinned app and moving the pointer a little lifts it: the tile goes into a layer of its
    // own that follows the pointer (the compositor moves it; nothing is drawn per move). For the drag
    // every other tile, and every section's name, is a layer of its own too (g_dragTiles, over the apps
    // layer's content), so when where it would drop changes, the compositor slides them to their new
    // places; the others close up around the gap it would drop into. Near the top or bottom edge the
    // grid scrolls by itself. Letting go glides it into the gap and saves the order; Esc glides
    // everything back. A press without a move still opens the app.
    //
    // Where it would drop goes by the lifted app's middle, wherever on it it was grabbed (see "Sections"):
    // - over another app of its section: that one makes way once the middle is past its icon (shrunk by
    //   PastInset on each side) on the side away from the gap: right of it or below it for one after
    //   the gap, left or above for one before (the user's wish: it moved as soon as the edge touched it);
    // - over an app of another section: into that section, beside it (after it over its icon or right
    //   half, before it over its left half);
    // - elsewhere in a section (its name, the room after its last app): to that section's end;
    // - over "+ Yeni bölüm": a new section of its own.
    // A new place is taken only once the pointer has rested on it for RegroupDelay, so what it passes
    // over does not shuffle about; letting go takes it at once.
    // The folder list's rows are reordered the same way, in one column (g_dragFolders).

    constexpr float DragStart = 6;   // how far the pointer moves before a press becomes a drag
    constexpr float DragEdge = 48;   // how near the content's edge the grid starts scrolling
    constexpr auto TileTime = 200ms; // the tiles' slide to a new place
    // "+ Yeni bölüm" comes as a drag starts, rising a little as it fades in, and goes the same way
    // once let go (the user's wish: it popped in and out).
    constexpr auto NewZoneTime = 300ms;
    constexpr float NewZoneRise = 12;
    constexpr ULONGLONG RegroupDelay = 35; // ms the pointer rests on a place before it is taken
    constexpr float PastInset = 0.15f;     // an app makes way once the dragged one is past its icon less this share on each side
    wuc::SpriteVisual g_dragVisual{nullptr};
    std::vector<wuc::SpriteVisual> g_dragTileVisuals; // by pin index; the dragged one's is empty
    int g_settleGeneration = 0;
    int g_renameWhenSettled = 0;            // a section just made by a drop: its name is edited once the tiles settle
    std::vector<GridEntry> g_settleEntries; // let go: the order everything settles in

    // A section's name while dragging, a layer of its own.
    struct DragHeader
    {
        int section = 0;
        wuc::SpriteVisual visual{nullptr};
        bool placed = false;
    };
    std::vector<DragHeader> g_dragHeaders;
    wuc::SpriteVisual g_newZoneVisual{nullptr}, g_newZonePressed{nullptr}; // "+ Yeni bölüm", and as pressed

    // While dragging, each section is shown as "+ Yeni bölüm" is, a faint rounded box behind it, from its
    // name to its last row, without the words (the user's wish), so the places an app can go into
    // stand out. The boxes grow and shrink with their sections: shapes, not drawn surfaces, so they
    // stay sharp at any size.
    struct DragRegion
    {
        int section = 0;
        wuc::ShapeVisual visual{nullptr};
        wuc::CompositionRoundedRectangleGeometry box{nullptr};
        bool placed = false;
    };
    std::vector<DragRegion> g_dragRegions;
    wuc::ContainerVisual g_regionHost{nullptr}; // under the tiles and the names

    AppTarget g_targetPending; // where the pointer rests, not taken yet
    bool g_hasPending = false;
    ULONGLONG g_pendingAt = 0;

    // Where folder `i` belongs while one is dragged: the inverse of PinAtSlot.
    int SlotOfPin(int i)
    {
        if (i == g_dragPin)
            return g_dragSlot;
        int k = i < g_dragPin ? i : i - 1;
        return k < g_dragSlot ? k : k + 1;
    }

    // The list being dragged in: the pinned apps' grid, or the folders' list (g_dragFolders).
    int DragCount() { return int(g_dragFolders ? g_folders.size() : g_pinned.size()); }
    float DragListScroll() { return g_dragFolders ? g_folderScroll : g_scroll; }
    D2D1_RECT_F SlotRect(int slot) { return g_dragFolders ? FolderRect(slot) : PinnedRect(slot); }
    wuc::ContainerVisual DragTiles() { return g_dragFolders ? g_folderTiles : g_dragTiles; }

    // A folder slot's place as if not scrolled; the rows' container takes the scroll.
    winrt::Windows::Foundation::Numerics::float3 SlotPlace(int slot)
    {
        auto r = SlotRect(slot);
        return {Px(r.left), Px(r.top + DragListScroll()), 0};
    }

    wuc::Vector3KeyFrameAnimation SlideTo(winrt::Windows::Foundation::Numerics::float3 to, std::chrono::milliseconds time = TileTime)
    {
        // No key frame at 0: a slide starts from wherever the tile is, so a quick change turns smoothly.
        auto slide = g_compositor.CreateVector3KeyFrameAnimation();
        slide.InsertKeyFrame(1.f, to, Bezier(0.1f, 0.9f, 0.2f, 1.f));
        slide.Duration(time);
        return slide;
    }

    // A tile going into a section of smaller tiles or out of one (SectionScale) grows or shrinks to its
    // new size as it slides there, from its top left; the grid draws it sharp once it has settled.
    void ScaleTo(wuc::Visual const &visual, float scale)
    {
        if (std::abs(scale - 1) < 0.001f)
            return;
        auto grow = g_compositor.CreateVector3KeyFrameAnimation();
        grow.InsertKeyFrame(1.f, {scale, scale, 1}, Bezier(0.1f, 0.9f, 0.2f, 1.f));
        grow.Duration(TileTime);
        visual.StartAnimation(L"Scale", grow);
    }

    // Something drawn once into a sprite of its own, w x h.
    template <class F>
    wuc::SpriteVisual DrawnVisual(float w, float h, F &&draw)
    {
        auto visual = g_compositor.CreateSpriteVisual();
        visual.Brush(PaddedBrush(w, h, std::forward<F>(draw)));
        visual.Size({Px(w), Px(h)});
        return visual;
    }

    // The grid as it is while dragging: with the dragged app where it would drop now; or, let go, as
    // it settles.
    GridLayout DragLayout()
    {
        return g_settling ? LayOutSections(g_settleEntries, false) : LayOutSections(DragEntries(g_dragTarget), true);
    }

    // The sections' boxes to where the layout has them, sliding and resizing as the tiles do. They
    // fade in as the drag starts, a section's out if its last app is dragged away, and all of them,
    // in place, once let go.
    void PlaceDragRegions(GridLayout const &layout, bool slide, std::chrono::milliseconds time = TileTime)
    {
        if (!g_regionHost)
            return;
        using winrt::Windows::Foundation::Numerics::float2;
        auto ease = Bezier(0.1f, 0.9f, 0.2f, 1.f), soft = Bezier(0.2f, 0.f, 0.2f, 1.f);
        auto glide = [&](wuc::CompositionObject const &object, wchar_t const *property, float2 to)
        {
            auto animation = g_compositor.CreateVector2KeyFrameAnimation();
            animation.InsertKeyFrame(1.f, to, ease);
            animation.Duration(time);
            object.StartAnimation(property, animation);
        };
        for (auto &region : g_dragRegions)
            region.placed = false;
        // With the boxes always shown (the setting), they are there from the start and stay through
        // the settling, sliding to the new places; otherwise they come with the drag and go after it.
        bool always = g_config.sectionStyle == 1;
        if (g_pinDragging || g_secDragging || always)
            for (auto const &[section, area] : layout.regions)
            {
                auto r = area;
                if (!section)
                    continue;
                float2 at{Px(r.left), Px(r.top)}, size{Px(r.right - r.left), Px(r.bottom - r.top)};
                auto it = std::find_if(g_dragRegions.begin(), g_dragRegions.end(), [section](DragRegion const &d)
                                       { return d.section == section; });
                if (it == g_dragRegions.end())
                {
                    DragRegion region{section};
                    region.box = g_compositor.CreateRoundedRectangleGeometry();
                    region.box.CornerRadius({Px(RegionRadius), Px(RegionRadius)});
                    region.box.Offset(at);
                    region.box.Size(size);
                    auto shape = g_compositor.CreateSpriteShape(region.box);
                    shape.FillBrush(g_compositor.CreateColorBrush({uint8_t(RegionFill >> 24), 255, 255, 255}));
                    region.visual = g_compositor.CreateShapeVisual();
                    region.visual.Size({Px(MenuW), 100000}); // room for the whole list; the shape is what shows
                    region.visual.Shapes().Append(shape);
                    region.visual.Opacity(always && !slide ? 1.f : 0.f);
                    g_regionHost.Children().InsertAtTop(region.visual);
                    g_dragRegions.push_back(region);
                    it = g_dragRegions.end() - 1;
                }
                else if (slide)
                {
                    glide(it->box, L"Offset", at);
                    glide(it->box, L"Size", size);
                }
                else
                {
                    it->box.Offset(at);
                    it->box.Size(size);
                }
                Animate(it->visual, L"Opacity", 1, slide ? TileTime : NewZoneTime, slide ? ease : soft);
                it->placed = true;
            }
        for (auto &region : g_dragRegions)
            if (!region.placed)
                Animate(region.visual, L"Opacity", 0, g_pinDragging || always ? TileTime : NewZoneTime, soft);
    }

    // The sections' names to where the layout has them. One not in it (its last app dragged out)
    // fades out; one new to it (the section a dropped app starts) fades in. "+ Yeni bölüm" goes with
    // the last section, turns pressed while the app is over it, and fades out once let go.
    void PlaceDragHeaders(GridLayout const &layout, bool slide)
    {
        PlaceDragRegions(layout, slide);
        auto ease = Bezier(0.1f, 0.9f, 0.2f, 1.f);
        auto fade = [&](wuc::Visual const &visual, float to)
        {
            if (slide)
                Animate(visual, L"Opacity", to, TileTime, ease);
            else
                visual.Opacity(to);
        };
        auto move = [&](wuc::Visual const &visual, D2D1_RECT_F r)
        {
            winrt::Windows::Foundation::Numerics::float3 at{Px(r.left), Px(r.top), 0};
            if (slide)
                visual.StartAnimation(L"Offset", SlideTo(at));
            else
                visual.Offset(at);
        };
        for (auto &header : g_dragHeaders)
            header.placed = false;
        for (auto const &[section, r] : layout.headers)
        {
            auto it = std::find_if(g_dragHeaders.begin(), g_dragHeaders.end(), [section](DragHeader const &h)
                                   { return h.section == section; });
            if (it == g_dragHeaders.end())
            {
                float w = r.right - r.left, h = r.bottom - r.top;
                std::wstring name = SectionName(section);
                auto visual = DrawnVisual(w, h, [w, h, name]
                                          { DrawSectionHeader({0, 0, w, h}, name); });
                visual.Opacity(0);
                visual.Offset({Px(r.left), Px(r.top), 0});
                DragTiles().Children().InsertAtTop(visual); // over the sections' boxes
                g_dragHeaders.push_back({section, visual});
                it = g_dragHeaders.end() - 1;
            }
            else
                move(it->visual, r);
            fade(it->visual, 1);
            it->placed = true;
        }
        for (auto &header : g_dragHeaders)
            if (!header.placed)
                fade(header.visual, 0);
        if (!g_newZoneVisual)
            return;
        bool shown = layout.newZone.bottom > 0;
        if (!shown)
        {
            // Let go: it sinks a little as it fades, gently; or, a section started in it, it fades where
            // it is, the new section taking its place (the user's wish).
            if (!slide)
                return;
            auto soft = Bezier(0.4f, 0.f, 0.2f, 1.f);
            bool started = g_renameWhenSettled != 0;
            for (auto const &visual : {g_newZoneVisual, g_newZonePressed})
            {
                Animate(visual, L"Opacity", 0, NewZoneTime, soft);
                if (!started)
                    Animate(visual, L"Offset.Y", visual.Offset().y + Px(NewZoneRise), NewZoneTime, soft);
            }
            return;
        }
        for (auto const &visual : {g_newZoneVisual, g_newZonePressed})
            move(visual, layout.newZone);
        fade(g_newZoneVisual, !layout.newZonePressed ? 1.f : 0.f);
        fade(g_newZonePressed, layout.newZonePressed ? 1.f : 0.f);
    }

    // Every tile but the dragged one to its place: sliding there, or at once when the drag starts.
    void PlaceDragTiles(bool slide)
    {
        if (!g_dragFolders)
        {
            auto layout = DragLayout();
            for (int i = 0; i < int(g_dragTileVisuals.size()); i++)
            {
                auto const &visual = g_dragTileVisuals[i];
                if (!visual)
                    continue;
                winrt::Windows::Foundation::Numerics::float3 at{Px(layout.cells[i].left), Px(layout.cells[i].top), 0};
                if (slide)
                    visual.StartAnimation(L"Offset", SlideTo(at));
                else
                    visual.Offset(at);
            }
            PlaceDragHeaders(layout, slide);
            return;
        }
        for (int i = 0; i < int(g_dragTileVisuals.size()); i++)
        {
            if (!g_dragTileVisuals[i])
                continue;
            auto at = SlotPlace(SlotOfPin(i));
            if (slide)
                g_dragTileVisuals[i].StartAnimation(L"Offset", SlideTo(at));
            else
                g_dragTileVisuals[i].Offset(at);
        }
    }

    void ScrollDragTiles() { DragTiles().Offset({0, -Px(DragListScroll()), 0}); }

    // The folder slot under the pointer.
    int SlotAt(D2D1_POINT_2F p)
    {
        return std::clamp(int(std::floor((p.y - GridY + g_folderScroll) / FolderRowH())), 0, int(g_folders.size()) - 1);
    }

    // The lifted tile follows the pointer, held where it was grabbed; a folder's row only up and down.
    void PlaceDragTile()
    {
        if (!g_dragVisual)
            return;
        float h = g_dragFolders ? FolderRowH() : Dims(g_pinned[g_dragPin].group != 0).h;
        float x = g_dragFolders ? FolderLeft() : g_mouse.x - g_dragGrab.x;
        float y = std::clamp(g_mouse.y - g_dragGrab.y, ContentTop - h / 2, ContentBottom - h / 2);
        g_dragVisual.Offset({Px(x), Px(y), 0});
    }

    // A cell's icon, in a section or out of one.
    D2D1_RECT_F IconRect(D2D1_RECT_F cell, bool grouped)
    {
        auto d = Dims(grouped);
        float icon = d.icon, left = std::round((cell.left + cell.right - icon) / 2);
        float top = cell.top + d.iconTop;
        return {left, top, left + icon, top + icon};
    }

    // Where the dragged app would drop with its middle at p (see "Reordering pins").
    AppTarget TargetAt(D2D1_POINT_2F p)
    {
        D2D1_POINT_2F middle{p.x, p.y + g_scroll};
        auto entries = DragEntries(g_dragTarget);
        auto layout = LayOutSections(entries, true);
        std::unordered_map<int, int> count; // each section's apps so far, the dragged one left out
        for (auto const &e : entries)
        {
            // A tile's place takes half the gap round it, so the gaps between tiles are no-man's-land
            // for none (else the app would jump to its section's end there); across, a section's own
            // gap, which may be wider.
            auto tile = layout.cells[e.pin];
            auto d = Dims(e.section != 0);
            float half = d.gap / 2, across = e.section ? (SectionTileStep() - d.w) / 2 : half;
            D2D1_RECT_F cell{tile.left - across, tile.top - half, tile.right + across, tile.bottom + half};
            if (e.pin == g_dragPin)
            {
                if (Inside(cell, middle))
                    return g_dragTarget; // over its own gap: it stays
                continue;
            }
            int k = count[e.section]++;
            if (!Inside(cell, middle))
                continue;
            auto icon = IconRect(tile, e.section != 0);
            if (e.section != g_dragTarget.section)
            {
                bool after = Inside(icon, middle) || middle.x >= (cell.left + cell.right) / 2;
                return {e.section, after ? k + 1 : k};
            }
            float inset = (icon.right - icon.left) * PastInset;
            D2D1_RECT_F core{icon.left + inset, icon.top + inset, icon.right - inset, icon.bottom - inset};
            if (k >= g_dragTarget.index)
                return middle.x >= core.right || middle.y >= core.bottom ? AppTarget{e.section, k + 1} : g_dragTarget;
            return middle.x < core.left || middle.y < core.top ? AppTarget{e.section, k} : g_dragTarget;
        }
        if (Inside(layout.newZone, middle))
            return {NewSection, 0};
        // Above every section's box: into the apps in no section, at their end; with none there, their
        // row slides open for it (and shut again if it goes back down).
        float sectionsTop = FLT_MAX;
        for (auto const &[section, r] : layout.regions)
            if (section)
                sectionsTop = std::min(sectionsTop, r.top);
        if (sectionsTop < FLT_MAX && middle.y < sectionsTop)
            return {0, count[0]};
        // Centred, a short row has room on its left too: there, before the row's first app.
        if (g_config.centerUngrouped)
        {
            int k = 0;
            for (auto const &e : entries)
            {
                if (e.section || e.pin == g_dragPin)
                    continue;
                auto tile = layout.cells[e.pin];
                if (middle.y >= tile.top - TileGap() / 2 && middle.y < tile.bottom + TileGap() / 2)
                {
                    if (middle.x < tile.left)
                        return {0, k};
                    break;
                }
                k++;
            }
        }
        for (auto const &[section, r] : layout.regions)
            if (Inside(r, middle))
                return section == NewSection ? g_dragTarget : AppTarget{section, count[section]};
        return g_dragTarget;
    }

    void MovePinDrag()
    {
        PlaceDragTile();
        if (g_dragFolders)
        {
            if (int slot = SlotAt(g_mouse); slot != g_dragSlot)
            {
                g_dragSlot = slot;
                PlaceDragTiles(true);
            }
            return;
        }
        auto lifted = Dims(g_pinned[g_dragPin].group != 0); // the lifted tile keeps its size till let go
        auto at = TargetAt({g_mouse.x - g_dragGrab.x + lifted.w / 2, g_mouse.y - g_dragGrab.y + lifted.h / 2});
        if (at == g_dragTarget)
        {
            g_hasPending = false;
            return;
        }
        // Taken once the pointer has rested there (the drag's timer looks again meanwhile).
        ULONGLONG now = GetTickCount64();
        if (!g_hasPending || at != g_targetPending)
        {
            g_targetPending = at;
            g_hasPending = true;
            g_pendingAt = now;
            return;
        }
        if (now - g_pendingAt < RegroupDelay)
            return;
        g_dragTarget = at;
        g_hasPending = false;
        PlaceDragTiles(true);
    }

    // A tile (or a folder's row) drawn into a sprite of its own, its slot's size, with a highlight
    // under it if `lifted`.
    wuc::SpriteVisual TileVisual(int pin, bool lifted)
    {
        auto slot = g_dragFolders ? SlotRect(0) : PinnedRect(pin); // a section's tiles may be smaller
        float w = slot.right - slot.left, h = slot.bottom - slot.top;
        return DrawnVisual(w, h, [w, h, pin, lifted]
                           {
        if (lifted) g_dc->FillRoundedRectangle(D2D1::RoundedRect({ 0, 0, w, h }, 6, 6), Brush(0x2EFFFFFF));
        if (g_dragFolders) DrawFolderRow(g_folders[pin], { 0, 0, w, h });
        else DrawTile(g_pinned[pin], { 0, 0, w, h }); });
    }

    void StartPinDrag()
    {
        g_pinDragging = true;
        g_dragSlot = g_dragPin;
        g_hasPending = false;
        if (!g_dragFolders)
        {
            // Where it starts: its section, and its place among that section's apps.
            int k = 0;
            for (int i = 0; i < g_dragPin; i++)
                k += g_pinned[i].group == g_pinned[g_dragPin].group;
            g_dragTarget = {g_pinned[g_dragPin].group, k};
            // The section it comes from keeps its rows till the drag ends.
            int section = g_pinned[g_dragPin].group, count = 0, across = section ? SectionColumnTiles() : Columns();
            for (auto const &tile : g_pinned)
                count += tile.group == section;
            g_dragFromSection = section;
            g_dragFromRows = (count + across - 1) / across;
        }
        g_pressed = KeyNone;
        DropHovers(true);
        g_hot = KeyNone;
        g_glide.start = 0; // the apps layer stops gliding: the tiles below scroll with it at once
        // The lifted tile, a little see-through, over everything. A folder's row has a highlight under
        // it; an app is its icon alone (the user's wish: the highlight showing up as the hover's faded
        // out looked muddled).
        g_dragVisual = TileVisual(g_dragPin, g_dragFolders);
        g_dragVisual.Opacity(0.9f);
        g_main.body.Children().InsertAbove(g_dragVisual, g_frontVisual); // over both columns' layers
        PlaceDragTile();
        // The rest, each in its place, in place of the list's own drawing; for the apps, with the
        // sections' names and "+ Yeni bölüm", which fades in.
        if (!g_dragFolders)
        {
            // Its box's size, as LayOutSections makes it.
            float w = SectionColumnWidth() + 2 * SectionPad, h = SectionNameRowH() + Dims(true).h + 2 * SectionPad;
            g_newZoneVisual = DrawnVisual(w, h, [w, h]
                                          { DrawNewZone({0, 0, w, h}); });
            g_newZonePressed = DrawnVisual(w, h, [w, h]
                                           { DrawNewZone({0, 0, w, h}, true); });
            for (auto const &visual : {g_newZoneVisual, g_newZonePressed})
            {
                visual.Opacity(0);
                DragTiles().Children().InsertAtBottom(visual);
            }
            g_regionHost = g_compositor.CreateContainerVisual();
            DragTiles().Children().InsertAtBottom(g_regionHost);
        }
        g_dragTileVisuals.assign(DragCount(), nullptr);
        for (int i = 0; i < DragCount(); i++)
            if (i != g_dragPin)
            {
                g_dragTileVisuals[i] = TileVisual(i, false);
                DragTiles().Children().InsertAtTop(g_dragTileVisuals[i]);
            }
        PlaceDragTiles(false);
        if (g_newZoneVisual)
        {
            // In from a little below, faded out (placing the tiles put it in full view at once).
            auto soft = Bezier(0.2f, 0.f, 0.2f, 1.f);
            float y = g_newZoneVisual.Offset().y;
            g_newZoneVisual.Offset({g_newZoneVisual.Offset().x, y + Px(NewZoneRise), 0});
            g_newZoneVisual.Opacity(0);
            Animate(g_newZoneVisual, L"Offset.Y", y, NewZoneTime, soft);
            Animate(g_newZoneVisual, L"Opacity", 1, NewZoneTime, soft);
        }
        ScrollDragTiles();
        SetTimer(g_hwnd, TimerDragScroll, 16, nullptr);
        Invalidate();
    }

    // Everything has settled: back to the list's own drawing, in the new order.
    void FinishPinDrag()
    {
        g_main.body.Children().Remove(g_dragVisual);
        g_dragVisual = nullptr;
        DragTiles().Children().RemoveAll();
        g_dragTileVisuals.clear();
        g_dragHeaders.clear();
        g_dragRegions.clear();
        g_regionHost = nullptr;
        g_newZoneVisual = g_newZonePressed = nullptr;
        g_hasPending = false;
        g_settling = false;
        g_dragPin = -1;
        g_dragFolders = false;
        g_dragFromSection = -1;
        SetPins(g_config.pins);
        if (FixSectionColumns())
            SaveConfig(); // a section just started: its column
        UpdateHot();
        Invalidate();
        if (int section = std::exchange(g_renameWhenSettled, 0))
            BeginRename(section);
    }

    // ---- Moving a section ----
    //
    // While the settings panel is open (the menu being arranged, as the column line is), each section's
    // box has a grip at its top right (the user's choice, 16.10.2026, in place of "Yukarı/Aşağı taşı"
    // on its menu). Held, the section lifts and follows the pointer; the others slide out of the way of
    // where it would drop: any column, any place in it (each column is its own list, PlanSections).
    // Let go, it settles there and the place is saved; Esc, or the capture lost, puts it back.
    int g_secFromColumn = 0, g_secFromIndex = 0;
    D2D1_POINT_2F g_secGrab{}; // where in its box it was taken
    D2D1_SIZE_F g_secLiftedSize{};
    wuc::SpriteVisual g_secLifted{nullptr};

    void PlaceLiftedSection()
    {
        if (g_secLifted)
            g_secLifted.Offset({Px(g_mouse.x - g_secGrab.x), Px(g_mouse.y - g_secGrab.y), 0});
    }

    // Every tile, name and box but the lifted section's to where `layout` has them, sliding.
    void SlideSections(GridLayout const &layout)
    {
        for (int i = 0; i < int(g_dragTileVisuals.size()); i++)
            if (g_dragTileVisuals[i])
                g_dragTileVisuals[i].StartAnimation(L"Offset", SlideTo({Px(layout.cells[i].left), Px(layout.cells[i].top), 0}));
        for (auto const &header : g_dragHeaders)
            for (auto const &[s, r] : layout.headers)
                if (s == header.section)
                    header.visual.StartAnimation(L"Offset", SlideTo({Px(r.left), Px(r.top), 0}));
        PlaceDragRegions(layout, true);
    }

    void StartSectionDrag(int region)
    {
        GridLayout layout = PinLayout();
        if (region < 0 || region >= int(layout.regions.size()) || !g_compositor)
            return;
        auto [section, box] = layout.regions[region];
        auto plan = PlanSections(PinEntries(), false);
        for (int c = 0; c < int(plan.size()); c++)
            for (int k = 0; k < int(plan[c].size()); k++)
                if (plan[c][k] == section)
                    g_secFromColumn = c, g_secFromIndex = k;
        EndRename(true);
        g_secDrag = section;
        g_secDragColumn = g_secFromColumn;
        g_secDragIndex = g_secFromIndex;
        g_secDragging = true;
        DropHovers(true);
        g_hot = KeyNone;
        g_glide.start = 0;
        g_settling = true; // the grid's own drawing gives way to the visuals

        // Every other tile and name in a visual of its own where it is; the boxes too (the moved one's
        // staying behind, as where it will drop).
        struct Piece
        {
            int pin;
            D2D1_RECT_F r;
        };
        std::vector<Piece> lifted;
        D2D1_RECT_F header{};
        g_dragTileVisuals.assign(g_pinned.size(), nullptr);
        for (int i = 0; i < int(g_pinned.size()); i++)
        {
            auto r = layout.cells[i];
            if (g_pinned[i].group == section)
            {
                lifted.push_back({i, {r.left - box.left, r.top - box.top, r.right - box.left, r.bottom - box.top}});
                continue;
            }
            g_dragTileVisuals[i] = TileVisual(i, false);
            g_dragTileVisuals[i].Offset({Px(r.left), Px(r.top), 0});
            DragTiles().Children().InsertAtTop(g_dragTileVisuals[i]);
        }
        for (auto const &[s, r] : layout.headers)
        {
            if (s == section)
            {
                header = {r.left - box.left, r.top - box.top, r.right - box.left, r.bottom - box.top};
                continue;
            }
            float w = r.right - r.left, h = r.bottom - r.top;
            std::wstring name = SectionName(s);
            auto visual = DrawnVisual(w, h, [w, h, name]
                                      { DrawSectionHeader({0, 0, w, h}, name); });
            visual.Offset({Px(r.left), Px(r.top), 0});
            DragTiles().Children().InsertAtTop(visual);
            g_dragHeaders.push_back({s, visual});
        }
        g_regionHost = g_compositor.CreateContainerVisual();
        DragTiles().Children().InsertAtBottom(g_regionHost);
        PlaceDragRegions(layout, false);
        ScrollDragTiles();

        // The section itself, lifted: its box, its name and its tiles, over everything.
        float w = box.right - box.left, h = box.bottom - box.top;
        std::wstring name = SectionName(section);
        g_secLifted = DrawnVisual(w, h, [w, h, name, header, lifted]
                                  {
        g_dc->FillRoundedRectangle(D2D1::RoundedRect({ 0, 0, w, h }, RegionRadius, RegionRadius), Brush(0x1FFFFFFF));
        if (header.bottom > header.top) DrawSectionHeader(header, name);
        for (auto const& piece : lifted) DrawTile(g_pinned[piece.pin], piece.r); });
        g_secLifted.Opacity(0.92f);
        g_main.body.Children().InsertAbove(g_secLifted, g_frontVisual);
        g_secLiftedSize = {w, h};
        g_secGrab = {g_mouse.x - box.left, g_mouse.y - (box.top - g_scroll)};
        PlaceLiftedSection();
        SetTimer(g_hwnd, TimerDragScroll, 16, nullptr); // near the view's ends the grid scrolls (DragScrollTick)
        Invalidate();
    }

    // Where it would drop: the column under its box's middle, and in it, under every other section
    // whose middle is above its top (a little in).
    void MoveSectionDrag()
    {
        PlaceLiftedSection();
        float x = g_mouse.x - g_secGrab.x + g_secLiftedSize.width / 2, y = g_mouse.y - g_secGrab.y + g_scroll + 24;
        int c = std::clamp(int((x - GridLeft() + SectionGap / 2) / SectionColumnStep()), 0, SectionColumnCount() - 1);
        auto entries = PinEntries();
        auto layout = LayOutSections(entries, false);
        auto plan = PlanSections(entries, false);
        int k = 0;
        for (int s : plan[c])
        {
            if (s == g_secDrag)
                continue;
            for (auto const &[rs, r] : layout.regions)
                if (rs == s && (r.top + r.bottom) / 2 < y)
                    k++;
        }
        if (c == g_secDragColumn && k == g_secDragIndex)
            return;
        g_secDragColumn = c;
        g_secDragIndex = k;
        SlideSections(LayOutSections(entries, false));
    }

    // ---- Breaking up a section ----
    //
    // "Bölümü dağıt" on its menu (the user's wish): its apps join those in no section, at their end,
    // and it goes. Everything slides from where it was to where it goes, its name and box fading out;
    // the grid draws them again once they are there.
    void DissolveSection(int section)
    {
        if (g_pinDragging || g_settling || g_secDragging)
            return;
        std::vector<AppRef> pins;
        for (auto const &pin : g_config.pins)
            if (!pin.folder && !pin.group)
                pins.push_back(pin);
        for (auto const &pin : g_config.pins)
            if (!pin.folder && pin.group == section)
            {
                pins.push_back(pin);
                pins.back().group = 0;
            }
        for (auto const &pin : g_config.pins)
            if (pin.folder || (pin.group && pin.group != section))
                pins.push_back(pin);

        bool animate = g_compositor && g_visible && !ShowingList();
        GridLayout before = PinLayout();
        std::vector<std::wstring> ids;
        for (auto const &tile : g_pinned)
            ids.push_back(tile.id);
        if (animate)
        {
            // Each tile, name and box where it is now, in place of the grid's own drawing.
            EndRename(true);
            DropHovers(true);
            g_hot = KeyNone;
            g_glide.start = 0;
            g_settling = true;
            g_dragTileVisuals.assign(g_pinned.size(), nullptr);
            for (int i = 0; i < int(g_pinned.size()); i++)
            {
                g_dragTileVisuals[i] = TileVisual(i, false);
                g_dragTileVisuals[i].Offset({Px(before.cells[i].left), Px(before.cells[i].top), 0});
                DragTiles().Children().InsertAtTop(g_dragTileVisuals[i]);
            }
            for (auto const &[s, r] : before.headers)
            {
                float w = r.right - r.left, h = r.bottom - r.top;
                std::wstring name = SectionName(s);
                auto visual = DrawnVisual(w, h, [w, h, name]
                                          { DrawSectionHeader({0, 0, w, h}, name); });
                visual.Offset({Px(r.left), Px(r.top), 0});
                DragTiles().Children().InsertAtTop(visual);
                g_dragHeaders.push_back({s, visual});
            }
            g_regionHost = g_compositor.CreateContainerVisual();
            DragTiles().Children().InsertAtBottom(g_regionHost);
            PlaceDragRegions(before, false); // with the boxes shown always; otherwise none
            ScrollDragTiles();
        }

        g_config.pins = std::move(pins);
        SaveConfig();
        SetPins(g_config.pins);
        ClampScrolls();
        if (!animate)
            return Invalidate();

        // Each to its new place; the section's name fades where it was.
        auto const &after = PinLayout();
        int generation = ++g_settleGeneration;
        auto batch = g_compositor.CreateScopedBatch(wuc::CompositionBatchTypes::Animation);
        auto ease = Bezier(0.1f, 0.9f, 0.2f, 1.f);
        for (int i = 0; i < int(ids.size()); i++)
        {
            auto it = std::find_if(g_pinned.begin(), g_pinned.end(), [&](Tile const &t)
                                   { return t.id == ids[i]; });
            if (it == g_pinned.end())
                continue;
            auto const &r = after.cells[it - g_pinned.begin()];
            g_dragTileVisuals[i].StartAnimation(L"Offset", SlideTo({Px(r.left), Px(r.top), 0}));
            ScaleTo(g_dragTileVisuals[i], (r.right - r.left) / (before.cells[i].right - before.cells[i].left));
        }
        for (auto const &header : g_dragHeaders)
        {
            bool kept = false;
            for (auto const &[s, r] : after.headers)
                if (s == header.section)
                {
                    header.visual.StartAnimation(L"Offset", SlideTo({Px(r.left), Px(r.top), 0}));
                    kept = true;
                }
            if (!kept)
                Animate(header.visual, L"Opacity", 0, TileTime, ease);
        }
        PlaceDragRegions(after, true);
        ScrollDragTiles();
        batch.End();
        batch.Completed([generation](auto &&, auto &&)
                        {
        if (generation != g_settleGeneration) return;
        DragTiles().Children().RemoveAll();
        g_dragTileVisuals.clear();
        g_dragHeaders.clear();
        g_dragRegions.clear();
        g_regionHost = nullptr;
        g_settling = false;
        UpdateHot();
        Invalidate(); });
    }

    void EndSectionDrag(bool drop)
    {
        if (!g_secDragging)
            return;
        g_secDragging = false;
        KillTimer(g_hwnd, TimerDragScroll);
        if (GetCapture() == g_hwnd)
            ReleaseCapture();
        if (!drop)
        {
            g_secDragColumn = g_secFromColumn;
            g_secDragIndex = g_secFromIndex;
        }
        auto entries = PinEntries();
        auto plan = PlanSections(entries, false);
        auto layout = LayOutSections(entries, false);
        if (drop)
        {
            // Each section's column, and the pins in the plan's order: the apps in no section, each
            // column's sections, the folders after.
            std::vector<AppRef> pins;
            for (auto const &pin : g_config.pins)
                if (!pin.folder && !pin.group)
                    pins.push_back(pin);
            for (int c = 0; c < int(plan.size()); c++)
                for (int s : plan[c])
                {
                    SetSectionColumn(s, c);
                    for (auto const &pin : g_config.pins)
                        if (!pin.folder && pin.group == s)
                            pins.push_back(pin);
                }
            for (auto const &pin : g_config.pins)
                if (pin.folder)
                    pins.push_back(pin);
            g_config.pins = std::move(pins);
            SaveConfig();
        }
        int generation = ++g_settleGeneration;
        auto batch = g_compositor.CreateScopedBatch(wuc::CompositionBatchTypes::Animation);
        SlideSections(layout);
        for (auto const &[s, r] : layout.regions)
            if (s == g_secDrag)
                g_secLifted.StartAnimation(L"Offset", SlideTo({Px(r.left), Px(r.top - g_scroll), 0}));
        Animate(g_secLifted, L"Opacity", 1, TileTime, Bezier(0.1f, 0.9f, 0.2f, 1.f));
        batch.End();
        batch.Completed([generation](auto &&, auto &&)
                        {
        if (generation != g_settleGeneration) return;
        g_main.body.Children().Remove(g_secLifted);
        g_secLifted = nullptr;
        DragTiles().Children().RemoveAll();
        g_dragTileVisuals.clear();
        g_dragHeaders.clear();
        g_dragRegions.clear();
        g_regionHost = nullptr;
        g_settling = false;
        g_secDrag = 0;
        SetPins(g_config.pins);
        UpdateHot();
        Invalidate(); });
    }

    // Near the content's top or bottom edge, the grid scrolls that way, faster the nearer. Also takes
    // the place the pointer has rested on long enough, should it not move again.
    void DragScrollTick()
    {
        if (!g_secDragging && !g_dragFolders && g_hasPending)
            MovePinDrag();
        float speed = 0;
        if (g_mouse.y < SplitTop + DragEdge)
            speed = -(SplitTop + DragEdge - g_mouse.y);
        else if (g_mouse.y > SplitBottom - DragEdge)
            speed = g_mouse.y - (SplitBottom - DragEdge);
        if (!speed)
            return;
        // In whole DIPs, as every scroll is, so the tiles stay on the pixels the grid would put them on.
        float &scrolled = g_dragFolders ? g_folderScroll : g_scroll;
        float scroll = std::clamp(std::round(scrolled + std::clamp(speed, -DragEdge, DragEdge) / 3), 0.f,
                                  g_dragFolders ? MaxFolderScroll() : MaxScroll());
        if (scroll == scrolled)
            return;
        scrolled = scroll;
        ScrollDragTiles();
        // A section moved by its grip too (the user's wish: the grid did not scroll under it).
        if (g_secDragging)
            MoveSectionDrag();
        else
            MovePinDrag();
        Invalidate();
    }

    // An app let go: into its gap (in a new section, named "Yeni bölüm", over "+ Yeni bölüm"); or,
    // with Esc, back where it was. The pins are saved in the new order, the folders after them; a
    // section left empty is not saved. Everything glides there, and when it has, the grid draws it again.
    void DropApp(bool drop)
    {
        g_settleEntries = drop ? DragEntries(g_dragTarget) : PinEntries();
        if (drop)
        {
            int id = 1;
            for (auto const &g : g_config.pinGroups)
                id = std::max(id, g.id + 1);
            for (auto const &pin : g_config.pins)
                id = std::max(id, pin.group + 1);
            std::vector<AppRef> pins;
            for (auto const &e : g_settleEntries)
            {
                int section = e.section == NewSection ? id : e.section;
                pins.push_back({g_pinned[e.pin].id, g_pinned[e.pin].name, false, 0, section});
            }
            if (g_dragTarget.section == NewSection)
            {
                g_config.pinGroups.push_back({id, NewSectionName});
                g_renameWhenSettled = id; // named as it starts, once the tiles are in place
            }
            for (auto const &pin : g_config.pins)
                if (pin.folder)
                    pins.push_back(pin);
            g_config.pins = std::move(pins);
            SaveConfig();
        }
        int generation = ++g_settleGeneration;
        auto ease = Bezier(0.1f, 0.9f, 0.2f, 1.f);
        auto batch = g_compositor.CreateScopedBatch(wuc::CompositionBatchTypes::Animation);
        PlaceDragTiles(true);
        auto at = Snapped(DragLayout().cells[g_dragPin], g_scroll);
        g_dragVisual.StartAnimation(L"Offset", SlideTo({Px(at.left), Px(at.top), 0}));
        Animate(g_dragVisual, L"Opacity", 1, TileTime, ease);
        ScaleTo(g_dragVisual, (at.right - at.left) / Dims(g_pinned[g_dragPin].group != 0).w);
        batch.End();
        batch.Completed([generation](auto &&, auto &&)
                        {
        if (generation == g_settleGeneration) FinishPinDrag(); });
    }

    // Let go: what was dragged glides into its place (or, with Esc, everything glides back) and the
    // new order is saved; when it has settled the list draws it again (FinishPinDrag).
    void EndPinDrag(bool drop)
    {
        if (!g_pinDragging)
        {
            if (!g_settling)
                g_dragPin = -1;
            return;
        }
        g_pinDragging = false;
        // Settling before the capture goes: letting it go sends WM_CAPTURECHANGED at once, which
        // would otherwise take this for a press that never became a drag and forget the pin.
        g_settling = true;
        KillTimer(g_hwnd, TimerDragScroll);
        if (GetCapture() == g_hwnd)
            ReleaseCapture();
        if (!g_dragFolders)
        {
            // A place the pointer was on but had not rested on long enough is still where it was let go.
            if (drop && g_hasPending)
                g_dragTarget = g_targetPending;
            return DropApp(drop);
        }
        if (!drop)
            g_dragSlot = g_dragPin;
        else if (g_dragSlot != g_dragPin)
        {
            std::vector<AppRef> apps, folders;
            for (auto const &pin : g_config.pins)
                (pin.folder ? folders : apps).push_back(pin);
            AppRef moved = folders[g_dragPin];
            folders.erase(folders.begin() + g_dragPin);
            folders.insert(folders.begin() + g_dragSlot, moved);
            apps.insert(apps.end(), folders.begin(), folders.end());
            g_config.pins = std::move(apps);
            SaveConfig();
        }
        int generation = ++g_settleGeneration;
        auto batch = g_compositor.CreateScopedBatch(wuc::CompositionBatchTypes::Animation);
        PlaceDragTiles(true);
        auto slot = SlotRect(g_dragSlot);
        g_dragVisual.StartAnimation(L"Offset", SlideTo({Px(slot.left), Px(slot.top), 0}));
        Animate(g_dragVisual, L"Opacity", 1, TileTime, Bezier(0.1f, 0.9f, 0.2f, 1.f));
        batch.End();
        batch.Completed([generation](auto &&, auto &&)
                        {
        if (generation == g_settleGeneration) FinishPinDrag(); });
    }

    // The thumb follows the pointer, held where it was grabbed; the content scrolls to match.
    void DragScroll(float y)
    {
        int bar = g_scrollDragBar;
        auto track = ScrollTrack(bar), thumb = ScrollThumb(bar);
        float room = (track.bottom - track.top) - (thumb.bottom - thumb.top);
        float t = room > 0 ? std::clamp((y - g_scrollGrab - track.top) / room, 0.f, 1.f) : 0;
        float scroll = std::round(t * BarMaxScroll(bar));
        if (scroll == BarScroll(bar))
            return;
        BarScroll(bar) = scroll;
        Invalidate();
    }

    // A press on the thumb grabs it; one elsewhere on the track first brings the thumb's middle there.
    void StartScrollDrag(float y, int bar)
    {
        g_scrollDragBar = bar;
        auto thumb = ScrollThumb(bar);
        bool onThumb = y >= thumb.top && y < thumb.bottom;
        g_scrollGrab = onThumb ? y - thumb.top : (thumb.bottom - thumb.top) / 2;
        g_scrollDragging = true;
        SetCapture(g_hwnd);
        DragScroll(y);
        Invalidate();
    }

    void EndScrollDrag()
    {
        if (!g_scrollDragging)
            return;
        g_scrollDragging = false;
        if (GetCapture() == g_hwnd)
            ReleaseCapture();
        UpdateHot();
        Invalidate();
    }

    void EndSplitDrag()
    {
        if (!g_splitDragging)
            return;
        g_splitDragging = false;
        if (GetCapture() == g_hwnd)
            ReleaseCapture();
        SaveConfig();
        UpdateHot();
        Invalidate();
    }

    void OpenSettings()
    {
        g_setOpen = true;
        Invalidate(); // the menu shows the handle on its column line
        g_setHot = g_setPressed = SetNone;
        g_setDragging = false;
        g_accent = AccentColor();

        int w = int(Px(SetW)), h = int(Px(SetH));
        PlaceSettings();
        UpdateAcrylic(g_set);

        g_setBack.Size({float(w), float(h)});
        g_setBackSurface = CreateSurface(SetW, SetH);
        g_setBack.Brush(g_compositor.CreateSurfaceBrush(g_setBackSurface));
        float mainH = SetLayoutOf.height[BlockMain];
        for (auto visual : {g_setMain.as<wuc::Visual>(), g_setMainSprite.as<wuc::Visual>()})
            visual.Size({float(w), Px(mainH)});
        g_setMainSurface = CreateSurface(SetW, mainH);
        g_setMainSprite.Brush(g_compositor.CreateSurfaceBrush(g_setMainSurface));

        // The chevron, drawn once, turning about its centre.
        auto chevron = CreateSurface(ChevronBox, ChevronBox);
        DrawSurface(chevron, ChevronBox, ChevronBox, DrawChevron);
        D2D1_RECT_F header{HeaderRect.left + 4, HeaderRect.top + 4, HeaderRect.right - 4, HeaderRect.bottom - 4};
        for (int card = 0; card < CardCount; card++)
        {
            auto &c = g_setCards[card];
            float content = SetLayoutOf.height[BlockSize + card];
            for (auto visual : {c.visual.as<wuc::Visual>(), c.card.as<wuc::Visual>()})
                visual.Size({float(w), Px(SetHeaderH + content)});
            // The card, a shape so its height can follow the drawer: a faint lighter panel with a
            // fainter rim, as Windows' settings cards.
            c.shape.CornerRadius({Px(6) - 0.5f, Px(6) - 0.5f});
            c.shape.Offset({Px(CardLeft) + 0.5f, 0.5f});
            c.headerSurface = CreateSurface(SetW, SetHeaderH);
            c.header.Brush(g_compositor.CreateSurfaceBrush(c.headerSurface));
            c.header.Size({float(w), Px(SetHeaderH)});
            c.contentSurface = CreateSurface(SetW, content);
            c.body.Brush(g_compositor.CreateSurfaceBrush(c.contentSurface));
            for (auto visual : {c.content.as<wuc::Visual>(), c.body.as<wuc::Visual>()})
                visual.Size({float(w), Px(content)});
            c.content.Offset({0, Px(SetHeaderH), 0});
            c.chevron.Brush(g_compositor.CreateSurfaceBrush(chevron));
            c.chevron.Size({Px(ChevronBox), Px(ChevronBox)});
            c.chevron.CenterPoint({Px(ChevronBox) / 2, Px(ChevronBox) / 2, 0});
            c.chevron.Offset({Px(ChevronAt.x), Px(ChevronAt.y), 0});
            c.hover.Brush(ShapeFor(header));
            c.hover.Size({Px(header.right - header.left), Px(header.bottom - header.top)});
            c.hover.Offset({Px(header.left), Px(header.top), 0});
            c.hover.Opacity(0);
        }
        ShowDrawers(false);

        for (auto &t : g_setToggles)
        {
            t.knobOnSurface = CreateSurface(KnobBox, KnobBox);
            t.knobOffSurface = CreateSurface(KnobBox, KnobBox);
            t.knobOn.Brush(g_compositor.CreateSurfaceBrush(t.knobOnSurface));
            t.knobOff.Brush(g_compositor.CreateSurfaceBrush(t.knobOffSurface));
            for (auto visual : {t.knob.as<wuc::Visual>(), t.knobOn.as<wuc::Visual>(), t.knobOff.as<wuc::Visual>()})
                visual.Size({Px(KnobBox), Px(KnobBox)});
            // The tracks never change while the panel is open (the accent is read on opening): drawn once.
            auto toggle = ToggleRect(t);
            for (bool on : {true, false})
            {
                auto track = on ? t.trackOn : t.trackOff;
                auto surface = CreateSurface(ToggleW, ToggleH);
                DrawSurface(surface, ToggleW, ToggleH, [on]
                            { DrawTrack(on); });
                track.Brush(g_compositor.CreateSurfaceBrush(surface));
                track.Size({Px(ToggleW), Px(ToggleH)});
                track.Offset({Px(toggle.left), Px(toggle.top), 0});
            }
            auto row = ToggleRow(t);
            t.hover.Brush(ShapeFor(row));
            t.hover.Size({Px(row.right - row.left), Px(row.bottom - row.top)});
            t.hover.Offset({Px(row.left), Px(row.top), 0});
            t.hover.Opacity(0);
        }
        DrawSettings();
        for (auto const &t : g_setToggles)
            ShowToggle(t, false);

        OpenPopup(g_set, {0, SlideDistance}, false); // rising from the taskbar, as the menu does
    }

    void CloseSettings()
    {
        if (!g_setOpen)
            return;
        g_setOpen = false;
        EndSplitDrag();
        if (g_hot == KeySplitter)
            g_hot = KeyNone;
        Invalidate(); // the handle goes; the line is locked again
        if (g_setDragging)
        {
            EndSliderDrag();
            ReleaseCapture();
        }
        g_setHot = g_setPressed = SetNone;
        ClosePopup(g_set, {0, SlideDistance});
    }

    LRESULT CALLBACK SettingsProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
    {
        switch (message)
        {
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_NCHITTEST:
        {
            POINT p{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            ScreenToClient(hwnd, &p);
            auto body = ClientToBody(g_set, p.x, p.y);
            // Above the body's top, with the drawer closed, is not the panel.
            return InBody(g_set, body) && body.y >= SetShift() ? HTCLIENT : HTTRANSPARENT;
        }
        case WM_MOUSEMOVE:
        {
            if (!g_setOpen)
                return 0;
            if (!g_setTracking)
            {
                TRACKMOUSEEVENT track{sizeof track, TME_LEAVE, hwnd, 0};
                g_setTracking = TrackMouseEvent(&track);
            }
            auto p = ClientToBody(g_set, GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
            g_setMouse = p;
            if (g_setDragging)
            {
                SetSliderStep(g_setHot, SliderStepAt(g_setHot, p.x));
                return 0;
            }
            SetPart part = SettingsPartAt(p);
            if (part != g_setHot)
                g_setPressed = SetNone;
            // Over a size choice, which of its buttons.
            int choice = IsChoice(part) ? ChoiceAt(part, p.x) : -1;
            bool redraw = choice != g_setChoiceHot && part == g_setHot;
            g_setChoiceHot = choice;
            SetSettingsHot(part);
            if (redraw)
                DrawSettings();
            return 0;
        }
        case WM_MOUSELEAVE:
            g_setTracking = false;
            if (g_setOpen && !g_setDragging)
                SetSettingsHot(SetNone);
            return 0;
        case WM_LBUTTONDOWN:
        {
            if (!g_setOpen)
                return 0;
            auto p = ClientToBody(g_set, GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
            if (IsSlider(g_setHot))
            {
                // A press anywhere on the track jumps there, and the drag goes on from it.
                g_setDragging = true;
                SetCapture(hwnd);
                SetSliderStep(g_setHot, SliderStepAt(g_setHot, p.x));
                DrawSettings();
            }
            else if (IsHeader(g_setHot) || IsChoice(g_setHot))
                g_setPressed = g_setHot;
            // A toggle's row lights up under the pointer, but only the toggle itself takes a click.
            else
                for (auto const &t : g_setToggles)
                    if (g_setHot == t.part && Inside(ToggleRect(t), InBlock(t.part, p)))
                        g_setPressed = t.part;
            return 0;
        }
        case WM_LBUTTONUP:
            if (g_setDragging)
            {
                EndSliderDrag();
                ReleaseCapture();
                SetSettingsHot(SettingsPartAt(ClientToBody(g_set, GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam))));
                DrawSettings();
            }
            else if (g_setOpen && IsHeader(g_setPressed) &&
                     SettingsPartAt(ClientToBody(g_set, GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam))) == g_setPressed)
                ToggleDrawer(g_setPressed - SetSizeHeader);
            else if (g_setOpen && IsChoice(g_setPressed))
            {
                auto p = ClientToBody(g_set, GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
                if (SettingsPartAt(p) == g_setPressed)
                    SetMenuStep(g_setPressed, ChoiceAt(g_setPressed, p.x));
            }
            else if (g_setOpen)
                for (auto const &t : g_setToggles)
                    if (g_setPressed == t.part && Inside(ToggleRect(t), InBlock(t.part, ClientToBody(g_set, GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)))))
                        FlipToggle(t);
            g_setPressed = SetNone;
            return 0;
        case WM_CAPTURECHANGED:
            if (g_setDragging && reinterpret_cast<HWND>(lparam) != hwnd)
            {
                EndSliderDrag();
                DrawSettings();
            }
            return 0;
        case WM_MOUSEWHEEL:
            // Over a slider the wheel moves it a step, over a size choice to the next one; elsewhere it does nothing.
            if (g_setOpen && IsChoice(g_setHot))
                SetMenuStep(g_setHot, ChoiceValue(g_setHot) + (GET_WHEEL_DELTA_WPARAM(wparam) > 0 ? 1 : -1));
            else if (g_setOpen && IsSlider(g_setHot) && !g_setDragging)
            {
                SetSliderStep(g_setHot, SliderStep(g_setHot) + (GET_WHEEL_DELTA_WPARAM(wparam) > 0 ? 1 : -1));
                SaveConfig();
            }
            return 0;
        case WM_PAINT:
            ValidateRect(hwnd, nullptr);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        }
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }

    HRESULT CreateSettings(HINSTANCE instance)
    {
        // Owned by the menu, so it stays above it; never activated, so the menu keeps the foreground.
        // Opaque: a host backdrop over the menu's kept DWM from renewing the menu's own while the menu
        // changed size; the menu's blur stayed where the menu had been, shifted, until the panel closed
        // (the user saw it; measured with a gradient window behind, 17.10.2026).
        g_set.opaque = true;
        if (HRESULT hr = CreatePopup(g_set, L"MyStartMenuSettings", L"", SettingsProc, WS_EX_NOACTIVATE, g_hwnd, instance); FAILED(hr))
            return hr;
        g_setBack = g_compositor.CreateSpriteVisual();
        g_set.body.Children().InsertAtTop(g_setBack);
        g_setMain = g_compositor.CreateContainerVisual();
        g_setMainSprite = g_compositor.CreateSpriteVisual();
        g_set.body.Children().InsertAtTop(g_setMain);

        // Each drawer: its card under everything, the header's highlight, text and chevron, and its
        // settings under the header, all in the card's visual, clipped to what is open of it.
        for (auto &c : g_setCards)
        {
            c.visual = g_compositor.CreateContainerVisual();
            c.clip = g_compositor.CreateInsetClip();
            c.visual.Clip(c.clip);
            c.card = g_compositor.CreateShapeVisual();
            c.shape = g_compositor.CreateRoundedRectangleGeometry();
            auto card = g_compositor.CreateSpriteShape(c.shape);
            card.FillBrush(g_compositor.CreateColorBrush(winrt::Windows::UI::Color{0x0D, 255, 255, 255}));
            card.StrokeBrush(g_compositor.CreateColorBrush(winrt::Windows::UI::Color{0x12, 255, 255, 255}));
            card.StrokeThickness(1);
            c.card.Shapes().Append(card);
            c.hover = g_compositor.CreateSpriteVisual();
            c.header = g_compositor.CreateSpriteVisual();
            c.chevron = g_compositor.CreateSpriteVisual();
            c.content = g_compositor.CreateContainerVisual();
            c.body = g_compositor.CreateSpriteVisual();
            for (auto visual : {c.card.as<wuc::Visual>(), c.hover.as<wuc::Visual>(), c.header.as<wuc::Visual>(),
                                c.chevron.as<wuc::Visual>(), c.content.as<wuc::Visual>()})
                c.visual.Children().InsertAtTop(visual);
            g_set.body.Children().InsertAtTop(c.visual);
        }

        // Each toggle in its block's visual: the row's highlight under the block's text, the switch over it.
        auto layerOf = [](SetToggle const &t)
        {
            SetBlock block = BlockOf(t.part);
            return block == BlockMain ? g_setMain : g_setCards[block - BlockSize].content;
        };
        for (auto &t : g_setToggles)
        {
            t.hover = g_compositor.CreateSpriteVisual();
            layerOf(t).Children().InsertAtTop(t.hover);
        }
        g_setMain.Children().InsertAtTop(g_setMainSprite);
        for (auto &c : g_setCards)
            c.content.Children().InsertAtTop(c.body);
        for (auto &t : g_setToggles)
        {
            t.trackOff = g_compositor.CreateSpriteVisual();
            t.trackOn = g_compositor.CreateSpriteVisual();
            t.knob = g_compositor.CreateContainerVisual();
            t.knobOff = g_compositor.CreateSpriteVisual();
            t.knobOn = g_compositor.CreateSpriteVisual();
            t.knob.Children().InsertAtTop(t.knobOff);
            t.knob.Children().InsertAtTop(t.knobOn);
            for (auto visual : {t.trackOff.as<wuc::Visual>(), t.trackOn.as<wuc::Visual>(), t.knob.as<wuc::Visual>()})
                layerOf(t).Children().InsertAtTop(visual);
        }
        return S_OK;
    }

    constexpr auto RevealTime = 350ms;

    // Switching between the pinned apps and the every-apps list, the new one unrolls downward: the
    // compositor uncovers the apps layer top to bottom (its clip's bottom edge sliding down) as it
    // fades in. The search results, which run up from the box, unroll upward the same way (the
    // clip's top edge sliding up; the user's wish). The layer is drawn once, on the next render;
    // nothing is drawn per frame.
    // The apps layer's clip at rest: the scrolling view, below the every-apps list's title row while
    // that is open (AppsViewTop).
    void PlaceAppsClip()
    {
        if (!g_appsClip)
            return;
        g_appsClip.StopAnimation(L"TopInset");
        g_appsClip.StopAnimation(L"BottomInset");
        g_appsClip.TopInset(Px(AppsViewTop()));
        g_appsClip.BottomInset(Px(MenuH - SplitBottom));
    }

    void RevealApps(bool upward)
    {
        if (!g_compositor)
            return;
        if (!g_visible)
            return PlaceAppsClip();
        g_appsClip.StopAnimation(L"TopInset");
        g_appsClip.StopAnimation(L"BottomInset");
        g_appsLayer.StopAnimation(L"Opacity");
        float top = Px(AppsViewTop()), bottom = Px(MenuH - SplitBottom);
        g_appsClip.TopInset(upward ? Px(SplitBottom) : top);
        g_appsClip.BottomInset(upward ? bottom : Px(MenuH) - top);
        g_appsLayer.Opacity(0);
        auto ease = Bezier(0.1f, 0.9f, 0.2f, 1.f);
        if (upward)
            Animate(g_appsClip, L"TopInset", top, RevealTime, ease);
        else
            Animate(g_appsClip, L"BottomInset", bottom, RevealTime, ease);
        Animate(g_appsLayer, L"Opacity", 1, FadeInTime, g_compositor.CreateLinearEasingFunction());
        Invalidate();
    }

    // Opens or closes the every-apps list. Opening it closes the settings panel, whose open state
    // would otherwise make each click in the list just close the panel.
    void ShowAllApps(bool show)
    {
        if (show == g_allApps)
            return;
        if (show)
            CloseSettings();
        g_allApps = show;
        QueryChanged(); // lists every app, or goes back to the home view (or the search)
        RevealApps(SearchView());
        Invalidate();
    }

    void TogglePin(Tile const &app)
    {
        if (auto pin = PinOfApp(app); pin != g_config.pins.end())
            Unpin(std::wstring(pin->id));
        else
            Pin(app);
        Invalidate();
    }

    void Activate(int key)
    {
        if (key == KeySearchBack)
            return ClearSearch();
        if (key == KeyFolderAdd)
            return PickFolder();
        if (key == KeyAllApps)
            return ShowAllApps(!g_allApps);
        if (key >= KeySort && key < KeySort + SortCount)
        {
            if (g_config.appsSort == key - KeySort)
                return;
            g_config.appsSort = key - KeySort;
            SaveConfig();
            QueryChanged(); // the list in its new order, from the top
            return Invalidate();
        }
        if (key >= KeyResult && key - KeyResult < int(g_results.size()) && IsHeading(key - KeyResult))
            return;
        // In the every-apps list a click pins or unpins, so many can be done in a row; the menu stays.
        if (g_allApps && key >= KeyResult && key - KeyResult < int(g_results.size()))
            return TogglePin(g_apps[g_results[key - KeyResult]]);
        if (key >= KeyResult && key - KeyResult < int(g_results.size()))
            LaunchApp(g_apps[g_results[key - KeyResult]].id);
        else if (key == KeyAccount)
            return OpenFlyout(key, FlyoutAlign::Left, AccountItems());
        else if (key == KeyBar + PowerButton)
            return OpenFlyout(key, FlyoutAlign::Center, PowerItems(UpdatesPendingAtShutdown()));
        else if (key >= KeyBar && key < KeyBar + BarCount)
            OpenTarget(BarActions[key - KeyBar].target);
        else if (Tile *pin = PinOf(key))
            LaunchApp(pin->id);
        else
            return;
        HideMenu();
    }

    void UpdateHot()
    {
        if (g_splitDragging || g_scrollDragging || g_pinDragging)
            return; // what is dragged stays lit, wherever the pointer goes
        int key = HitTest(g_mouse);
        if (key == g_hot)
            return;
        // The handle and the scrollbar are drawn with the content.
        for (int drawn : {KeySplitter, KeyScrollbar, KeyFolderScrollbar})
            if (key == drawn || g_hot == drawn)
                Invalidate();
        if (IsSectionGrip(key) || IsSectionGrip(g_hot))
            Invalidate();
        FadeTo(g_hot, g_hot == g_keyFocus ? 1.f : 0.f); // the arrows' pick stays lit
        g_hot = key;
        FadeTo(key, 1);
    }

    // ---- Keyboard on the home view ----
    //
    // With nothing typed, the arrows move a highlight (the pointer's) among the pinned apps and the
    // folders (the user's wish: open the menu, pick with the arrows, Enter): from the one lit to the
    // nearest that way on the screen, sections and the folder column included; the first press lights
    // the first app. Enter opens it. Typing searches as ever; moving the pointer puts the highlight out.
    bool HomeKeys() { return g_query.empty() && !g_allApps && !g_renaming && !g_pinDragging && !g_secDragging && !g_settling; }

    void SetKeyFocus(int key)
    {
        if (key == g_keyFocus)
            return;
        int old = std::exchange(g_keyFocus, key);
        if (old != KeyNone)
            FadeTo(old, old == g_hot ? 1.f : 0.f);
        if (key != KeyNone)
            FadeTo(key, 1);
    }

    // Scrolls its column so the one lit is in full view.
    void RevealKeyFocus()
    {
        bool folder = g_keyFocus >= KeyFolder && g_keyFocus < KeyFolderAdd;
        auto r = KeyRect(g_keyFocus);
        float &scroll = folder ? g_folderScroll : g_scroll;
        float bottom = folder ? SplitBottom - FolderRowH() : SplitBottom; // the folders' "+" row stays under them
        if (r.top < SplitTop)
            scroll -= SplitTop - r.top + 8;
        else if (r.bottom > bottom)
            scroll += r.bottom - bottom + 8;
        else
            return;
        ClampScrolls();
        g_scroll = std::round(g_scroll);
        g_folderScroll = std::round(g_folderScroll);
        Invalidate();
    }

    void MoveKeyFocus(int dx, int dy)
    {
        std::vector<std::pair<int, D2D1_RECT_F>> items;
        for (int i = 0; i < int(g_pinned.size()); i++)
            items.push_back({KeyPinned + i, PinnedRect(i)});
        for (int i = 0; g_config.folders && i < int(g_folders.size()); i++)
            items.push_back({KeyFolder + i, FolderRect(i)});
        if (items.empty())
            return;
        auto now = std::find_if(items.begin(), items.end(), [](auto const &item)
                                { return item.first == g_keyFocus; });
        if (now == items.end())
        {
            SetKeyFocus(items.front().first);
            return RevealKeyFocus();
        }
        // The nearest ahead that way, going straight counting for more than drifting aside.
        float cx = (now->second.left + now->second.right) / 2, cy = (now->second.top + now->second.bottom) / 2;
        int best = KeyNone;
        float bestScore = FLT_MAX;
        for (auto const &[key, r] : items)
        {
            float x = (r.left + r.right) / 2 - cx, y = (r.top + r.bottom) / 2 - cy;
            float ahead = dx ? x * dx : y * dy, aside = std::abs(dx ? y : x);
            if (ahead <= 1)
                continue;
            float score = ahead + 2 * aside;
            if (score < bestScore)
                bestScore = score, best = key;
        }
        if (best == KeyNone)
            return;
        SetKeyFocus(best);
        RevealKeyFocus();
    }

    void KeepSelectedVisible()
    {
        float top = ListTop() + ResultSlot(g_selected) * RowH;
        if (top - g_scroll < AppsViewTop())
            g_scroll = top - AppsViewTop();
        if (top + RowH - g_scroll > SplitBottom)
            g_scroll = top + RowH - SplitBottom;
        g_scroll = std::clamp(g_scroll, 0.f, MaxScroll());
    }

    // ---- Tray ----

    std::wstring TrayTip() { return g_config.enabled ? L"Başlat Menüm: etkin" : L"Başlat Menüm: duraklatıldı"; }

    void ShowTrayMenu()
    {
        // The look (transparency, border) is set in the menu's own settings panel, where it shows live.
        enum : UINT
        {
            CmdOpen = 1,
            CmdEnabled,
            CmdAutostart,
            CmdExit
        };
        auto check = [](bool on)
        { return on ? MF_CHECKED : MF_UNCHECKED; };

        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, TrayTip().c_str());
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, CmdOpen, L"Menüyü aç");
        AppendMenuW(menu, MF_STRING | check(g_config.enabled), CmdEnabled, L"Win tuşu ve Başlat düğmesi bu menüyü açsın");
        AppendMenuW(menu, MF_STRING | check(AutostartEnabled()), CmdAutostart, L"Başlangıçta çalıştır");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, CmdExit, L"Çıkış");

        // The menu only closes on an outside click if its owner is the foreground window.
        POINT p;
        GetCursorPos(&p);
        SetForegroundWindow(g_hwnd);
        UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, p.x, p.y, 0, g_hwnd, nullptr);
        PostMessageW(g_hwnd, WM_NULL, 0, 0);
        DestroyMenu(menu);

        if (command == CmdOpen)
            ShowMenu();
        else if (command == CmdEnabled)
        {
            g_config.enabled = !g_config.enabled;
            g_hooksEnabled = g_config.enabled;
            SaveConfig();
            UpdateTrayIcon(g_config.enabled, TrayTip());
        }
        else if (command == CmdAutostart)
            SetAutostart(!AutostartEnabled());
        else if (command == CmdExit)
        {
            RemoveTrayIcon();
            DestroyWindow(g_hwnd);
        }
    }

    // ---- Device setup ----

    ComPtr<IDWriteTextFormat> Format(wchar_t const *family, float size, DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL)
    {
        ComPtr<IDWriteTextFormat> format;
        g_dwrite->CreateTextFormat(family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"tr-TR", &format);
        return format;
    }

    wchar_t const *PickFamily(std::initializer_list<wchar_t const *> names)
    {
        ComPtr<IDWriteFontCollection> fonts;
        g_dwrite->GetSystemFontCollection(&fonts);
        for (auto name : names)
        {
            UINT32 index;
            BOOL exists = FALSE;
            fonts->FindFamilyName(name, &index, &exists);
            if (exists)
                return name;
        }
        return L"Segoe UI";
    }

    void CreateFormats()
    {
        auto text = PickFamily({L"Segoe UI Variable Text", L"Segoe UI"});
        auto icons = PickFamily({L"Segoe Fluent Icons", L"Segoe MDL2 Assets"});

        g_fmtTitle = Format(text, 14, DWRITE_FONT_WEIGHT_SEMI_BOLD);

        g_fmtCell = Format(text, 12);
        g_fmtCell->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        g_fmtCell->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        g_dwrite->CreateEllipsisTrimmingSign(g_fmtCell.Get(), &g_ellipsis);
        DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        g_fmtCell->SetTrimming(&trimming, g_ellipsis.Get());

        g_fmtRow = Format(text, 13);
        g_fmtFolder = Format(text, 13);
        g_fmtSearch = Format(text, 14);
        g_fmtName = Format(text, 12);
        for (auto const &format : {g_fmtRow, g_fmtFolder, g_fmtSearch, g_fmtName})
        {
            format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        }
        // Folder names shorten with an ellipsis as their column narrows.
        g_dwrite->CreateEllipsisTrimmingSign(g_fmtFolder.Get(), &g_folderEllipsis);
        g_fmtFolder->SetTrimming(&trimming, g_folderEllipsis.Get());

        g_fmtGlyph = Format(icons, 14);
        g_fmtBarGlyph = Format(icons, 16);
        for (auto const &format : {g_fmtGlyph, g_fmtBarGlyph})
        {
            format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
    }

    HRESULT CreateDevice()
    {
        // WARP draws on the CPU. A hardware device would load the graphics driver into the process,
        // which on this machine (NVIDIA) cost ~40 MB of private memory. We only draw when something
        // changes, never per animation frame, so the CPU is plenty fast.
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                       nullptr, 0, D3D11_SDK_VERSION, &g_d3d, nullptr, nullptr);
        if (FAILED(hr))
            return hr;

        ComPtr<IDXGIDevice> dxgi;
        g_d3d.As(&dxgi);
        if (FAILED(hr = g_d2d->CreateDevice(dxgi.Get(), &g_d2dDevice)))
            return hr;
        if (FAILED(hr = g_d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &g_resources)))
            return hr;
        g_resources->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        g_dc = g_resources;
        return g_resources->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1), &g_brush);
    }

    // The flyout's window and visual tree; its layers are filled in each time it opens.
    HRESULT CreateFlyout(HINSTANCE instance)
    {
        // Owned by the menu, so it always stays above it; never activated, so the menu keeps the keyboard.
        if (HRESULT hr = CreatePopup(g_fly, L"MyStartMenuFlyout", L"", FlyoutProc, WS_EX_NOACTIVATE, g_hwnd, instance); FAILED(hr))
            return hr;
        // Above the blurred backdrop: the tint, the highlights, the content.
        g_flyBack = g_compositor.CreateSpriteVisual();
        g_flyHovers = g_compositor.CreateContainerVisual();
        g_flyFront = g_compositor.CreateSpriteVisual();
        for (auto visual : {g_flyBack.as<wuc::Visual>(), g_flyHovers.as<wuc::Visual>(), g_flyFront.as<wuc::Visual>()})
            g_fly.body.Children().InsertAtTop(visual);
        return S_OK;
    }

    // ---- Input ----

    void OnKeyDown(WPARAM key)
    {
        // While the flyout is open the keyboard is its; other keys do nothing.
        if (g_flyOpen)
        {
            FlyoutKey(key);
            return;
        }
        // While a pin is dragged, Esc puts it back; other keys wait.
        if (g_pinDragging)
        {
            if (key == VK_ESCAPE)
                EndPinDrag(false);
            return;
        }
        // Likewise while a section is moved by its grip.
        if (g_secDragging)
        {
            if (key == VK_ESCAPE)
                EndSectionDrag(false);
            return;
        }
        bool shift = GetKeyState(VK_SHIFT) < 0, ctrl = GetKeyState(VK_CONTROL) < 0;
        // While a section's name is edited the keys are its: Enter keeps the name, Esc the old one.
        if (g_renaming)
        {
            if (key == VK_RETURN || key == VK_ESCAPE)
                EndRename(key == VK_RETURN);
            else
                EditKey(g_nameField, key, shift, ctrl);
            return;
        }
        // With nothing typed, the arrows pick among the pinned apps and folders, Enter opens the pick.
        if (HomeKeys() && !ctrl)
        {
            switch (key)
            {
            case VK_LEFT:
                return MoveKeyFocus(-1, 0);
            case VK_RIGHT:
                return MoveKeyFocus(1, 0);
            case VK_UP:
                return MoveKeyFocus(0, -1);
            case VK_DOWN:
                return MoveKeyFocus(0, 1);
            case VK_RETURN:
                if (g_keyFocus != KeyNone)
                    return Activate(g_keyFocus);
                break;
            }
        }
        if (EditKey(g_search, key, shift, ctrl))
            return;
        switch (key)
        {
        case VK_ESCAPE:
            if (g_setOpen)
                CloseSettings();
            else if (!g_query.empty())
                ClearSearch();
            else if (g_allApps)
                ShowAllApps(false);
            else
                HideMenu();
            break;
        case VK_UP:
        case VK_DOWN:
            if (!g_results.empty())
            {
                // The arrows move on the screen: up is the next result in a list that runs upward.
                int step = (key == VK_DOWN) == !ResultsUpward() ? 1 : -1;
                g_selected = std::clamp(g_selected + step, 0, int(g_results.size()) - 1);
                KeepSelectedVisible();
                UpdateHot();
                Invalidate();
            }
            break;
        case VK_RETURN:
            if (!g_results.empty())
                Activate(KeyResult + g_selected);
            break;
        }
    }

    UINT g_taskbarCreated = 0;

    LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
    {
        switch (message)
        {
        case WM_CHAR:
            // Control characters arrive here too (Backspace, Ctrl+letters); WM_KEYDOWN handles those.
            if (wparam >= 32 && wparam != 127 && !g_flyOpen)
            {
                wchar_t c = wchar_t(wparam);
                ReplaceSelection(Focused(), {&c, 1});
            }
            return 0;
        case WM_KEYDOWN:
            // Not keys we sent ourselves: the Esc that closes a shell flyout (FrontStep) could arrive
            // after the flyout had gone and the menu had the foreground, and close the menu too.
            if (GetMessageExtraInfo() != LPARAM(InjectTag))
                OnKeyDown(wparam);
            return 0;
        case WM_MOUSEMOVE:
        {
            // The pointer moving (not a move sent with it still) puts out the arrows' pick.
            auto at = ClientToBody(g_main, GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
            if (at.x != g_mouse.x || at.y != g_mouse.y)
                SetKeyFocus(KeyNone);
            g_mouse = at;
        }
            if (!g_tracking)
            {
                TRACKMOUSEEVENT track{sizeof track, TME_LEAVE, hwnd, 0};
                g_tracking = TrackMouseEvent(&track);
            }
            if (g_selecting)
                MoveCaret(Focused(), CaretFromPoint(Focused(), g_mouse.x), true);
            if (g_secDragging)
            {
                MoveSectionDrag();
                return 0;
            }
            if (g_splitDragging)
                DragSplit(g_mouse.x);
            if (g_scrollDragging)
                DragScroll(g_mouse.y);
            // A pressed pin lifts once the pointer has moved off it a little. (While a dropped one
            // settles g_dragPin is still set, with the button up: that is no press.)
            if (g_dragPin >= 0 && !g_pinDragging && !g_settling && (wparam & MK_LBUTTON) &&
                std::hypot(g_mouse.x - g_dragFrom.x, g_mouse.y - g_dragFrom.y) > DragStart)
                StartPinDrag();
            if (g_pinDragging)
                MovePinDrag();
            UpdateHot();
            return 0;
        case WM_MOUSELEAVE:
            g_tracking = false;
            g_mouse = {-1, -1};
            UpdateHot();
            return 0;
        case WM_CAPTURECHANGED:
            if (reinterpret_cast<HWND>(lparam) != hwnd)
            {
                EndSplitDrag();
                EndScrollDrag();
                EndPinDrag(false);
                EndSectionDrag(false);
            }
            return 0;
        case WM_LBUTTONDOWN:
            // A click anywhere in the menu just closes an open flyout, like a click outside a popup menu.
            if (g_flyOpen)
            {
                CloseFlyout();
                return 0;
            }
            // The shadow margin is outside the menu as far as the user can tell: close, as a click elsewhere does.
            if (!InBody(g_main, g_mouse))
            {
                HideMenu();
                return 0;
            }
            // A section's name being edited: a press in its box puts the caret there (and drags out a
            // selection); anywhere else keeps the name, and the click goes on as any other.
            if (g_renaming)
            {
                if (Inside(NameBoxRect(), g_mouse))
                {
                    SetCapture(hwnd);
                    g_pressed = KeyNone;
                    g_selecting = true;
                    MoveCaret(g_nameField, CaretFromPoint(g_nameField, g_mouse.x), GetKeyState(VK_SHIFT) < 0);
                    return 0;
                }
                EndRename(true);
            }
            // The scrollbar works whether the settings panel is open or not.
            if (g_hot == KeyScrollbar || g_hot == KeyFolderScrollbar)
            {
                StartScrollDrag(g_mouse.y, g_hot == KeyFolderScrollbar);
                return 0;
            }
            // The column line, movable while the settings panel is open.
            if (g_hot == KeySplitter)
            {
                g_splitDragging = true;
                g_splitGrab = g_mouse.x - FolderSplit();
                g_config.folderWidth = int(FolderWidth()); // from wherever the setting was, into range
                SetCapture(hwnd);
                Invalidate();
                return 0;
            }
            // A section's grip, there while the settings panel is open: the section lifts.
            if (IsSectionGrip(g_hot) && !g_settling && !g_pinDragging)
            {
                SetCapture(hwnd);
                StartSectionDrag(g_hot - KeySectionGrip);
                return 0;
            }
            // Likewise for the settings panel; the every-apps button it shows still opens its list.
            if (g_setOpen && g_hot != KeyAllApps)
            {
                CloseSettings();
                return 0;
            }
            SetCapture(hwnd);
            g_pressed = g_hot;
            FadeTo(g_hot, 0.55f);
            // A pinned app may be dragged to a new place from here (see "Reordering pins").
            // So may a pinned folder, within its list.
            if (((g_hot >= KeyPinned && g_hot < KeyFolder && !ShowingList()) || (g_hot >= KeyFolder && g_hot < KeyFolderAdd)) &&
                !g_settling)
            {
                g_dragFolders = g_hot >= KeyFolder;
                g_dragPin = g_hot - (g_dragFolders ? KeyFolder : KeyPinned);
                g_dragFrom = g_mouse;
                auto r = SlotRect(g_dragPin);
                g_dragGrab = {g_mouse.x - r.left, g_mouse.y - r.top};
            }
            if (g_hot == KeySearch)
            {
                g_selecting = true;
                MoveCaret(g_search, CaretFromPoint(g_search, g_mouse.x), GetKeyState(VK_SHIFT) < 0);
            }
            return 0;
        case WM_LBUTTONUP:
        {
            if (g_secDragging)
            {
                EndSectionDrag(true);
                return 0;
            }
            if (g_splitDragging || g_scrollDragging || g_pinDragging)
            {
                EndSplitDrag();
                EndScrollDrag();
                EndPinDrag(true);
                return 0;
            }
            g_dragPin = -1; // pressed and let go without moving: a click
            g_dragFolders = false;
            ReleaseCapture();
            int pressed = g_pressed;
            g_pressed = KeyNone;
            g_selecting = false;
            if (pressed != KeyNone && pressed == g_hot)
            {
                FadeTo(pressed, 1);
                Activate(pressed);
            }
            return 0;
        }
        case WM_RBUTTONUP:
            // Right-clicking a pin, a search result or a section's name opens its context menu at the
            // pointer (closing any other first). In a name being edited it does nothing.
            if (g_renaming && Inside(NameBoxRect(), g_mouse))
                return 0;
            EndRename(true);
            CloseFlyout();
            CloseSettings();
            if (Tile *pin = PinOf(g_hot))
                OpenFlyoutAtPointer(PinnedItems(*pin));
            else if (g_hot >= KeyResult && g_hot - KeyResult < int(g_results.size()))
                OpenFlyoutAtPointer(ResultItems(g_hot - KeyResult));
            else if (int section = SectionAt(g_mouse))
            {
                if (auto items = SectionItems(section); !items.empty())
                    OpenFlyoutAtPointer(items);
            }
            return 0;
        case WM_MOUSEWHEEL:
        {
            // On the home view the wheel scrolls the column under the pointer. A name being edited is
            // kept first: its caret does not glide with the content. Not while a section is moved.
            if (g_secDragging)
                return 0;
            EndRename(true);
            float delta = GET_WHEEL_DELTA_WPARAM(wparam) / 120.f * 60;
            if (InFolderColumn(g_mouse))
            {
                // The folder list glides there too, as the apps side does.
                float from = GlideNow(g_folderGlide, g_folderDrawnScroll), before = g_folderScroll;
                g_folderScroll = std::clamp(std::round(g_folderScroll - delta), 0.f, MaxFolderScroll());
                if (g_folderScroll != before && g_pinDragging && g_dragFolders)
                {
                    // Mid-drag the rows scroll with the list at once.
                    ScrollDragTiles();
                    MovePinDrag();
                }
                else if (g_folderScroll != before)
                    GlideTo(g_folderGlide, from, g_folderScroll);
            }
            else
            {
                // The apps side glides there, from wherever an earlier step's glide has got to.
                float from = GlideNow(), before = g_scroll;
                g_scroll = std::clamp(std::round(g_scroll - delta), 0.f, MaxScroll()); // whole DIPs (touchpads send fractions)
                if (g_scroll != before && g_pinDragging && !g_dragFolders)
                {
                    // Mid-drag the tiles scroll with the grid at once.
                    ScrollDragTiles();
                    MovePinDrag();
                }
                else if (g_scroll != before)
                    GlideTo(from);
            }
            UpdateHot();
            Invalidate();
            return 0;
        }
        case WM_SETCURSOR:
            if (LOWORD(lparam) == HTCLIENT)
            {
                SetCursor(LoadCursorW(nullptr, g_hot == KeySearch || (g_renaming && Inside(NameBoxRect(), g_mouse)) ? IDC_IBEAM
                                               : g_hot == KeySplitter || g_splitDragging                            ? IDC_SIZEWE
                                               : IsSectionGrip(g_hot) || g_secDragging                              ? IDC_SIZEALL
                                                                                                                    : IDC_ARROW));
                return TRUE;
            }
            break;
        case WM_APPS_CHANGED:
            // Installs come as bursts of file and registry changes; act once they settle.
            SetTimer(hwnd, TimerAppsChanged, 2000, nullptr);
            return 0;
        case WM_TIMER:
            if (wparam == TimerDragScroll)
                DragScrollTick();
            else if (wparam == TimerHideScrollbar || wparam == TimerHideScrollbar + 1)
                HideScrollbar(int(wparam - TimerHideScrollbar));
            else if (wparam == TimerAppsChanged)
            {
                KillTimer(hwnd, TimerAppsChanged);
                // An update may have brought new icons: cached ones from before now get refreshed.
                g_appsGeneration++;
                g_appsChangedAt = FileTimeNow();
                for (auto *tiles : {&g_pinned, &g_folders, &g_apps})
                    for (auto &tile : *tiles)
                        tile.iconPixels = 0;
                FetchApps();
                PrefetchIcons();
                Invalidate();
            }
            return 0;
        case WM_PINS_READY:
            // Windows' pins, read for the menu's first list (LoadPins).
            if (g_seedingPins && !g_config.hasPins)
                AdoptPins(ReadPinList());
            g_seedingPins = false;
            return 0;
        case WM_FOLDER_PICKED:
            FolderPicked();
            return 0;
        case WM_APPS_READY:
            g_fetchingApps = false;
            g_appsFetchedAt = GetTickCount64();
            SetApps(ReadAppList());
            if (ShowingList())
                QueryChanged(); // the results index the old list
            if (g_fetchAgain)
            {
                g_fetchAgain = false;
                FetchApps();
            }
            return 0;
        case WM_ICONS_READY:
            IconsReady();
            return 0;
        case WM_TOGGLE:
            Toggle();
            return 0;
        case WM_START_MOVED:
            StartButtonMoved();
            return 0;
        case WM_PRESSED:
            PressedOutside({GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
            return 0;
        case WM_SHOW_MENU:
            ShowMenu();
            return 0;
        case WM_TRAY:
            if (lparam == WM_LBUTTONUP)
            {
                // Clicking the icon deactivates an open menu first, which already hides it.
                if (GetTickCount64() - g_deactivatedAt > 300)
                    Toggle();
            }
            else if (lparam == WM_RBUTTONUP)
                ShowTrayMenu();
            return 0;
        case WM_ACTIVATE:
            // Like the Windows menu, clicking anywhere else closes it; not while the foreground is held
            // from a closing shell flyout (FrontStep), which takes it back.
            if (LOWORD(wparam) == WA_INACTIVE && g_visible && !g_closing && !g_frontHolding)
            {
                g_deactivatedAt = GetTickCount64();
                HideMenu();
            }
            break;
        case WM_DPICHANGED:
            if (g_visible)
                PlaceWindow();
            return 0;
        case WM_PAINT:
            ValidateRect(hwnd, nullptr);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        }
        if (message == g_taskbarCreated && g_taskbarCreated)
        {
            // Explorer restarted and lost our icon.
            AddTrayIcon(hwnd, WM_TRAY, g_config.enabled, TrayTip());
            return 0;
        }
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }

    // ---- Entry points ----

    int RunMenu(HINSTANCE instance, bool background)
    {
        if (HRESULT hr = CreateDevice(); FAILED(hr))
        {
            Log(L"CreateDevice failed: " + std::to_wstring(hr));
            return 1;
        }
        if (HRESULT hr = CreateComposition(); FAILED(hr))
        {
            Log(L"CreateComposition failed: " + std::to_wstring(hr));
            return 1;
        }
        if (HRESULT hr = CreateMenu(instance); FAILED(hr))
        {
            Log(L"CreateMenu failed: " + std::to_wstring(hr));
            return 1;
        }
        if (HRESULT hr = CreateFlyout(instance); FAILED(hr))
        {
            Log(L"CreateFlyout failed: " + std::to_wstring(hr));
            return 1;
        }
        if (HRESULT hr = CreateSettings(instance); FAILED(hr))
        {
            Log(L"CreateSettings failed: " + std::to_wstring(hr));
            return 1;
        }
        // Not essential: without it the menu works, only the taskbar button does not light up.
        if (HRESULT hr = CreateStartHighlight(instance); FAILED(hr))
            Log(L"CreateStartHighlight failed: " + std::to_wstring(hr));

        StartInput(g_hwnd, WM_TOGGLE, WM_START_MOVED, WM_PRESSED);
        UseDarkMenus();
        g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
        AddTrayIcon(g_hwnd, WM_TRAY, g_config.enabled, TrayTip());
        // Last run's app list right away, a fresh one from the helper shortly after.
        SetApps(ReadAppList());
        LoadPins();
        WatchAppChanges(g_hwnd, WM_APPS_CHANGED);
        FetchApps();
        PrefetchIcons();

        // At sign-in the app starts in the background and waits for the Win key.
        if (!background)
            ShowMenu();

        // Draw only when something changed. Animations need nothing from us: the compositor plays them.
        MSG msg;
        for (;;)
        {
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
            {
                if (msg.message == WM_QUIT)
                    return int(msg.wParam);
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            if (g_dirty && g_visible)
                Render();
            else
                WaitMessage();
        }
    }

    void SavePng(ID2D1Bitmap1 *target, std::wstring const &path)
    {
        auto size = target->GetPixelSize();
        auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                                             D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        ComPtr<ID2D1Bitmap1> cpu;
        g_dc->CreateBitmap(size, nullptr, 0, props, &cpu);
        cpu->CopyFromBitmap(nullptr, target, nullptr);

        D2D1_MAPPED_RECT mapped;
        cpu->Map(D2D1_MAP_OPTIONS_READ, &mapped);
        std::vector<uint8_t> pixels(size_t(size.width) * size.height * 4);
        for (UINT y = 0; y < size.height; y++)
            memcpy(&pixels[size_t(y) * size.width * 4], mapped.bits + size_t(y) * mapped.pitch, size.width * 4);
        cpu->Unmap();
        for (size_t i = 0; i < pixels.size(); i += 4)
            if (uint8_t a = pixels[i + 3]; a && a < 255)
                for (int c = 0; c < 3; c++)
                    pixels[i + c] = uint8_t(std::min(255, pixels[i + c] * 255 / a));

        ComPtr<IWICImagingFactory> wic;
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
        ComPtr<IWICStream> stream;
        wic->CreateStream(&stream);
        stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
        ComPtr<IWICBitmapEncoder> encoder;
        wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
        encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
        ComPtr<IWICBitmapFrameEncode> frame;
        encoder->CreateNewFrame(&frame, nullptr);
        frame->Initialize(nullptr);
        frame->SetSize(size.width, size.height);
        WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
        frame->SetPixelFormat(&format);
        frame->WritePixels(size.height, size.width * 4, UINT(pixels.size()), pixels.data());
        frame->Commit();
        encoder->Commit();
    }

    // Layout check without a window: draws a few states into an offscreen bitmap.
    int RenderToFiles(std::wstring const &folder)
    {
        if (HRESULT hr = CreateDevice(); FAILED(hr))
        {
            Log(L"CreateDevice failed: " + std::to_wstring(hr));
            return 1;
        }
        auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
                                             D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        ComPtr<ID2D1Bitmap1> target;
        g_dc->CreateBitmap(D2D1::SizeU(UINT(MenuW), UINT(MenuH)), nullptr, 0, props, &target);
        g_dc->SetTarget(target.Get());
        SetApps(EnumerateApps());
        SplitPins(g_config.hasPins ? g_config.pins : ReadPinList()); // the menu's own list, or the first copy it would make
        ReadBarShown();
        g_recycleBinFull = RecycleBinFull();
        g_tint = ReadTint();

        DrawAll();
        SavePng(target.Get(), folder + L"\\home.png");

        // The first pin being dragged over the fourth slot: the others close up around the gap.
        if (g_pinned.size() > 3)
        {
            // A section of the second to the fourth apps (unless there are sections already), as made by dropping.
            auto pins = g_config.hasPins ? g_config.pins : ReadPinList();
            auto drawSections = [&]
            {
                DrawAll();
                SavePng(target.Get(), folder + L"\\sections.png");
                // The first section's name being edited, all of it selected, as a new one starts.
                if (!PinLayout().headers.empty())
                {
                    BeginRename(PinLayout().headers[0].first);
                    DrawAll();
                    SavePng(target.Get(), folder + L"\\rename.png");
                    EndRename(false);
                }
            };
            if (std::none_of(pins.begin(), pins.end(), [](AppRef const &pin)
                             { return pin.group; }))
            {
                auto grouped = pins;
                int apps = 0;
                for (auto &pin : grouped)
                    if (!pin.folder && ++apps >= 2 && apps <= 4)
                        pin.group = 999;
                SplitPins(grouped);
                drawSections();
                SplitPins(pins);
            }
            else
                drawSections();

            // The first app being dragged before the fourth: the others close up around the gap.
            g_dragPin = 0;
            g_pinDragging = true;
            g_dragTarget = {g_pinned[0].group, 3};
            DrawAll();
            SavePng(target.Get(), folder + L"\\drag.png");
            // Over "+ Yeni bölüm": pressed, the app's place in it.
            g_dragTarget = {NewSection, 0};
            DrawAll();
            SavePng(target.Get(), folder + L"\\dragnew.png");
            // Likewise the first folder over the third row, the rows closing up around the gap.
            if (g_folders.size() > 2)
            {
                g_dragFolders = true;
                g_dragSlot = 2;
                DrawAll();
                SavePng(target.Get(), folder + L"\\dragfolder.png");
                g_dragFolders = false;
            }
            g_pinDragging = false;
            g_dragPin = -1;
        }

        g_highlights[KeyPinned + 2] = 1;
        g_highlights[KeyBar + 1] = 1; // Settings
        g_highlights[KeyAccount] = 1;
        g_updatesPending = true; // the hover view also shows the power button's update dot
        g_setOpen = true;        // and the column line's handle, as with the settings panel open
        DrawAll();
        SavePng(target.Get(), folder + L"\\hover.png");
        g_highlights.clear();
        g_setOpen = false;

        g_query = L"co";
        g_search.caret = g_search.anchor = g_query.size();
        QueryChanged();
        g_highlights[KeySearchBack] = 1; // the back arrow under the pointer
        DrawAll();
        SavePng(target.Get(), folder + L"\\search.png");
        g_highlights.erase(KeySearchBack);

        g_query.clear();
        g_search.caret = g_search.anchor = 0;
        g_allApps = true;
        for (int sort = 0; sort < SortCount; sort++)
        {
            g_config.appsSort = sort;
            QueryChanged();
            DrawAll();
            SavePng(target.Get(), folder + L"\\allapps" + std::to_wstring(sort) + L".png");
        }
        g_allApps = false;
        g_highlights.clear();

        // The power flyout on its own, second row hovered.
        g_flyItems = PowerItems(true);
        SizeFlyout();
        g_dc->BeginDraw();
        g_dc->Clear(D2D1::ColorF(0, 0, 0, 0));
        DrawFlyoutBack();
        g_dc->FillRoundedRectangle(D2D1::RoundedRect(FlyoutRow(1), 6, 6), Brush(0x17FFFFFF));
        DrawFlyoutFront();
        g_dc->EndDraw();
        SavePng(target.Get(), folder + L"\\flyout.png");

        // The settings panel on its own, slider hovered: the size's drawer open, then the sections', then
        // the apps' (settings.png, settings2.png, settings3.png).
        g_setHot = SetSlider;
        for (int card : {0, 2, 1})
        {
            g_setDrawer = card;
            g_dc->BeginDraw();
            g_dc->Clear(D2D1::ColorF(0, 0, 0, 0));
            g_dc->SetTransform(D2D1::Matrix3x2F::Translation(0, SetShift()));
            FillSolidBody(SetW, SetH - SetShift()); // the body as it is now, under its top
            DrawSettingsFront();
            g_dc->EndDraw();
            SavePng(target.Get(), folder + (card == 2 ? L"\\settings2.png" : card ? L"\\settings3.png"
                                                                                  : L"\\settings.png"));
        }
        return 0;
    }
}

// An exception nothing caught ends the app; it says what it was in the log first (a crash
// otherwise leaves only "abort" in the event log), and where: the calls on the stack, as offsets in
// the exe to look up in a map file (see CLAUDE.md). The stack is not yet unwound here, so it reaches
// down to the throw.
[[noreturn]] void LogTerminate()
{
    void *frames[48];
    USHORT count = CaptureStackBackTrace(0, 48, frames, nullptr);
    auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    std::wstring stack = L"terminate: stack";
    for (USHORT i = 0; i < count; i++)
    {
        HMODULE module = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCWSTR>(frames[i]), &module);
        if (reinterpret_cast<uintptr_t>(module) != base)
            continue; // Windows' own code
        wchar_t rva[16];
        swprintf_s(rva, L" %llX", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(frames[i]) - base));
        stack += rva;
    }
    Log(stack);
    try
    {
        if (auto exception = std::current_exception())
            std::rethrow_exception(exception);
        Log(L"terminate: no exception");
    }
    catch (winrt::hresult_error const &e)
    {
        wchar_t code[16];
        swprintf_s(code, L"%08X", uint32_t(e.code()));
        Log(L"terminate: hresult " + std::wstring(code) + L" " + std::wstring(e.message()));
    }
    catch (std::exception const &e)
    {
        Log(L"terminate: " + std::wstring(e.what(), e.what() + strlen(e.what())));
    }
    catch (...)
    {
        Log(L"terminate: unknown exception");
    }
    abort();
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
    std::set_terminate(LogTerminate);
    winrt::init_apartment(winrt::apartment_type::single_threaded);

    int argc = 0;
    PWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    // The helper (see Shell.h): does one shell job, writes the result to disk and exits.
    if (argc == 3 && std::wstring_view(argv[1]) == L"--apps")
        return RunAppsHelper(argv[2]);
    if (argc == 4 && std::wstring_view(argv[1]) == L"--icons")
        return RunIconsHelper(_wtoi(argv[2]), argv[3]);
    if (argc == 3 && std::wstring_view(argv[1]) == L"--pins")
        return RunPinsHelper(argv[2]);
    if (argc == 3 && std::wstring_view(argv[1]) == L"--pick-folder")
        return RunPickFolderHelper(argv[2]);
    if (argc == 2 && std::wstring_view(argv[1]) == L"--empty-recycle-bin")
        return RunEmptyRecycleBinHelper();

    std::wstring renderFolder;
    bool background = false;
    for (int i = 1; i < argc; i++)
    {
        std::wstring_view arg = argv[i];
        if (arg == L"--render" && i + 1 < argc)
            renderFolder = argv[++i];
        else if (arg == L"--background")
            background = true;
    }
    LocalFree(argv);

    // One copy at a time; starting another just asks the running one to show the menu.
    // (Same names as the WPF version, so the two can never run side by side.)
    HANDLE instanceLock = nullptr;
    if (renderFolder.empty())
    {
        instanceLock = CreateMutexW(nullptr, TRUE, L"Local\\MyStartMenu.Instance");
        if (GetLastError() == ERROR_ALREADY_EXISTS)
        {
            if (HANDLE running = OpenEventW(EVENT_MODIFY_STATE, FALSE, L"Local\\MyStartMenu.Show"))
            {
                SetEvent(running);
                CloseHandle(running);
            }
            return 0;
        }
    }

    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, g_d2d.GetAddressOf());
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown **>(g_dwrite.GetAddressOf()));
    CreateFormats();
    SetUpFields();
    g_configStamp = ConfigStamp();
    bool firstRun = g_configStamp == 0; // no settings file yet
    ApplyConfig(LoadConfig());

    if (!renderFolder.empty())
        return RenderToFiles(renderFolder);

    // On at logon from the first run (the user's wish); turned off from the tray, it stays off.
    // A Run value from an older version moves to the logon task, once.
    if (firstRun || AutostartIsOld())
        SetAutostart(true);

    HANDLE showEvent = CreateEventW(nullptr, FALSE, FALSE, L"Local\\MyStartMenu.Show");
    std::thread([showEvent]
                {
        while (WaitForSingleObject(showEvent, INFINITE) == WAIT_OBJECT_0)
            if (g_hwnd) PostMessageW(g_hwnd, WM_SHOW_MENU, 0, 0); })
        .detach();

    int result = RunMenu(instance, background);
    CloseHandle(instanceLock);
    return result;
}
