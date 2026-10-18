#include "Input.h"

#include <UIAutomation.h>
#include <wrl/client.h>

#include <cstdint>
#include <thread>

using Microsoft::WRL::ComPtr;

std::atomic<bool> g_hooksEnabled{ true };

namespace
{
constexpr WORD MaskKey = 0xE8;

HWND g_notify{};
UINT g_message{}, g_movedMessage{}, g_pressedMessage{};

// Set while the menu is open (MenuOpen): the button is followed closely, mouse presses are posted.
std::atomic<bool> g_follow{ false };
HANDLE g_followWake = CreateEventW(nullptr, FALSE, FALSE, nullptr);

// Touched only on the hook thread.
bool g_winDown = false, g_combo = false, g_swallowButtonUp = false;

// The Start button's screen rectangle, packed so the mouse hook can read it without locking.
struct PackedRect { int16_t left, top, right, bottom; };
std::atomic<PackedRect> g_startButton{ PackedRect{} };

bool IsDown(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

void RequestToggle() { PostMessageW(g_notify, g_message, 0, 0); }

bool OnStartButton(POINT p)
{
    auto r = g_startButton.load();
    return r.right > r.left && p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom;
}

LRESULT CALLBACK KeyboardProc(int code, WPARAM wparam, LPARAM lparam)
{
    auto const& key = *reinterpret_cast<KBDLLHOOKSTRUCT const*>(lparam);
    if (code >= 0 && g_hooksEnabled && key.dwExtraInfo != InjectTag)
    {
        bool down = wparam == WM_KEYDOWN || wparam == WM_SYSKEYDOWN;
        if (key.vkCode == VK_LWIN || key.vkCode == VK_RWIN)
        {
            if (down)
            {
                // The async state still shows the key as up on a fresh press, which tells it
                // apart from auto-repeat and recovers from a missed key-up.
                if (!g_winDown || !IsDown(int(key.vkCode)))
                {
                    g_winDown = true;
                    g_combo = IsDown(VK_CONTROL) || IsDown(VK_MENU) || IsDown(VK_SHIFT);
                }
            }
            else if (g_winDown)
            {
                g_winDown = false;
                if (!g_combo)
                {
                    // Swallow the real key-up and replay it after the mask key, so the shell
                    // sees Win+<key> and leaves its own Start menu closed.
                    SendMaskKey(WORD(key.vkCode));
                    RequestToggle();
                    return 1;
                }
            }
        }
        else if (g_winDown)
            g_combo = true;
    }
    return CallNextHookEx(nullptr, code, wparam, lparam);
}

LRESULT CALLBACK MouseProc(int code, WPARAM wparam, LPARAM lparam)
{
    if (code >= 0 && g_hooksEnabled)
    {
        // A click while Win is held is no combo: Windows still opens its menu when Win goes up, so
        // ours must (the user saw Windows' own open when pressing Win and clicking together).
        auto const& mouse = *reinterpret_cast<MSLLHOOKSTRUCT const*>(lparam);
        if (wparam == WM_LBUTTONDOWN && mouse.dwExtraInfo != InjectTag && OnStartButton(mouse.pt))
        {
            g_swallowButtonUp = true;
            RequestToggle();
            return 1;
        }
        if (wparam == WM_LBUTTONUP && g_swallowButtonUp)
        {
            g_swallowButtonUp = false;
            return 1;
        }
        // The menu closes when it loses the foreground; one opened without it (over a taskbar
        // button's jump list) never does, so it is told of presses and closes on one outside it.
        if (g_follow && (wparam == WM_LBUTTONDOWN || wparam == WM_RBUTTONDOWN || wparam == WM_MBUTTONDOWN))
            PostMessageW(g_notify, g_pressedMessage, 0, MAKELPARAM(int16_t(mouse.pt.x), int16_t(mouse.pt.y)));
    }
    return CallNextHookEx(nullptr, code, wparam, lparam);
}

// Low-level hooks run on this thread, which does nothing else, so a busy UI thread can never
// delay system-wide input.
void HookThread()
{
    HINSTANCE module = GetModuleHandleW(nullptr);
    HHOOK keyboard = SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardProc, module, 0);
    HHOOK mouse = SetWindowsHookExW(WH_MOUSE_LL, MouseProc, module, 0);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {}
    UnhookWindowsHookEx(keyboard);
    UnhookWindowsHookEx(mouse);
}

// The button moves on a centered taskbar, and the mouse hook must answer instantly, so the
// rectangle is polled here through UI Automation rather than looked up on click. While the menu is
// open (MenuOpen) it is polled every frame, so its highlight follows the button as taskbar icons come and go.
// (UI Automation gives the button's new place at once, as the taskbar starts sliding it there.)
void TrackStartButton()
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ComPtr<IUIAutomation> automation;
    if (FAILED(CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&automation)))) return;

    VARIANT id{};
    id.vt = VT_BSTR;
    id.bstrVal = SysAllocString(L"StartButton");
    ComPtr<IUIAutomationCondition> isStartButton;
    automation->CreatePropertyCondition(UIA_AutomationIdPropertyId, id, &isStartButton);
    VariantClear(&id);

    ComPtr<IUIAutomationElement> button;
    for (;;)
    {
        if (!button)
            if (HWND taskbar = FindWindowW(L"Shell_TrayWnd", nullptr))
            {
                ComPtr<IUIAutomationElement> root;
                if (SUCCEEDED(automation->ElementFromHandle(taskbar, &root)) && root)
                    root->FindFirst(TreeScope_Descendants, isStartButton.Get(), &button);
            }

        RECT r{};
        PackedRect before = g_startButton.load(), now{};
        if (button && SUCCEEDED(button->get_CurrentBoundingRectangle(&r)) && r.right > r.left)
            now = PackedRect{ int16_t(r.left), int16_t(r.top), int16_t(r.right), int16_t(r.bottom) };
        else
            button.Reset();  // Explorer restarted or the element went away; look it up again next round
        g_startButton = now;
        if (g_follow && (now.left != before.left || now.top != before.top || now.right != before.right || now.bottom != before.bottom))
            PostMessageW(g_notify, g_movedMessage, 0, 0);
        WaitForSingleObject(g_followWake, g_follow ? 16 : 500);
    }
}

INPUT Key(WORD vk, bool up)
{
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = vk;
    input.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
    input.ki.dwExtraInfo = InjectTag;
    return input;
}
}

void SendMaskKey(WORD thenRelease)
{
    INPUT inputs[3] = { Key(MaskKey, false), Key(MaskKey, true), Key(thenRelease, true) };
    SendInput(thenRelease ? 3 : 2, inputs, sizeof(INPUT));
}

void SendEscape()
{
    INPUT inputs[2] = { Key(VK_ESCAPE, false), Key(VK_ESCAPE, true) };
    SendInput(2, inputs, sizeof(INPUT));
}

RECT StartButtonRect()
{
    auto r = g_startButton.load();
    return { r.left, r.top, r.right, r.bottom };
}

void MenuOpen(bool open)
{
    g_follow = open;
    if (open) SetEvent(g_followWake);  // look now: the last poll may be half a second old
}

void StartInput(HWND notify, UINT message, UINT moved, UINT pressed)
{
    g_notify = notify;
    g_message = message;
    g_movedMessage = moved;
    g_pressedMessage = pressed;
    std::thread(HookThread).detach();
    std::thread(TrackStartButton).detach();
}
