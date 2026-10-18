#include "Tray.h"

#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace
{
NOTIFYICONDATAW g_data{};
HICON g_icon{};

// Four rounded squares, drawn at runtime so the app needs no .ico file. Laid out on a 32-unit
// grid like the WPF version's icon and scaled to the size the taskbar asks for.
HICON MakeIcon(bool active)
{
    DWORD light = 0, size = sizeof light;
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size);
    uint8_t gray = light ? 32 : 255;
    float opacity = active ? 1.f : 120 / 255.f;

    int pixels = GetSystemMetricsForDpi(SM_CXSMICON, GetDpiForSystem());
    float unit = pixels / 32.f;
    constexpr float corners[4][2] = { { 3, 3 }, { 18, 3 }, { 3, 18 }, { 18, 18 } };
    constexpr float side = 11, radius = 3;

    auto inside = [&](float x, float y) {
        for (auto const& c : corners)
        {
            float left = c[0] * unit, top = c[1] * unit, s = side * unit, r = radius * unit;
            float dx = std::max({ left + r - x, 0.f, x - (left + s - r) });
            float dy = std::max({ top + r - y, 0.f, y - (top + s - r) });
            if (x >= left && x <= left + s && y >= top && y <= top + s && dx * dx + dy * dy <= r * r) return true;
        }
        return false;
    };

    BITMAPV5HEADER header{};
    header.bV5Size = sizeof header;
    header.bV5Width = pixels;
    header.bV5Height = -pixels;
    header.bV5Planes = 1;
    header.bV5BitCount = 32;
    header.bV5Compression = BI_BITFIELDS;
    header.bV5RedMask = 0x00FF0000;
    header.bV5GreenMask = 0x0000FF00;
    header.bV5BlueMask = 0x000000FF;
    header.bV5AlphaMask = 0xFF000000;
    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(screen, reinterpret_cast<BITMAPINFO*>(&header), DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);

    // 4x4 samples per pixel for smooth edges.
    auto out = static_cast<uint32_t*>(bits);
    for (int y = 0; y < pixels; y++)
        for (int x = 0; x < pixels; x++)
        {
            int hits = 0;
            for (int sy = 0; sy < 4; sy++)
                for (int sx = 0; sx < 4; sx++) hits += inside(x + (sx + 0.5f) / 4, y + (sy + 0.5f) / 4);
            uint32_t alpha = uint32_t(std::lround(hits / 16.f * opacity * 255));
            uint32_t c = gray * alpha / 255;  // premultiplied
            out[y * pixels + x] = (alpha << 24) | (c << 16) | (c << 8) | c;
        }

    HBITMAP mask = CreateBitmap(pixels, pixels, 1, 1, nullptr);
    ICONINFO info{ TRUE, 0, 0, mask, color };
    HICON icon = CreateIconIndirect(&info);
    DeleteObject(mask);
    DeleteObject(color);
    return icon;
}

void SetIcon(bool active, std::wstring const& tip)
{
    HICON old = g_icon;
    g_icon = MakeIcon(active);
    g_data.hIcon = g_icon;
    wcsncpy_s(g_data.szTip, tip.c_str(), _TRUNCATE);
    if (old) DestroyIcon(old);
}
}

void AddTrayIcon(HWND owner, UINT callbackMessage, bool active, std::wstring const& tip)
{
    g_data = { sizeof g_data };
    g_data.hWnd = owner;
    g_data.uID = 1;
    g_data.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_data.uCallbackMessage = callbackMessage;
    SetIcon(active, tip);
    Shell_NotifyIconW(NIM_ADD, &g_data);
}

void UpdateTrayIcon(bool active, std::wstring const& tip)
{
    SetIcon(active, tip);
    Shell_NotifyIconW(NIM_MODIFY, &g_data);
}

void RemoveTrayIcon()
{
    Shell_NotifyIconW(NIM_DELETE, &g_data);
}

void UseDarkMenus()
{
    // uxtheme's SetPreferredAppMode / FlushMenuThemes: exported by ordinal only, but they are
    // what Explorer and the Windows tools use to get dark context menus.
    HMODULE uxtheme = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!uxtheme) return;
    using SetPreferredAppMode = int(WINAPI*)(int);
    using FlushMenuThemes = void(WINAPI*)();
    constexpr int forceDark = 2;
    if (auto set = reinterpret_cast<SetPreferredAppMode>(GetProcAddress(uxtheme, MAKEINTRESOURCEA(135)))) set(forceDark);
    if (auto flush = reinterpret_cast<FlushMenuThemes>(GetProcAddress(uxtheme, MAKEINTRESOURCEA(136)))) flush();
}
