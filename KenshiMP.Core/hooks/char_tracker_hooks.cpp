#include "char_tracker_hooks.h"
#include "../core.h"
#include "../game/game_types.h"
#include "../game/spawn_manager.h"
#include "kmp/hook_manager.h"
#include "kmp/memory.h"
#include <spdlog/spdlog.h>
#include <Windows.h>
#include <Psapi.h>
#include <cstdio>
#include <unordered_map>
#include <mutex>

#pragma comment(lib, "Psapi.lib")

namespace kmp::char_tracker_hooks {

static std::mutex s_trackerMutex;
static std::unordered_map<void*, TrackedChar> s_trackedChars;
static std::function<void(const TrackedChar&)> s_onNewChar;
static void* s_localPlayerAnimClass = nullptr;

// ── Deferred discovery ring buffer ──
// The inline hook fires 300+ times/sec. We MUST avoid heap allocation, mutex,
// spdlog, and CharacterAccessor in the hook body. Instead, record raw pointers
// into a lock-free ring buffer. ProcessDeferredDiscovery() (called from
// OnGameTick) does the expensive name lookup and map insertion.
struct PendingCharUpdate {
    void* animClassPtr;
    uintptr_t charPtr;
};
static constexpr int PENDING_RING_SIZE = 128;
static PendingCharUpdate s_pendingRing[PENDING_RING_SIZE];
static std::atomic<int> s_pendingWrite{0};
static std::atomic<int> s_pendingRead{0};
static std::atomic<uint64_t> s_hookCalls{0};
static std::atomic<uint64_t> s_invalidAnimPtr{0};
static std::atomic<uint64_t> s_charReadFailed{0};
static std::atomic<uint64_t> s_nullCharPtr{0};
static std::atomic<uint64_t> s_invalidCharPtr{0};
static std::atomic<uint64_t> s_alreadyTracked{0};
static std::atomic<uint64_t> s_enqueued{0};
static std::atomic<uint64_t> s_ringFull{0};
static std::atomic<uint64_t> s_emptyName{0};
static std::atomic<uint64_t> s_discovered{0};
static uint64_t s_lastStatsLogTick = 0;
static std::atomic<int> s_charPtrOffset{-1};

static bool IsUserPointer(uintptr_t ptr) {
    return ptr >= 0x10000 && ptr <= 0x00007FFFFFFFFFFF;
}

static bool GetHostModuleRange(uintptr_t& moduleBase, uintptr_t& moduleEnd) {
    static uintptr_t s_moduleBase = 0;
    static uintptr_t s_moduleEnd = 0;
    if (s_moduleBase == 0) {
        HMODULE h = GetModuleHandleA(nullptr);
        if (h) {
            MODULEINFO mi{};
            if (GetModuleInformation(GetCurrentProcess(), h, &mi, sizeof(mi))) {
                s_moduleBase = reinterpret_cast<uintptr_t>(mi.lpBaseOfDll);
                s_moduleEnd = s_moduleBase + mi.SizeOfImage;
            }
        }
    }
    moduleBase = s_moduleBase;
    moduleEnd = s_moduleEnd;
    return moduleBase != 0 && moduleEnd > moduleBase;
}

static int DiscoverCharacterBackpointerOffset(uintptr_t animPtr) {
    uintptr_t moduleBase = 0;
    uintptr_t moduleEnd = 0;
    GetHostModuleRange(moduleBase, moduleEnd);

    char dbg[2048];
    int pos = sprintf_s(dbg, sizeof(dbg),
        "char_tracker: PROBING anim=0x%llX module=0x%llX..0x%llX\n",
        static_cast<unsigned long long>(animPtr),
        static_cast<unsigned long long>(moduleBase),
        static_cast<unsigned long long>(moduleEnd));

    int chosen = -1;
    constexpr uintptr_t ANIM_BLOCK_HALF = 0x1000;
    for (int off = 8; off <= 0x600; off += 8) {
        uintptr_t candidate = 0;
        if (!Memory::Read(animPtr + off, candidate)) continue;
        if (candidate == animPtr) continue;
        if (!IsUserPointer(candidate) || (candidate & 0x7) != 0) continue;
        if (moduleBase != 0 && candidate >= moduleBase && candidate < moduleEnd) continue;

        uintptr_t delta = (candidate > animPtr) ? (candidate - animPtr) : (animPtr - candidate);
        if (delta < ANIM_BLOCK_HALF) continue;

        uintptr_t vtable = 0;
        if (!Memory::Read(candidate, vtable)) continue;
        bool vtableInModule = (moduleBase != 0 && vtable >= moduleBase && vtable < moduleEnd);
        if (!vtableInModule) continue;

        uint64_t nameSize = 0;
        if (!Memory::Read(candidate + game::GetOffsets().character.name + 0x10, nameSize)) continue;
        if (nameSize == 0 || nameSize > 256) {
            pos += sprintf_s(dbg + pos, sizeof(dbg) - pos,
                             "  candidate +0x%03X = 0x%llX (vtable in-module, nameSize=%llu skip)\n",
                             off, static_cast<unsigned long long>(candidate),
                             static_cast<unsigned long long>(nameSize));
            if (pos > static_cast<int>(sizeof(dbg)) - 160) break;
            continue;
        }

        pos += sprintf_s(dbg + pos, sizeof(dbg) - pos,
                         "  candidate +0x%03X = 0x%llX (vtable=0x%llX, nameSize=%llu) ACCEPTED\n",
                         off, static_cast<unsigned long long>(candidate),
                         static_cast<unsigned long long>(vtable),
                         static_cast<unsigned long long>(nameSize));
        chosen = off;
        break;
    }

    if (chosen >= 0) {
        pos += sprintf_s(dbg + pos, sizeof(dbg) - pos,
                         "  CHOSEN offset: +0x%03X (auto-discovered)\n", chosen);
    } else {
        chosen = 0x2D8;
        pos += sprintf_s(dbg + pos, sizeof(dbg) - pos,
                         "  NO valid candidate found - falling back to +0x2D8\n");
    }

    OutputDebugStringA(dbg);
    spdlog::info("{}", dbg);
    spdlog::default_logger()->flush();
    return chosen;
}

static void OnCharUpdate(void* animClassHuman) {
    s_hookCalls.fetch_add(1, std::memory_order_relaxed);
    if (!animClassHuman) {
        s_invalidAnimPtr.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    uintptr_t animPtr = reinterpret_cast<uintptr_t>(animClassHuman);
    if (!IsUserPointer(animPtr)) {
        s_invalidAnimPtr.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    int charPtrOffset = s_charPtrOffset.load(std::memory_order_acquire);
    if (charPtrOffset < 0) {
        charPtrOffset = DiscoverCharacterBackpointerOffset(animPtr);
        s_charPtrOffset.store(charPtrOffset, std::memory_order_release);
    }

    uintptr_t charPtr = 0;
    if (!Memory::Read(animPtr + charPtrOffset, charPtr)) {
        s_charReadFailed.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (charPtr == 0) {
        s_nullCharPtr.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (!IsUserPointer(charPtr)) {
        s_invalidCharPtr.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    void* charKey = reinterpret_cast<void*>(charPtr);

    // Fast path: already tracked — just update timestamp (atomic, no mutex)
    // Use a simple spinlock-free check: try_lock fails = skip this update (no stall)
    if (s_trackerMutex.try_lock()) {
        auto it = s_trackedChars.find(charKey);
        if (it != s_trackedChars.end()) {
            it->second.animClassPtr = animClassHuman;
            it->second.lastSeenTick = GetTickCount64();
            s_trackerMutex.unlock();
            s_alreadyTracked.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        s_trackerMutex.unlock();
    }

    // Slow path: new character — push to ring buffer for deferred processing.
    // NO heap allocation, NO mutex hold, NO spdlog in this path.
    int writeIdx = s_pendingWrite.load(std::memory_order_relaxed);
    int nextIdx = (writeIdx + 1) % PENDING_RING_SIZE;
    if (nextIdx != s_pendingRead.load(std::memory_order_acquire)) {
        s_pendingRing[writeIdx] = {animClassHuman, charPtr};
        s_pendingWrite.store(nextIdx, std::memory_order_release);
        s_enqueued.fetch_add(1, std::memory_order_relaxed);
    } else {
        s_ringFull.fetch_add(1, std::memory_order_relaxed);
    }
    // If ring is full, drop this update (will be caught on next animation tick)
}

// ── Inline Hook Implementation ──
static uint8_t s_originalBytes[14] = {};
static void* s_trampolineAlloc = nullptr;
static uintptr_t s_installedHookAddr = 0;

enum class HookSourceRegister {
    Rbx,
    Rsi,
};

static void SEH_OnCharUpdate(void* animClassHuman) {
    __try {
        OnCharUpdate(animClassHuman);
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
}

static bool BuildInlineHook(uintptr_t hookAddr, HookSourceRegister sourceRegister) {
    __try {
        memcpy(s_originalBytes, reinterpret_cast<void*>(hookAddr), 14);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        spdlog::error("char_tracker: Failed to read original bytes at 0x{:X}", hookAddr);
        return false;
    }

    size_t trampolineSize = 256;
    s_trampolineAlloc = VirtualAlloc(nullptr, trampolineSize,
                                      MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!s_trampolineAlloc) {
        spdlog::error("char_tracker: VirtualAlloc failed for trampoline");
        return false;
    }

    uint8_t* code = static_cast<uint8_t*>(s_trampolineAlloc);
    int off = 0;

    // Save all registers
    uint8_t saveRegs[] = {
        0x50, 0x51, 0x52, 0x53, 0x55, 0x56, 0x57,
        0x41,0x50, 0x41,0x51, 0x41,0x52, 0x41,0x53,
        0x41,0x54, 0x41,0x55, 0x41,0x56, 0x41,0x57
    };
    memcpy(code + off, saveRegs, sizeof(saveRegs));
    off += sizeof(saveRegs);

    // sub rsp, 0x28 (shadow space)
    code[off++] = 0x48; code[off++] = 0x83; code[off++] = 0xEC; code[off++] = 0x28;

    if (sourceRegister == HookSourceRegister::Rsi) {
        // mov rcx, rsi (Steam internal hook site keeps AnimationClassHuman* in RSI)
        code[off++] = 0x48; code[off++] = 0x89; code[off++] = 0xF1;
    } else {
        // mov rcx, rbx (legacy hook site keeps AnimationClassHuman* in RBX)
        code[off++] = 0x48; code[off++] = 0x89; code[off++] = 0xD9;
    }

    // call [rip+2]; jmp over ptr
    code[off++] = 0xFF; code[off++] = 0x15; code[off++] = 0x02;
    code[off++] = 0x00; code[off++] = 0x00; code[off++] = 0x00;
    code[off++] = 0xEB; code[off++] = 0x08;
    uintptr_t funcAddr = reinterpret_cast<uintptr_t>(&SEH_OnCharUpdate);
    memcpy(code + off, &funcAddr, 8);
    off += 8;

    // add rsp, 0x28
    code[off++] = 0x48; code[off++] = 0x83; code[off++] = 0xC4; code[off++] = 0x28;

    // Restore all registers
    uint8_t restoreRegs[] = {
        0x41,0x5F, 0x41,0x5E, 0x41,0x5D, 0x41,0x5C,
        0x41,0x5B, 0x41,0x5A, 0x41,0x59, 0x41,0x58,
        0x5F, 0x5E, 0x5D, 0x5B, 0x5A, 0x59, 0x58
    };
    memcpy(code + off, restoreRegs, sizeof(restoreRegs));
    off += sizeof(restoreRegs);

    // Execute original 14 bytes
    memcpy(code + off, s_originalBytes, 14);
    off += 14;

    // jmp back to hookAddr + 14
    code[off++] = 0xFF; code[off++] = 0x25;
    code[off++] = 0x00; code[off++] = 0x00; code[off++] = 0x00; code[off++] = 0x00;
    uintptr_t returnAddr = hookAddr + 14;
    memcpy(code + off, &returnAddr, 8);
    off += 8;

    // Patch original code to jump to trampoline
    DWORD oldProtect;
    if (!VirtualProtect(reinterpret_cast<void*>(hookAddr), 14, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        spdlog::error("char_tracker: VirtualProtect failed for hook site");
        VirtualFree(s_trampolineAlloc, 0, MEM_RELEASE);
        s_trampolineAlloc = nullptr;
        return false;
    }

    uint8_t* hookSite = reinterpret_cast<uint8_t*>(hookAddr);
    hookSite[0] = 0xFF; hookSite[1] = 0x25;
    hookSite[2] = 0x00; hookSite[3] = 0x00; hookSite[4] = 0x00; hookSite[5] = 0x00;
    uintptr_t trampolineAddr = reinterpret_cast<uintptr_t>(s_trampolineAlloc);
    memcpy(hookSite + 6, &trampolineAddr, 8);

    VirtualProtect(reinterpret_cast<void*>(hookAddr), 14, oldProtect, &oldProtect);

    spdlog::info("char_tracker: Inline hook installed at 0x{:X} -> trampoline 0x{:X}",
                 hookAddr, trampolineAddr);
    s_installedHookAddr = hookAddr;
    return true;
}

const TrackedChar* FindByName(const std::string& name) {
    std::lock_guard lock(s_trackerMutex);
    for (auto& [key, tc] : s_trackedChars) {
        if (tc.name == name) return &tc;
    }
    return nullptr;
}

const TrackedChar* FindByPtr(void* characterPtr) {
    std::lock_guard lock(s_trackerMutex);
    auto it = s_trackedChars.find(characterPtr);
    return (it != s_trackedChars.end()) ? &it->second : nullptr;
}

void* GetLocalPlayerAnimClass() { return s_localPlayerAnimClass; }

void* GetRemotePlayerAnimClass(const std::string& name) {
    auto* tc = FindByName(name);
    return tc ? tc->animClassPtr : nullptr;
}

void SetOnNewCharacter(std::function<void(const TrackedChar&)> callback) {
    std::lock_guard lock(s_trackerMutex);
    s_onNewChar = callback;
}

int GetTrackedCount() {
    std::lock_guard lock(s_trackerMutex);
    return static_cast<int>(s_trackedChars.size());
}

std::vector<TrackedChar> GetTrackedSnapshot() {
    std::lock_guard lock(s_trackerMutex);
    std::vector<TrackedChar> out;
    out.reserve(s_trackedChars.size());
    for (auto& [key, tc] : s_trackedChars) {
        out.push_back(tc);
    }
    return out;
}

void DumpTrackedChars() {
    std::lock_guard lock(s_trackerMutex);
    uint64_t now = GetTickCount64();
    spdlog::info("char_tracker: {} tracked characters:", s_trackedChars.size());
    for (auto& [key, tc] : s_trackedChars) {
        float ageSec = (now - tc.lastSeenTick) / 1000.f;
        spdlog::info("  '{}' at 0x{:X} (animClass=0x{:X}), pos=({:.0f},{:.0f},{:.0f}), age={:.1f}s",
                     tc.name, reinterpret_cast<uintptr_t>(tc.characterPtr),
                     reinterpret_cast<uintptr_t>(tc.animClassPtr),
                     tc.position.x, tc.position.y, tc.position.z, ageSec);
    }
}

void ProcessDeferredDiscovery() {
    uint64_t now = GetTickCount64();
    if (now - s_lastStatsLogTick >= 5000) {
        s_lastStatsLogTick = now;
        spdlog::info("char_tracker: stats calls={} enqueued={} discovered={} tracked={} invalidAnim={} readFail={} nullChar={} invalidChar={} alreadyTracked={} ringFull={} emptyName={}",
                     s_hookCalls.load(std::memory_order_relaxed),
                     s_enqueued.load(std::memory_order_relaxed),
                     s_discovered.load(std::memory_order_relaxed),
                     GetTrackedCount(),
                     s_invalidAnimPtr.load(std::memory_order_relaxed),
                     s_charReadFailed.load(std::memory_order_relaxed),
                     s_nullCharPtr.load(std::memory_order_relaxed),
                     s_invalidCharPtr.load(std::memory_order_relaxed),
                     s_alreadyTracked.load(std::memory_order_relaxed),
                     s_ringFull.load(std::memory_order_relaxed),
                     s_emptyName.load(std::memory_order_relaxed));
    }

    int processed = 0;
    while (processed < 8) { // Cap per tick to avoid stalls
        int readIdx = s_pendingRead.load(std::memory_order_relaxed);
        if (readIdx == s_pendingWrite.load(std::memory_order_acquire)) break; // Empty
        PendingCharUpdate pending = s_pendingRing[readIdx];
        s_pendingRead.store((readIdx + 1) % PENDING_RING_SIZE, std::memory_order_release);
        processed++;

        void* charKey = reinterpret_cast<void*>(pending.charPtr);

        // Check if already tracked (could have been added between push and now)
        {
            std::lock_guard lock(s_trackerMutex);
            if (s_trackedChars.count(charKey) > 0) {
                s_trackedChars[charKey].animClassPtr = pending.animClassPtr;
                s_trackedChars[charKey].lastSeenTick = GetTickCount64();
                continue;
            }
        }

        // Now do the expensive work: read name, build TrackedChar, insert
        game::CharacterAccessor accessor(charKey);
        std::string name = accessor.GetName();
        if (name.empty()) {
            s_emptyName.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        TrackedChar tc;
        tc.animClassPtr = pending.animClassPtr;
        tc.characterPtr = charKey;
        tc.name = name;
        tc.position = accessor.GetPosition();
        tc.lastSeenTick = GetTickCount64();

        {
            std::lock_guard lock(s_trackerMutex);
            s_trackedChars[charKey] = tc;
        }

        spdlog::info("char_tracker: NEW character '{}' at 0x{:X} (animClass=0x{:X})",
                     name, pending.charPtr, reinterpret_cast<uintptr_t>(pending.animClassPtr));
        s_discovered.fetch_add(1, std::memory_order_relaxed);

        if (s_onNewChar) {
            s_onNewChar(tc);
        }
    }
}

bool Install() {
    auto& funcs = Core::Get().GetGameFunctions();
    HookSourceRegister sourceRegister = HookSourceRegister::Rbx;

    if (!funcs.CharAnimUpdate) {
        constexpr uintptr_t STEAM_CHAR_ANIM_UPDATE_RVA = 0x65F244;
        uintptr_t moduleBase = Memory::GetModuleBase();
        if (!moduleBase) {
            spdlog::warn("char_tracker: CharAnimUpdate unresolved and module base unavailable - hook not installed");
            return false;
        }

        uintptr_t fallbackHookAddr = moduleBase + STEAM_CHAR_ANIM_UPDATE_RVA;
        sourceRegister = HookSourceRegister::Rsi;
        spdlog::warn("char_tracker: CharAnimUpdate unresolved; using Steam fallback at 0x{:X} (RVA 0x{:X}, source=RSI)",
                     fallbackHookAddr, STEAM_CHAR_ANIM_UPDATE_RVA);
        spdlog::info("char_tracker: Installing inline hook at 0x{:X}", fallbackHookAddr);

        return BuildInlineHook(fallbackHookAddr, sourceRegister);
    }

    uintptr_t hookAddr = reinterpret_cast<uintptr_t>(funcs.CharAnimUpdate);
    spdlog::info("char_tracker: Installing inline hook at 0x{:X}", hookAddr);

    return BuildInlineHook(hookAddr, sourceRegister);
}

void Uninstall() {
    if (s_trampolineAlloc) {
        if (s_installedHookAddr) {
            uintptr_t hookAddr = s_installedHookAddr;
            DWORD oldProtect;
            if (VirtualProtect(reinterpret_cast<void*>(hookAddr), 14, PAGE_EXECUTE_READWRITE, &oldProtect)) {
                memcpy(reinterpret_cast<void*>(hookAddr), s_originalBytes, 14);
                VirtualProtect(reinterpret_cast<void*>(hookAddr), 14, oldProtect, &oldProtect);
            }
        }
        VirtualFree(s_trampolineAlloc, 0, MEM_RELEASE);
        s_trampolineAlloc = nullptr;
        s_installedHookAddr = 0;
    }
}

} // namespace kmp::char_tracker_hooks
