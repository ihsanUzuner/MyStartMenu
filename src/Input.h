#pragma once
// System-wide input: the Win key and clicks on the taskbar Start button open our menu instead
// of Windows' own.

#include <windows.h>
#include <atomic>

// Marks input we inject ourselves so the hooks let it through untouched (same tag as the WPF version).
constexpr ULONG_PTR InjectTag = 0x4D53544D;

extern std::atomic<bool> g_hooksEnabled;

// Installs the hooks on their own thread and starts tracking the Start button. Each lone Win
// press or Start button click posts `message` to `notify`; while the menu is open (MenuOpen), each
// move of the button posts `moved`, and each mouse button press anywhere `pressed` (the screen
// point packed in lparam, as in mouse messages).
void StartInput(HWND notify, UINT message, UINT moved, UINT pressed);

// The taskbar Start button's screen rectangle (pixels), as last polled; empty if unknown.
RECT StartButtonRect();

// While the menu is open the button is polled every frame rather than twice a second, and its moves
// and every mouse press are posted.
void MenuOpen(bool open);

// Taps an unassigned key (optionally then releasing `thenRelease`). Windows treats it as user
// input: it lets us take the foreground, and turns a Win press into a combo the shell ignores.
void SendMaskKey(WORD thenRelease = 0);

// Taps Esc, which the foreground window gets (tagged, so our own hook lets it through).
void SendEscape();
