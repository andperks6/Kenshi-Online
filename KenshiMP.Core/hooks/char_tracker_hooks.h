#pragma once
#include "kmp/types.h"
#include <string>
#include <functional>
#include <vector>

namespace kmp::char_tracker_hooks {

bool Install();
void Uninstall();

struct TrackedChar {
    void* animClassPtr;     // AnimationClassHuman*
    void* characterPtr;     // CharacterHuman* (auto-discovered from AnimationClassHuman)
    std::string name;
    Vec3 position;
    uint64_t lastSeenTick;
};

const TrackedChar* FindByName(const std::string& name);
const TrackedChar* FindByPtr(void* characterPtr);
void* GetLocalPlayerAnimClass();
void* GetRemotePlayerAnimClass(const std::string& name);
void SetOnNewCharacter(std::function<void(const TrackedChar&)> callback);
int GetTrackedCount();
std::vector<TrackedChar> GetTrackedSnapshot();
void DumpTrackedChars();

// Process deferred character discoveries from safe game-tick context.
// Called from Core::OnGameTick — NOT from inside the inline hook.
void ProcessDeferredDiscovery();

} // namespace kmp::char_tracker_hooks
