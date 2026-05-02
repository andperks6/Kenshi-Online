#include "ai_hooks.h"
#include "kmp/hook_manager.h"
#include "kmp/patterns.h"
#include "../core.h"
#include "../game/game_types.h"
#include <spdlog/spdlog.h>
#include <unordered_set>
#include <mutex>

namespace kmp::ai_hooks {

// ── Function typedefs ──
// AI::create is a constructor-style initializer:
//   RCX=this, RDX=character, R8/R9=state pointers, stack arg 5 stored at +0x318,
//   stack arg 6 stored at +0x10. Dropping args 3-6 leaves +0x318 null and crashes
//   later in AI scoring at game+0x59820D.
using AICreateFn   = void(__fastcall*)(void* ai, void* character, void* arg3,
                                       void* arg4, void* arg5, void* arg6);
using AIPackagesFn = void(__fastcall*)(void* character, void* aiPackage);

// ── State ──
static AICreateFn   s_origAICreate   = nullptr;
static AIPackagesFn s_origAIPackages = nullptr;
static int s_createCount = 0;
static int s_packageCount = 0;

// ── Remote-controlled character tracking ──
// Characters in this set have their AI decisions overridden by network input.
// The AI CONTROLLER is kept valid (no nullptr!) so the engine doesn't crash
// when downstream code dereferences it. Only the AI DECISIONS are suppressed.
static std::unordered_set<void*> s_remoteControlled;
static std::mutex s_remoteMutex;

void MarkRemoteControlled(void* character) {
    std::lock_guard lock(s_remoteMutex);
    s_remoteControlled.insert(character);
    spdlog::info("ai_hooks: Marked character 0x{:X} as remote-controlled ({} total)",
                 (uintptr_t)character, s_remoteControlled.size());
}

void UnmarkRemoteControlled(void* character) {
    std::lock_guard lock(s_remoteMutex);
    s_remoteControlled.erase(character);
}

bool IsRemoteControlled(void* character) {
    std::lock_guard lock(s_remoteMutex);
    return s_remoteControlled.count(character) > 0;
}

// ── Hooks ──

static void __fastcall Hook_AICreate(void* ai, void* character, void* arg3,
                                     void* arg4, void* arg5, void* arg6) {
    s_createCount++;

    // ALWAYS call the original AICreate — every character needs a valid AI controller.
    // Returning nullptr here was the root cause of crashes when interacting with
    // remote characters: downstream code dereferences the AI controller without
    // null checks (combat, pathfinding, animation state transitions, UI selection).
    __try {
        s_origAICreate(ai, character, arg3, arg4, arg5, arg6);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        spdlog::error("ai_hooks: AICreate crashed");
        return;
    }

    auto& core = Core::Get();
    if (!core.IsConnected()) return;

    // Check if this is a remote player's character — if so, mark it for
    // AI decision override (movement/tasks blocked, driven by network instead).
    // The AI controller itself stays VALID — only decisions are suppressed.
    auto& registry = core.GetEntityRegistry();
    EntityID netId = registry.GetNetId(character);
    if (netId != INVALID_ENTITY) {
        auto info = registry.GetInfo(netId);
        if (info.has_value() && info->isRemote) {
            MarkRemoteControlled(character);
            spdlog::info("ai_hooks: AICreate for remote entity {} — AI controller CREATED "
                         "(decisions will be overridden by network), ai=0x{:X}, char=0x{:X}",
                         netId, (uintptr_t)ai, (uintptr_t)character);
        }
    }

    if (s_createCount % 100 == 1) {
        spdlog::debug("ai_hooks: AICreate #{} (ai=0x{:X}, char=0x{:X}, arg3=0x{:X}, "
                       "arg4=0x{:X}, arg5=0x{:X}, arg6=0x{:X})",
                       s_createCount, (uintptr_t)ai, (uintptr_t)character,
                       (uintptr_t)arg3, (uintptr_t)arg4, (uintptr_t)arg5,
                       (uintptr_t)arg6);
    }
}

static void __fastcall Hook_AIPackages(void* character, void* aiPackage) {
    s_packageCount++;

    // ALWAYS let AI packages load — the character needs valid behavior trees
    // to prevent crashes when the engine queries them. Even for remote characters,
    // the behavior tree structure must exist; we just override the actual decisions.
    __try {
        s_origAIPackages(character, aiPackage);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        spdlog::error("ai_hooks: AIPackages crashed");
        return;
    }

    auto& core = Core::Get();
    if (!core.IsConnected()) return;

    // Log for remote characters (diagnostic only — no suppression)
    auto& registry = core.GetEntityRegistry();
    EntityID netId = registry.GetNetId(character);
    if (netId != INVALID_ENTITY) {
        auto info = registry.GetInfo(netId);
        if (info.has_value() && info->isRemote) {
            spdlog::debug("ai_hooks: AI packages LOADED for remote entity {} "
                           "(behavior tree valid, decisions overridden)",
                           netId);
        }
    }

    if (s_packageCount % 200 == 1) {
        spdlog::debug("ai_hooks: AIPackages #{} (char=0x{:X}, pkg=0x{:X})",
                       s_packageCount, (uintptr_t)character, (uintptr_t)aiPackage);
    }
}

// ── Install / Uninstall ──

bool Install() {
    auto& funcs = Core::Get().GetGameFunctions();
    auto& hooks = HookManager::Get();
    int installed = 0;

    if (funcs.AICreate) {
        if (hooks.InstallAt("AICreate", reinterpret_cast<uintptr_t>(funcs.AICreate),
                            &Hook_AICreate, &s_origAICreate)) {
            installed++;
            spdlog::info("ai_hooks: AICreate hook installed");
        }
    }

    if (funcs.AIPackages) {
        if (hooks.InstallAt("AIPackages", reinterpret_cast<uintptr_t>(funcs.AIPackages),
                            &Hook_AIPackages, &s_origAIPackages)) {
            installed++;
            spdlog::info("ai_hooks: AIPackages hook installed");
        }
    }

    spdlog::info("ai_hooks: {}/2 hooks installed", installed);
    return installed > 0;
}

void Uninstall() {
    auto& hooks = HookManager::Get();
    if (s_origAICreate)   hooks.Remove("AICreate");
    if (s_origAIPackages) hooks.Remove("AIPackages");
    s_origAICreate = nullptr;
    s_origAIPackages = nullptr;

    // Clear remote tracking
    std::lock_guard lock(s_remoteMutex);
    s_remoteControlled.clear();
}

} // namespace kmp::ai_hooks
