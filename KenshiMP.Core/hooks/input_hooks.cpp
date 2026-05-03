#include "input_hooks.h"
#include "../core.h"
#include "kmp/hook_manager.h"
#include <spdlog/spdlog.h>
#include <atomic>
#include <Windows.h>

namespace kmp::input_hooks {

// WndProc handles our UI text input and visible keybinds, but Kenshi also
// consumes keyboard events through OIS. Hook Kenshi's InputHandler so modal
// multiplayer UI can stop those events from reaching game actions.

// Keybinds:
// Tab        - Toggle player list
// Enter      - Toggle chat
// F1         - Toggle connection UI
// Escape     - Close any open overlay panel
// Tilde (~)  - Toggle debug overlay

static bool s_installed = false;

using InputKeyEventFn = void(__fastcall*)(void* inputHandler, int key);

static InputKeyEventFn s_origKeyDown = nullptr;
static InputKeyEventFn s_origKeyUp = nullptr;
static HHOOK s_keyboardHook = nullptr;

static constexpr uintptr_t RVA_INPUT_KEY_DOWN = 0x00360680;
static constexpr uintptr_t RVA_INPUT_KEY_UP = 0x003608F0;
static constexpr int OIS_KC_F1 = 0x3B;

static std::atomic<uint32_t> s_swallowedKeyDown{0};
static std::atomic<uint32_t> s_swallowedKeyUp{0};
static std::atomic<uint32_t> s_lowLevelSwallowed{0};

static bool ShouldCaptureOisKey(int key) {
    if (key == OIS_KC_F1) return true;

    auto& core = Core::Get();
    if (core.GetNativeHud().IsChatInputActive()) return true;
    if (core.GetOverlay().GetNativeMenu().IsVisible()) return true;

    return false;
}

static bool TranslatePrintable(WPARAM vk, LPARAM scanCode, wchar_t& out) {
    BYTE keyState[256] = {};
    if (!GetKeyboardState(keyState)) return false;

    wchar_t buf[8] = {};
    int rc = ToUnicode(static_cast<UINT>(vk),
                       static_cast<UINT>(scanCode),
                       keyState,
                       buf,
                       static_cast<int>(std::size(buf)),
                       0);
    if (rc != 1) return false;
    if (buf[0] < 32) return false;

    out = buf[0];
    return true;
}

static void ToggleNativeMenuFromKeyboard() {
    auto& core = Core::Get();
    auto& nativeMenu = core.GetOverlay().GetNativeMenu();
    if (nativeMenu.IsVisible()) {
        nativeMenu.Hide();
    } else {
        nativeMenu.Show();
    }
}

static LRESULT CALLBACK LowLevelKeyboardProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code < 0) {
        return CallNextHookEx(s_keyboardHook, code, wParam, lParam);
    }

    const auto* kb = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
    if (!kb) {
        return CallNextHookEx(s_keyboardHook, code, wParam, lParam);
    }

    bool keyDown = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
    bool keyUp = (wParam == WM_KEYUP || wParam == WM_SYSKEYUP);
    if (!keyDown && !keyUp) {
        return CallNextHookEx(s_keyboardHook, code, wParam, lParam);
    }

    auto& core = Core::Get();
    auto& hud = core.GetNativeHud();
    auto& menu = core.GetOverlay().GetNativeMenu();
    DWORD vk = kb->vkCode;

    // Own F1 completely so Kenshi's tutorial binding never sees it.
    if (vk == VK_F1) {
        if (keyDown && !(kb->flags & LLKHF_UP)) {
            ToggleNativeMenuFromKeyboard();
        }
        s_lowLevelSwallowed.fetch_add(1, std::memory_order_relaxed);
        return 1;
    }

    if (hud.IsChatInputActive()) {
        if (keyDown) {
            if (vk == VK_RETURN || vk == VK_BACK || vk == VK_ESCAPE) {
                hud.OnChatKeyDown(static_cast<int>(vk));
            } else {
                wchar_t ch = 0;
                if (TranslatePrintable(vk, kb->scanCode, ch)) {
                    hud.OnChatChar(ch);
                }
            }
        }
        s_lowLevelSwallowed.fetch_add(1, std::memory_order_relaxed);
        return 1;
    }

    if (menu.IsVisible()) {
        if (keyDown) {
            if (vk == VK_ESCAPE) {
                menu.OnKeyDown(VK_ESCAPE);
                menu.Hide();
            } else if (vk == VK_BACK || vk == VK_RETURN || vk == VK_TAB) {
                menu.OnKeyDown(static_cast<int>(vk));
            } else if (menu.HasActiveEditBox()) {
                wchar_t ch = 0;
                if (TranslatePrintable(vk, kb->scanCode, ch)) {
                    menu.OnChar(ch);
                }
            }
        }
        s_lowLevelSwallowed.fetch_add(1, std::memory_order_relaxed);
        return 1;
    }

    return CallNextHookEx(s_keyboardHook, code, wParam, lParam);
}

static void __fastcall Hook_InputKeyDown(void* inputHandler, int key) {
    if (ShouldCaptureOisKey(key)) {
        s_swallowedKeyDown.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    __try {
        s_origKeyDown(inputHandler, key);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        spdlog::error("input_hooks: InputHandler::keyDownEvent trampoline crashed");
    }
}

static void __fastcall Hook_InputKeyUp(void* inputHandler, int key) {
    if (ShouldCaptureOisKey(key)) {
        s_swallowedKeyUp.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    __try {
        s_origKeyUp(inputHandler, key);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        spdlog::error("input_hooks: InputHandler::keyUpEvent trampoline crashed");
    }
}

bool Install() {
    auto& scanner = Core::Get().GetScanner();
    uintptr_t base = scanner.GetBase();
    if (!base) {
        spdlog::warn("input_hooks: scanner base unavailable, OIS input gate not installed");
        return false;
    }

    auto& hookMgr = HookManager::Get();
    bool keyDownOk = hookMgr.InstallAt("InputKeyDown",
                                       base + RVA_INPUT_KEY_DOWN,
                                       &Hook_InputKeyDown,
                                       &s_origKeyDown);
    bool keyUpOk = hookMgr.InstallAt("InputKeyUp",
                                     base + RVA_INPUT_KEY_UP,
                                     &Hook_InputKeyUp,
                                     &s_origKeyUp);

    s_keyboardHook = SetWindowsHookExA(WH_KEYBOARD_LL, LowLevelKeyboardProc,
                                       GetModuleHandleA("KenshiMP.Core.dll"), 0);
    if (!s_keyboardHook) {
        spdlog::warn("input_hooks: WH_KEYBOARD_LL install failed (err={})", GetLastError());
    }

    s_installed = true;
    spdlog::info("input_hooks: Installed (WndProc + low-level keyboard + OIS gate, keyDown={}, keyUp={}, ll={})",
                 keyDownOk, keyUpOk, s_keyboardHook != nullptr);
    return (keyDownOk || s_keyboardHook != nullptr);
}

void Uninstall() {
    if (s_installed) {
        if (s_keyboardHook) {
            UnhookWindowsHookEx(s_keyboardHook);
            s_keyboardHook = nullptr;
        }
        HookManager::Get().Remove("InputKeyDown");
        HookManager::Get().Remove("InputKeyUp");
        spdlog::info("input_hooks: Uninstalled (swallowedDown={}, swallowedUp={}, lowLevel={})",
                     s_swallowedKeyDown.load(std::memory_order_relaxed),
                     s_swallowedKeyUp.load(std::memory_order_relaxed),
                     s_lowLevelSwallowed.load(std::memory_order_relaxed));
    }
    s_installed = false;
}

} // namespace kmp::input_hooks
