#pragma once
// Notification-area icon. The menu it opens is built by the caller, which owns the settings.

#include <windows.h>
#include <string>

void AddTrayIcon(HWND owner, UINT callbackMessage, bool active, std::wstring const& tip);
void UpdateTrayIcon(bool active, std::wstring const& tip);
void RemoveTrayIcon();

// Lets classic popup menus follow the dark theme, like the rest of the app.
void UseDarkMenus();
