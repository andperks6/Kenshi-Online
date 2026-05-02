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

static constexpr uintptr_t RVA_INPUT_KEY_DOWN = 0x00360680;
static constexpr uintptr_t RVA_INPUT_KEY_UP = 0x003608F0;
static constexpr int OIS_KC_F1 = 0x3B;

static std::atomic<uint32_t> s_swallowedKeyDown{0};
static std::atomic<uint32_t> s_swallowedKeyUp{0};

static bool ShouldCaptureOisKey(int key) {
    if (key == OIS_KC_F1) return true;

    auto& core = Core::Get();
    if (core.GetNativeHud().IsChatInputActive()) return true;
    if (core.GetOverlay().GetNativeMenu().IsVisible()) return true;

    return false;
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

    s_installed = true;
    spdlog::info("input_hooks: Installed (WndProc + OIS gate, keyDown={}, keyUp={})",
                 keyDownOk, keyUpOk);
    return keyDownOk && keyUpOk;
}

void Uninstall() {
    if (s_installed) {
        HookManager::Get().Remove("InputKeyDown");
        HookManager::Get().Remove("InputKeyUp");
        spdlog::info("input_hooks: Uninstalled (swallowedDown={}, swallowedUp={})",
                     s_swallowedKeyDown.load(std::memory_order_relaxed),
                     s_swallowedKeyUp.load(std::memory_order_relaxed));
    }
    s_installed = false;
}

} // namespace kmp::input_hooks
