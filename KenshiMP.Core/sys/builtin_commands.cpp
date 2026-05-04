#include "command_registry.h"
#include "../core.h"
#include "../game/game_types.h"
#include "../game/spawn_manager.h"
#include "../game/player_controller.h"
#include "../game/asset_facilitator.h"
#include "../game/loading_orchestrator.h"
#include "../hooks/time_hooks.h"
#include "../hooks/entity_hooks.h"
#include "../hooks/ai_hooks.h"
#include "../hooks/char_tracker_hooks.h"
#include "../hooks/order_hooks.h"
#include "../game/shared_save_sync.h"
#include "kmp/protocol.h"
#include "kmp/messages.h"
#include "kmp/constants.h"
#include "kmp/memory.h"
#include "kmp/hook_manager.h"
#include <spdlog/spdlog.h>
#include <cstdio>
#include <cmath>
#include <algorithm>
#include <cctype>
#include <mutex>
#include <vector>

namespace kmp {

namespace {

struct AnimTraceState {
    std::mutex mutex;
    bool active = false;
    std::string filter;
    float elapsed = 0.0f;
    float duration = 0.0f;
    float sampleAccum = 0.0f;
    float interval = 0.25f;
    int sampleIndex = 0;
    bool hasPrevPos = false;
    Vec3 prevPos{};
};

AnimTraceState g_animTrace;

std::string LowerCopy(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool IsLikelyPtr(uintptr_t v) {
    return v >= 0x10000 && v < 0x00007FFFFFFFFFFFULL && (v & 0x7) == 0;
}

const char_tracker_hooks::TrackedChar* FindTrackedByFilter(
    const std::vector<char_tracker_hooks::TrackedChar>& tracked,
    const std::string& filter) {
    std::string filterLower = LowerCopy(filter);
    for (const auto& tc : tracked) {
        if (LowerCopy(tc.name).find(filterLower) != std::string::npos) {
            return &tc;
        }
    }
    return nullptr;
}

bool ReadU32(uintptr_t base, size_t off, uint32_t& out) {
    return Memory::Read(base + off, out);
}

bool ReadF32(uintptr_t base, size_t off, float& out) {
    return Memory::Read(base + off, out) && std::isfinite(out);
}

void LogAnimTraceSampleLocked(AnimTraceState& st, float deltaTime) {
    auto tracked = char_tracker_hooks::GetTrackedSnapshot();
    const auto* match = FindTrackedByFilter(tracked, st.filter);
    if (!match) {
        spdlog::warn("anim_trace sample={} no tracked character matched '{}'", st.sampleIndex, st.filter);
        return;
    }

    uintptr_t anim = reinterpret_cast<uintptr_t>(match->animClassPtr);
    uintptr_t chr = reinterpret_cast<uintptr_t>(match->characterPtr);
    if (!IsLikelyPtr(anim) || !IsLikelyPtr(chr)) {
        spdlog::warn("anim_trace sample={} '{}' invalid pointers char=0x{:X} anim=0x{:X}",
                     st.sampleIndex, match->name, chr, anim);
        return;
    }

    game::CharacterAccessor accessor(reinterpret_cast<void*>(chr));
    Vec3 pos = accessor.GetPosition();
    float speed = 0.0f;
    if (st.hasPrevPos && deltaTime > 0.001f) {
        float dx = pos.x - st.prevPos.x;
        float dy = pos.y - st.prevPos.y;
        float dz = pos.z - st.prevPos.z;
        speed = std::sqrt(dx * dx + dy * dy + dz * dz) / deltaTime;
    }
    st.prevPos = pos;
    st.hasPrevPos = true;

    uint32_t v0c8 = 0;
    uint32_t v334 = 0;
    uint32_t v340 = 0;
    uint32_t v344 = 0;
    uint32_t v348 = 0;
    uint32_t v34c = 0;
    uint32_t v350 = 0;
    uint32_t v530 = 0;
    uint32_t v570 = 0;
    uintptr_t controller = 0;
    uintptr_t result = 0;
    uint32_t ctrlActive = 0;
    uint32_t resFlags = 0;
    uint32_t resMode38 = 0;
    uint32_t resMode40 = 0;
    uint32_t resKind50 = 0;
    float resF0 = 0.0f;
    float resF8 = 0.0f;
    float resFC = 0.0f;
    float resF10 = 0.0f;
    float resF14 = 0.0f;
    float resF20 = 0.0f;
    ReadU32(anim, 0x0C8, v0c8);
    ReadU32(anim, 0x334, v334);
    ReadU32(anim, 0x340, v340);
    ReadU32(anim, 0x344, v344);
    ReadU32(anim, 0x348, v348);
    ReadU32(anim, 0x34C, v34c);
    ReadU32(anim, 0x350, v350);
    ReadU32(anim, 0x530, v530);
    ReadU32(anim, 0x570, v570);

    if (Memory::Read(anim + 0x328, controller) && IsLikelyPtr(controller)) {
        uint8_t activeByte = 0;
        if (Memory::Read(controller + 0x18, activeByte)) {
            ctrlActive = activeByte;
        }
        if (Memory::Read(controller + 0x8, result) && IsLikelyPtr(result)) {
            uint8_t b4 = 0, b5 = 0, b6 = 0, b7 = 0;
            Memory::Read(result + 0x4, b4);
            Memory::Read(result + 0x5, b5);
            Memory::Read(result + 0x6, b6);
            Memory::Read(result + 0x7, b7);
            resFlags = static_cast<uint32_t>(b4) |
                       (static_cast<uint32_t>(b5) << 8) |
                       (static_cast<uint32_t>(b6) << 16) |
                       (static_cast<uint32_t>(b7) << 24);
            ReadU32(result, 0x38, resMode38);
            ReadU32(result, 0x40, resMode40);
            ReadU32(result, 0x50, resKind50);
            ReadF32(result, 0x0, resF0);
            ReadF32(result, 0x8, resF8);
            ReadF32(result, 0x0C, resFC);
            ReadF32(result, 0x10, resF10);
            ReadF32(result, 0x14, resF14);
            ReadF32(result, 0x20, resF20);
        }
    }

    spdlog::info(
        "anim_trace sample={} t={:.2f}/{:.2f} name='{}' char=0x{:X} anim=0x{:X} "
        "pos=({:.2f},{:.2f},{:.2f}) speed={:.2f} "
        "+0C8={} +334={} +340={} +344={} +348={} +34C={} +350={} +530={} +570={} "
        "ctrl=0x{:X} ctrlActive={} result=0x{:X} resFlags=0x{:08X} resKind50={} "
        "resMode38={} resMode40={} resF0={:.3f} resF8={:.3f} resFC={:.3f} resF10={:.3f} resF14={:.3f} resF20={:.3f}",
        st.sampleIndex, st.elapsed, st.duration, match->name, chr, anim,
        pos.x, pos.y, pos.z, speed,
        v0c8, v334, v340, v344, v348, v34c, v350, v530, v570,
        controller, ctrlActive, result, resFlags, resKind50,
        resMode38, resMode40, resF0, resF8, resFC, resF10, resF14, resF20);
}

} // namespace

void ProcessCommandDiagnosticsTick(float deltaTime) {
    std::lock_guard lock(g_animTrace.mutex);
    if (!g_animTrace.active) return;

    g_animTrace.elapsed += deltaTime;
    g_animTrace.sampleAccum += deltaTime;
    if (g_animTrace.sampleAccum < g_animTrace.interval) {
        return;
    }

    float sampleDelta = g_animTrace.sampleAccum;
    g_animTrace.sampleAccum = 0.0f;
    LogAnimTraceSampleLocked(g_animTrace, sampleDelta);
    g_animTrace.sampleIndex++;

    if (g_animTrace.elapsed >= g_animTrace.duration) {
        spdlog::info("anim_trace complete filter='{}' samples={} duration={:.2f}",
                     g_animTrace.filter, g_animTrace.sampleIndex, g_animTrace.elapsed);
        g_animTrace.active = false;
    }
}

void CommandRegistry::RegisterBuiltins() {
    // /help — List all registered commands
    Register("help", "List all available commands", [](const CommandArgs&) -> std::string {
        auto cmds = CommandRegistry::Get().GetAll();
        std::string result = "--- Commands ---";
        for (auto* cmd : cmds) {
            result += "\n/" + cmd->name + " - " + cmd->description;
        }
        return result;
    });

    // /tp [player] — Teleport to nearest (or named) remote player
    Register("tp", "Teleport to player (/tp or /tp name)", [](const CommandArgs& args) -> std::string {
        auto& core = Core::Get();
        if (!core.IsConnected()) return "Not connected to a server.";

        // If a player name is given, find their entity and teleport to it
        if (!args.args.empty()) {
            std::string targetName = args.args[0];
            // Join all args in case name has spaces
            for (size_t i = 1; i < args.args.size(); i++)
                targetName += " " + args.args[i];

            // Search remote players for a name match (case-insensitive partial)
            auto remotePlayers = core.GetPlayerController().GetAllRemotePlayers();
            PlayerID foundId = 0;
            std::string foundName;

            // First pass: exact match (case-insensitive)
            for (auto& rp : remotePlayers) {
                std::string rpLower = rp.playerName;
                std::string tgtLower = targetName;
                std::transform(rpLower.begin(), rpLower.end(), rpLower.begin(), ::tolower);
                std::transform(tgtLower.begin(), tgtLower.end(), tgtLower.begin(), ::tolower);
                if (rpLower == tgtLower) { foundId = rp.playerId; foundName = rp.playerName; break; }
            }
            // Second pass: prefix match
            if (foundId == 0) {
                for (auto& rp : remotePlayers) {
                    std::string rpLower = rp.playerName;
                    std::string tgtLower = targetName;
                    std::transform(rpLower.begin(), rpLower.end(), rpLower.begin(), ::tolower);
                    std::transform(tgtLower.begin(), tgtLower.end(), tgtLower.begin(), ::tolower);
                    if (rpLower.find(tgtLower) == 0) { foundId = rp.playerId; foundName = rp.playerName; break; }
                }
            }

            if (foundId == 0) return "Player '" + targetName + "' not found.";

            // Find an entity owned by this player
            auto remoteEntities = core.GetEntityRegistry().GetRemoteEntities();
            Vec3 targetPos(0, 0, 0);
            bool foundPos = false;
            for (EntityID eid : remoteEntities) {
                auto info = core.GetEntityRegistry().GetInfo(eid);
                if (info.has_value() && info->ownerPlayerId == foundId) {
                    Vec3 pos = info->lastPosition;
                    if (pos.x != 0.f || pos.y != 0.f || pos.z != 0.f) {
                        targetPos = pos;
                        foundPos = true;
                        break;
                    }
                }
            }
            if (!foundPos) return "Player '" + foundName + "' has no visible entities.";

            // Teleport local squad to target
            auto localEntities = core.GetEntityRegistry().GetPlayerEntities(core.GetLocalPlayerId());
            int teleported = 0;
            for (EntityID netId : localEntities) {
                void* gameObj = core.GetEntityRegistry().GetGameObject(netId);
                if (!gameObj) continue;
                game::CharacterAccessor accessor(gameObj);
                if (!accessor.IsValid()) continue;
                Vec3 tpPos = targetPos;
                tpPos.x += static_cast<float>(teleported % 4) * 3.0f;
                tpPos.z += static_cast<float>(teleported / 4) * 3.0f;
                if (accessor.WritePosition(tpPos)) {
                    core.GetEntityRegistry().UpdatePosition(netId, tpPos);
                    teleported++;
                }
            }
            if (teleported > 0) return "Teleported to " + foundName + "!";
            return "Teleport failed — no valid local characters.";
        }

        // No name given — teleport to nearest
        if (core.TeleportToNearestRemotePlayer()) {
            return ""; // TeleportToNearestRemotePlayer already shows messages
        }
        return ""; // Error messages already shown by the method
    });

    // /teleport alias — forward args to /tp
    Register("teleport", "Teleport to player (/teleport or /teleport name)", [](const CommandArgs& args) -> std::string {
        std::string cmd = "/tp";
        for (auto& a : args.args) cmd += " " + a;
        return CommandRegistry::Get().Execute(cmd);
    });

    // /pos — Show current position
    Register("pos", "Show your current position", [](const CommandArgs&) -> std::string {
        auto& core = Core::Get();
        auto localEntities = core.GetEntityRegistry().GetPlayerEntities(core.GetLocalPlayerId());
        if (!localEntities.empty()) {
            void* obj = core.GetEntityRegistry().GetGameObject(localEntities[0]);
            if (obj) {
                game::CharacterAccessor accessor(obj);
                Vec3 pos = accessor.GetPosition();
                char buf[128];
                snprintf(buf, sizeof(buf), "Position: (%.0f, %.0f, %.0f)", pos.x, pos.y, pos.z);
                return buf;
            }
        }
        return "No local character found.";
    });

    // /position alias
    Register("position", "Show your current position", [](const CommandArgs&) -> std::string {
        return CommandRegistry::Get().Execute("/pos");
    });

    // /players — List connected players with IDs
    Register("players", "List connected players", [](const CommandArgs&) -> std::string {
        auto& core = Core::Get();
        auto& pc = core.GetPlayerController();
        auto remotePlayers = pc.GetAllRemotePlayers();

        std::string result = "--- Players ---";
        result += "\nYou: " + pc.GetLocalPlayerName();
        for (auto& rp : remotePlayers) {
            result += "\n  " + rp.playerName + " (ID " + std::to_string(rp.playerId) + ")";
        }
        result += "\nTotal: " + std::to_string(1 + remotePlayers.size());
        return result;
    });

    // /who alias
    Register("who", "List connected players", [](const CommandArgs&) -> std::string {
        return CommandRegistry::Get().Execute("/players");
    });

    // /status — Connection, entity, spawn stats
    Register("status", "Show connection and entity status", [](const CommandArgs&) -> std::string {
        auto& core = Core::Get();
        auto& sm = core.GetSpawnManager();
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "Connected: %s | Entities: %d | Remote: %d | PendingSpawns: %d | Templates: %d",
                 core.IsConnected() ? "yes" : "no",
                 (int)core.GetEntityRegistry().GetEntityCount(),
                 (int)core.GetEntityRegistry().GetRemoteCount(),
                 (int)sm.GetPendingSpawnCount(),
                 (int)sm.GetTemplateCount());
        return buf;
    });

    // /connect [ip] [port] — Connect to a server, or trigger sync if already connected
    Register("connect", "Connect to a server (ip [port]), or trigger sync if already connected", [](const CommandArgs& args) -> std::string {
        auto& core = Core::Get();

        // If already connected and no args: trigger sync (same as /sync)
        if (core.IsConnected() && args.args.empty()) {
            if (!core.IsGameLoaded()) return "Connected but game not loaded. Load a save first.";
            entity_hooks::ResumeForNetwork();
            core.SendExistingEntitiesToServer();
            core.ForceSpawnRemotePlayers();
            auto localCount = core.GetEntityRegistry().GetPlayerEntities(core.GetLocalPlayerId()).size();
            return "Sync triggered! " + std::to_string(localCount) + " local entities sent. Use /status for details.";
        }

        // If already connected with args: disconnect first
        if (core.IsConnected()) {
            core.GetClient().Disconnect();
            core.SetConnected(false);
        }

        if (args.args.empty()) return "Usage: /connect <ip> [port]";

        std::string ip = args.args[0];
        uint16_t port = KMP_DEFAULT_PORT; // 27800
        if (args.args.size() >= 2) {
            try {
                port = static_cast<uint16_t>(std::stoi(args.args[1]));
            } catch (...) {
                return "Invalid port number.";
            }
        }

        // Set player name for handshake
        core.GetOverlay().SetConnectionInfo(ip, port, core.GetConfig().playerName);

        if (core.GetClient().ConnectAsync(ip, port)) {
            core.TransitionTo(ClientPhase::Connecting);
            core.GetOverlay().SetConnecting(true);
            if (core.IsGameLoaded()) {
                return "Connecting to " + ip + ":" + std::to_string(port) + "...";
            } else {
                return "Connecting to " + ip + ":" + std::to_string(port) + " — sync starts when you load a save.";
            }
        }
        return "Connection failed to start.";
    });

    // /sync — Manually trigger entity scan + spawn remote players
    Register("sync", "Rescan local squad and spawn remote players", [](const CommandArgs&) -> std::string {
        auto& core = Core::Get();
        if (!core.IsConnected()) return "Not connected. Use /connect <ip> first.";
        if (!core.IsGameLoaded()) return "Game not loaded yet. Load a save first.";

        // Re-enable entity hooks if they were deferred
        entity_hooks::ResumeForNetwork();

        // Force entity rescan (registers local characters with server)
        core.SendExistingEntitiesToServer();
        auto localCount = core.GetEntityRegistry().GetPlayerEntities(core.GetLocalPlayerId()).size();

        // Force spawn any pending remote characters
        core.ForceSpawnRemotePlayers();
        size_t pending = core.GetSpawnManager().GetPendingSpawnCount();

        std::string result = "Sync triggered! " + std::to_string(localCount) + " local entities sent.";
        if (pending > 0) {
            result += " " + std::to_string(pending) + " remote spawn(s) queued.";
        }

        auto remoteEntities = core.GetEntityRegistry().GetRemoteEntities();
        int spawned = 0;
        for (auto eid : remoteEntities) {
            if (core.GetEntityRegistry().GetGameObject(eid)) spawned++;
        }
        if (spawned > 0) {
            result += " " + std::to_string(spawned) + " remote player(s) visible.";
        }

        return result;
    });

    // /disconnect — Disconnect from server
    Register("disconnect", "Disconnect from server", [](const CommandArgs&) -> std::string {
        auto& core = Core::Get();
        if (!core.IsConnected()) return "Not connected.";

        // Teleport remote entities underground before clearing registry
        // (SetConnected(false) clears the registry but doesn't hide the game objects)
        auto& registry = core.GetEntityRegistry();
        auto remoteEntities = registry.GetRemoteEntities();
        int cleaned = 0;
        for (EntityID eid : remoteEntities) {
            void* gameObj = registry.GetGameObject(eid);
            if (gameObj) {
                game::CharacterAccessor accessor(gameObj);
                Vec3 underground(0.f, -10000.f, 0.f);
                accessor.WritePosition(underground);
                cleaned++;
            }
        }

        core.GetClient().Disconnect();
        core.SetConnected(false);

        std::string msg = "Disconnected from server.";
        if (cleaned > 0)
            msg += " Cleaned up " + std::to_string(cleaned) + " remote entities.";
        return msg;
    });

    // /time [value] — Show or set time of day (0.0=midnight, 0.5=noon)
    Register("time", "Show/set time (/time or /time 0.5)", [](const CommandArgs& args) -> std::string {
        if (!time_hooks::HasTimeManager())
            return "Time manager not captured yet (TimeUpdate hook hasn't fired).";

        // If argument given, try to set time
        if (!args.args.empty()) {
            try {
                float newTime = std::stof(args.args[0]);
                if (newTime < 0.f || newTime >= 1.f) return "Time must be between 0.0 and 1.0 (0=midnight, 0.5=noon).";
                if (time_hooks::WriteTimeOfDay(newTime)) {
                    char buf[64];
                    snprintf(buf, sizeof(buf), "Time set to %.2f", newTime);
                    return buf;
                }
                return "Failed to write time.";
            } catch (...) {
                return "Invalid time value. Use 0.0-1.0 (0=midnight, 0.5=noon).";
            }
        }

        // Show current time (read from captured TimeManager)
        float tod = time_hooks::GetTimeOfDay();
        float speed = time_hooks::GetGameSpeed();

        float hours24 = tod * 24.f;
        int hour = static_cast<int>(hours24) % 24;
        int minute = static_cast<int>((hours24 - std::floor(hours24)) * 60.f);

        char buf[128];
        snprintf(buf, sizeof(buf), "Time: %02d:%02d (%.2f) | Speed: %.1fx",
                 hour, minute, tod, speed);
        return buf;
    });

    // /debug — Toggle debug info overlay
    Register("debug", "Toggle debug info overlay", [](const CommandArgs&) -> std::string {
        auto& core = Core::Get();
        core.GetNativeHud().ToggleLogPanel();
        return "Debug overlay toggled.";
    });

    // /entities — List all tracked entities by type
    Register("entities", "List all tracked entities", [](const CommandArgs&) -> std::string {
        auto& core = Core::Get();
        auto& er = core.GetEntityRegistry();

        size_t total = er.GetEntityCount();
        size_t remote = er.GetRemoteCount();
        size_t spawned = er.GetSpawnedRemoteCount();

        char buf[256];
        snprintf(buf, sizeof(buf),
                 "Entities: %d total | %d local | %d remote (%d spawned in world)",
                 (int)total, (int)(total - remote), (int)remote, (int)spawned);
        return buf;
    });

    // /ping — Show current ping to server
    Register("ping", "Show current ping to server", [](const CommandArgs&) -> std::string {
        auto& core = Core::Get();
        if (!core.IsConnected()) return "Not connected.";

        uint32_t ping = core.GetClient().GetPing();
        return "Ping: " + std::to_string(ping) + " ms";
    });

    // /kick <player> [reason] — Kick a player (host only)
    Register("kick", "Kick a player (host only)", [](const CommandArgs& args) -> std::string {
        auto& core = Core::Get();
        if (!core.IsConnected()) return "Not connected.";
        if (!core.IsHost()) return "Only the host can kick players.";
        if (args.args.empty()) return "Usage: /kick <name> [reason]";

        std::string targetName = args.args[0];
        std::string reason;
        for (size_t i = 1; i < args.args.size(); i++)
            reason += (i > 1 ? " " : "") + args.args[i];

        auto remotePlayers = core.GetPlayerController().GetAllRemotePlayers();
        PlayerID targetId = 0;
        for (auto& rp : remotePlayers) {
            std::string rpLower = rp.playerName;
            std::string tgtLower = targetName;
            std::transform(rpLower.begin(), rpLower.end(), rpLower.begin(), ::tolower);
            std::transform(tgtLower.begin(), tgtLower.end(), tgtLower.begin(), ::tolower);
            if (rpLower.find(tgtLower) == 0) { targetId = rp.playerId; break; }
        }
        if (targetId == 0) return "Player '" + targetName + "' not found.";

        MsgAdminCommand msg{};
        msg.commandType = 0; // kick
        msg.targetPlayerId = targetId;
        if (!reason.empty()) strncpy(msg.textParam, reason.c_str(), sizeof(msg.textParam) - 1);

        PacketWriter writer;
        writer.WriteHeader(MessageType::C2S_AdminCommand);
        writer.WriteRaw(&msg, sizeof(msg));
        core.GetClient().SendReliable(writer.Data(), writer.Size());
        return "Kick request sent.";
    });

    // /announce <message> — Broadcast system message (host only)
    Register("announce", "Broadcast system message (host only)", [](const CommandArgs& args) -> std::string {
        auto& core = Core::Get();
        if (!core.IsConnected()) return "Not connected.";
        if (!core.IsHost()) return "Only the host can announce.";
        if (args.args.empty()) return "Usage: /announce <message>";

        std::string message;
        for (size_t i = 0; i < args.args.size(); i++)
            message += (i > 0 ? " " : "") + args.args[i];

        MsgAdminCommand msg{};
        msg.commandType = 4; // announce
        strncpy(msg.textParam, message.c_str(), sizeof(msg.textParam) - 1);

        PacketWriter writer;
        writer.WriteHeader(MessageType::C2S_AdminCommand);
        writer.WriteRaw(&msg, sizeof(msg));
        core.GetClient().SendReliable(writer.Data(), writer.Size());
        return "Announcement sent.";
    });

    // ═══════════════════════════════════════════════════════════════════
    // DEBUG / REVERSE ENGINEERING TOOLS
    // ═══════════════════════════════════════════════════════════════════

    // /offsets — Dump all known offsets with verification status
    Register("offsets", "Dump all game offsets and their status", [](const CommandArgs&) -> std::string {
        auto& co = game::GetOffsets().character;
        auto& wo = game::GetOffsets().world;

        auto fmtOff = [](const char* name, int val) -> std::string {
            char buf[64];
            if (val >= 0)
                snprintf(buf, sizeof(buf), "\n  %-22s 0x%03X  OK", name, val);
            else
                snprintf(buf, sizeof(buf), "\n  %-22s  -1    UNKNOWN", name);
            return buf;
        };

        std::string r = "--- Character Offsets ---";
        r += fmtOff("name", co.name);
        r += fmtOff("faction", co.faction);
        r += fmtOff("position (read)", co.position);
        r += fmtOff("rotation", co.rotation);
        r += fmtOff("gameDataPtr", co.gameDataPtr);
        r += fmtOff("inventory", co.inventory);
        r += fmtOff("stats", co.stats);
        r += fmtOff("animClassOffset", co.animClassOffset);
        r += fmtOff("charMovementOffset", co.charMovementOffset);
        r += fmtOff("writablePosOffset", co.writablePosOffset);
        r += fmtOff("writablePosVecOff", co.writablePosVecOffset);
        r += fmtOff("squad", co.squad);
        r += fmtOff("equipment", co.equipment);
        r += fmtOff("isPlayerControlled", co.isPlayerControlled);
        r += fmtOff("health (direct)", co.health);
        r += fmtOff("healthChain1", co.healthChain1);
        r += fmtOff("healthChain2", co.healthChain2);
        r += fmtOff("healthBase", co.healthBase);
        r += fmtOff("moneyChain1", co.moneyChain1);
        r += fmtOff("moneyChain2", co.moneyChain2);
        r += fmtOff("moneyBase", co.moneyBase);
        r += fmtOff("sceneNode", co.sceneNode);
        r += fmtOff("aiPackage", co.aiPackage);
        r += "\n--- World Offsets ---";
        r += fmtOff("gameSpeed", wo.gameSpeed);
        r += fmtOff("characterList", wo.characterList);
        r += fmtOff("zoneManager", wo.zoneManager);
        r += fmtOff("timeOfDay", wo.timeOfDay);

        int known = 0, unknown = 0;
        auto count = [&](int v) { if (v >= 0) known++; else unknown++; };
        count(co.name); count(co.faction); count(co.position); count(co.rotation);
        count(co.animClassOffset); count(co.squad); count(co.equipment);
        count(co.isPlayerControlled); count(co.health); count(co.sceneNode);
        count(co.aiPackage);

        char summary[128];
        snprintf(summary, sizeof(summary), "\n--- %d known | %d unknown ---", known, unknown);
        r += summary;
        return r;
    });

    // /dump <hex_addr> [lines] — Hex dump memory at address
    Register("dump", "Hex dump memory (/dump <addr> [lines])", [](const CommandArgs& args) -> std::string {
        if (args.args.empty()) return "Usage: /dump <hex_address> [lines=4]";

        uintptr_t addr = 0;
        try {
            addr = std::stoull(args.args[0], nullptr, 16);
        } catch (...) {
            return "Invalid hex address.";
        }

        int lines = 4;
        if (args.args.size() >= 2) {
            try { lines = std::stoi(args.args[1]); } catch (...) {}
        }
        if (lines < 1) lines = 1;
        if (lines > 32) lines = 32;

        std::string r = "--- Memory dump at 0x" + args.args[0] + " ---";
        for (int line = 0; line < lines; line++) {
            uintptr_t lineAddr = addr + line * 16;
            char hexPart[80] = {};
            char asciiPart[20] = {};
            bool anyFail = false;

            for (int b = 0; b < 16; b++) {
                uint8_t byte = 0;
                if (Memory::Read(lineAddr + b, byte)) {
                    sprintf_s(hexPart + b * 3, 4, "%02X ", byte);
                    asciiPart[b] = (byte >= 0x20 && byte <= 0x7E) ? (char)byte : '.';
                } else {
                    sprintf_s(hexPart + b * 3, 4, "?? ");
                    asciiPart[b] = '?';
                    anyFail = true;
                }
            }
            asciiPart[16] = '\0';

            char lineBuf[160];
            snprintf(lineBuf, sizeof(lineBuf), "\n  %012llX  %s |%s|",
                     (unsigned long long)lineAddr, hexPart, asciiPart);
            r += lineBuf;

            if (anyFail) { r += " [READ FAIL]"; break; }
        }
        return r;
    });

    // /probe [list|<hex_addr>] — Read all known fields of a character.
    //   /probe          -- probe registry's primary character; falls back to
    //                      first character from CharacterIterator if registry
    //                      empty (works even with entity_hooks disabled)
    //   /probe list     -- list all known characters via CharacterIterator
    //   /probe <hex>    -- probe an explicit address
    Register("probe", "Probe character fields (/probe [list|<hex>])", [](const CommandArgs& args) -> std::string {
        auto& core = Core::Get();

        // ── Sub-mode: /probe list ──
        // Print a compact list — one valid character per line, chat-panel width
        // friendly (truncated names, no position).
        if (!args.args.empty() && args.args[0] == "list") {
            auto tracked = char_tracker_hooks::GetTrackedSnapshot();
            game::CharacterIterator it;
            std::string r = "Tracked=" + std::to_string(tracked.size()) +
                            ", iter count=" + std::to_string(it.Count()) + ":";
            char lbuf[128];
            int idx = 0, valid = 0;
            for (const auto& tc : tracked) {
                std::string nm = tc.name;
                if (nm.size() > 20) nm = nm.substr(0, 17) + "...";
                snprintf(lbuf, sizeof(lbuf),
                         "\n[T%d] 0x%012llX '%s'",
                         idx, (unsigned long long)tc.characterPtr,
                         nm.empty() ? "(noname)" : nm.c_str());
                r += lbuf;
                idx++;
                valid++;
                if (valid >= 30) { r += "\n... (more, truncated at 30)"; return r; }
            }
            while (it.HasNext()) {
                game::CharacterAccessor a = it.Next();
                void* obj = a.Raw();
                if (!obj) { idx++; continue; }
                std::string nm = a.GetName();
                if (nm.size() > 20) nm = nm.substr(0, 17) + "...";
                snprintf(lbuf, sizeof(lbuf),
                         "\n[%d] 0x%012llX '%s'",
                         idx, (unsigned long long)obj,
                         nm.empty() ? "(noname)" : nm.c_str());
                r += lbuf;
                idx++;
                valid++;
                if (valid >= 30) { r += "\n... (more, truncated at 30)"; break; }
            }
            if (valid == 0) r += "\n(no valid entries)";
            return r;
        }

        // ── Resolve target character ──
        void* primaryChar = nullptr;
        std::string source = "registry";

        // Explicit address arg: /probe 0x7FF6AABBCC00 or /probe 7FF6AABBCC00
        if (!args.args.empty()) {
            const std::string& s = args.args[0];
            std::string hex = s;
            if (hex.size() >= 2 && (hex.substr(0, 2) == "0x" || hex.substr(0, 2) == "0X"))
                hex = hex.substr(2);
            try {
                uintptr_t a = std::stoull(hex, nullptr, 16);
                if (a >= 0x10000 && a < 0x00007FFFFFFFFFFFULL) {
                    primaryChar = reinterpret_cast<void*>(a);
                    source = "explicit-arg";
                }
            } catch (...) {
                return "Usage: /probe [list|<hex_addr>]";
            }
        }

        // Fall back to registry
        if (!primaryChar) {
            primaryChar = core.GetPlayerController().GetPrimaryCharacter();
            if (primaryChar) source = "registry";
        }

        // Fall back to animation-tracker discoveries. This path is often available
        // before CharacterIterator can resolve the GameWorld character list.
        if (!primaryChar) {
            auto tracked = char_tracker_hooks::GetTrackedSnapshot();
            if (!tracked.empty() && tracked[0].characterPtr) {
                primaryChar = tracked[0].characterPtr;
                source = "char_tracker[0]";
            }
        }

        // Fall back to CharacterIterator (works without entity_hooks).
        // Walk until we find the first VALID slot — invalid entries (freed,
        // bad vtable, unaligned) return a null Raw() and we keep going.
        if (!primaryChar) {
            game::CharacterIterator it;
            int total = it.Count();
            int scanned = 0;
            while (it.HasNext()) {
                game::CharacterAccessor a = it.Next();
                scanned++;
                if (a.Raw()) {
                    primaryChar = a.Raw();
                    char sbuf[64];
                    snprintf(sbuf, sizeof(sbuf), "iter[%d/%d]", scanned - 1, total);
                    source = sbuf;
                    break;
                }
            }
        }

        if (!primaryChar) {
            int cnt = game::CharacterIterator().Count();
            char nbuf[160];
            snprintf(nbuf, sizeof(nbuf),
                     "No character. registry empty, iter count=%d (all invalid). Try /probe list.",
                     cnt);
            return std::string(nbuf);
        }

        uintptr_t ptr = reinterpret_cast<uintptr_t>(primaryChar);
        game::CharacterAccessor accessor(primaryChar);
        auto& co = game::GetOffsets().character;

        char buf[128];
        std::string r = "--- Character Probe (source: " + source + ") ---";
        snprintf(buf, sizeof(buf), "\n  Address:  0x%012llX", (unsigned long long)ptr);
        r += buf;

        // Name
        std::string name = accessor.GetName();
        r += "\n  Name:     " + (name.empty() ? "(empty)" : name);

        // Position
        Vec3 pos = accessor.GetPosition();
        snprintf(buf, sizeof(buf), "\n  Position: (%.1f, %.1f, %.1f)", pos.x, pos.y, pos.z);
        r += buf;

        // Rotation
        Quat rot = accessor.GetRotation();
        snprintf(buf, sizeof(buf), "\n  Rotation: (%.2f, %.2f, %.2f, %.2f)", rot.w, rot.x, rot.y, rot.z);
        r += buf;

        // Faction
        uintptr_t factionPtr = accessor.GetFactionPtr();
        if (factionPtr) {
            game::FactionAccessor faction(reinterpret_cast<void*>(factionPtr));
            std::string factionName = faction.GetName();
            snprintf(buf, sizeof(buf), "\n  Faction:  0x%llX '%s'",
                     (unsigned long long)factionPtr, factionName.c_str());
        } else {
            snprintf(buf, sizeof(buf), "\n  Faction:  (null)");
        }
        r += buf;

        // GameData
        uintptr_t gdPtr = accessor.GetGameDataPtr();
        if (gdPtr) {
            std::string gdName = SpawnManager::ReadKenshiString(gdPtr + 0x28);
            snprintf(buf, sizeof(buf), "\n  GameData: 0x%llX '%s'",
                     (unsigned long long)gdPtr, gdName.c_str());
        } else {
            snprintf(buf, sizeof(buf), "\n  GameData: (null)");
        }
        r += buf;

        // Squad
        uintptr_t squadPtr = accessor.GetSquadPtr();
        snprintf(buf, sizeof(buf), "\n  Squad:    0x%llX", (unsigned long long)squadPtr);
        r += buf;

        // Health chain
        float hp = accessor.GetHealth(BodyPart::Head);
        snprintf(buf, sizeof(buf), "\n  Health:   %.1f (head)", hp);
        r += buf;

        // Money
        int money = accessor.GetMoney();
        snprintf(buf, sizeof(buf), "\n  Money:    %d cats", money);
        r += buf;

        // AnimClass offset
        snprintf(buf, sizeof(buf), "\n  AnimClass offset: %s",
                 co.animClassOffset >= 0 ? std::to_string(co.animClassOffset).c_str() : "UNKNOWN");
        r += buf;

        // isPlayerControlled offset
        snprintf(buf, sizeof(buf), "\n  PlayerControlled offset: %s",
                 co.isPlayerControlled >= 0 ? std::to_string(co.isPlayerControlled).c_str() : "UNKNOWN");
        r += buf;

        // Write-position chain test
        if (co.animClassOffset >= 0) {
            uintptr_t animClass = 0;
            Memory::Read(ptr + co.animClassOffset, animClass);
            uintptr_t charMov = 0;
            if (animClass) Memory::Read(animClass + co.charMovementOffset, charMov);
            snprintf(buf, sizeof(buf), "\n  WritePos chain: animClass=0x%llX charMov=0x%llX",
                     (unsigned long long)animClass, (unsigned long long)charMov);
            r += buf;
            if (charMov) {
                uintptr_t posAddr = charMov + co.writablePosOffset + co.writablePosVecOffset;
                float wx = 0, wy = 0, wz = 0;
                Memory::Read(posAddr, wx);
                Memory::Read(posAddr + 4, wy);
                Memory::Read(posAddr + 8, wz);
                snprintf(buf, sizeof(buf), "\n  WritablePos:    (%.1f, %.1f, %.1f)", wx, wy, wz);
                r += buf;
            }
        }

        return r;
    });

    // /chars — List all characters visible to the mod
    Register("chars", "List all known characters", [](const CommandArgs&) -> std::string {
        auto& core = Core::Get();
        auto& registry = core.GetEntityRegistry();
        std::string r = "--- Registry Entities ---";

        auto localEntities = registry.GetPlayerEntities(core.GetLocalPlayerId());
        auto remoteEntities = registry.GetRemoteEntities();

        char buf[256];
        snprintf(buf, sizeof(buf), "\n  Local: %d  Remote: %d",
                 (int)localEntities.size(), (int)remoteEntities.size());
        r += buf;

        // Local entities
        for (EntityID eid : localEntities) {
            void* obj = registry.GetGameObject(eid);
            if (obj) {
                game::CharacterAccessor accessor(obj);
                Vec3 pos = accessor.GetPosition();
                std::string name = accessor.GetName();
                snprintf(buf, sizeof(buf), "\n  [L] #%u 0x%llX '%s' (%.0f,%.0f,%.0f)",
                         eid, (unsigned long long)obj, name.c_str(), pos.x, pos.y, pos.z);
            } else {
                snprintf(buf, sizeof(buf), "\n  [L] #%u (no game object)", eid);
            }
            r += buf;
        }

        // Remote entities
        for (EntityID eid : remoteEntities) {
            void* obj = registry.GetGameObject(eid);
            auto info = registry.GetInfo(eid);
            PlayerID owner = info.has_value() ? info->ownerPlayerId : 0;
            if (obj) {
                game::CharacterAccessor accessor(obj);
                Vec3 pos = accessor.GetPosition();
                std::string name = accessor.GetName();
                snprintf(buf, sizeof(buf), "\n  [R] #%u owner=%u 0x%llX '%s' (%.0f,%.0f,%.0f)",
                         eid, owner, (unsigned long long)obj, name.c_str(), pos.x, pos.y, pos.z);
            } else {
                snprintf(buf, sizeof(buf), "\n  [R] #%u owner=%u (no game object — pending spawn)",
                         eid, owner);
            }
            r += buf;
        }

        // CharacterIterator count
        game::CharacterIterator iter;
        snprintf(buf, sizeof(buf), "\n--- CharacterIterator: %d characters in world ---", iter.Count());
        r += buf;

        // Loading cache removed — CharacterIterator is the sole discovery path

        return r;
    });

    // /spawn — SpawnManager readiness and state
    Register("spawn", "Show spawn system status", [](const CommandArgs&) -> std::string {
        auto& core = Core::Get();
        auto& sm = core.GetSpawnManager();

        char buf[256];
        std::string r = "--- Spawn System Status ---";

        snprintf(buf, sizeof(buf), "\n  Factory ready:     %s", sm.IsReady() ? "YES" : "NO");
        r += buf;
        snprintf(buf, sizeof(buf), "\n  Pre-call data:     %s", sm.HasPreCallData() ? "YES" : "NO");
        r += buf;
        snprintf(buf, sizeof(buf), "\n  Request struct:    %s", sm.HasRequestStruct() ? "YES" : "NO");
        r += buf;
        snprintf(buf, sizeof(buf), "\n  Pending spawns:    %d", (int)sm.GetPendingSpawnCount());
        r += buf;
        snprintf(buf, sizeof(buf), "\n  Total templates:   %d", (int)sm.GetTemplateCount());
        r += buf;
        snprintf(buf, sizeof(buf), "\n  Factory templates: %d", (int)sm.GetFactoryTemplateCount());
        r += buf;
        snprintf(buf, sizeof(buf), "\n  Char templates:    %d", (int)sm.GetCharacterTemplateCount());
        r += buf;
        snprintf(buf, sizeof(buf), "\n  GDM pointer:       0x%llX",
                 (unsigned long long)sm.GetManagerPointer());
        r += buf;

        // Spawn path readiness
        bool inPlace = sm.IsReady() && sm.HasPreCallData();
        bool direct = sm.HasPreCallData();
        snprintf(buf, sizeof(buf), "\n  --- Spawn Paths ---");
        r += buf;
        snprintf(buf, sizeof(buf), "\n  In-place replay:   %s", inPlace ? "READY" : "NOT READY");
        r += buf;
        snprintf(buf, sizeof(buf), "\n  Direct spawn:      %s", direct ? "READY" : "NOT READY");
        r += buf;

        // In-place spawn stats
        int inPlaceCount = entity_hooks::GetInPlaceSpawnCount();
        bool recentSpawn = entity_hooks::HasRecentInPlaceSpawn(30);
        snprintf(buf, sizeof(buf), "\n  In-place spawns:   %d (recent: %s)",
                 inPlaceCount, recentSpawn ? "yes" : "no");
        r += buf;

        // Game loaded state
        snprintf(buf, sizeof(buf), "\n  Game loaded:       %s", core.IsGameLoaded() ? "YES" : "NO");
        r += buf;
        snprintf(buf, sizeof(buf), "\n  Connected:         %s", core.IsConnected() ? "YES" : "NO");
        r += buf;

        // Loading orchestrator state (spawn gating)
        auto& orch = core.GetLoadingOrch();
        const char* phaseName = "?";
        switch (orch.GetPhase()) {
            case LoadingPhase::Idle: phaseName = "Idle"; break;
            case LoadingPhase::InitialLoad: phaseName = "InitialLoad"; break;
            case LoadingPhase::ZoneTransition: phaseName = "ZoneTransition"; break;
            case LoadingPhase::SpawnLoad: phaseName = "SpawnLoad"; break;
        }
        r += "\n  --- Spawn Gate ---";
        snprintf(buf, sizeof(buf), "\n  Phase:             %s", phaseName);
        r += buf;
        snprintf(buf, sizeof(buf), "\n  Orch game loaded:  %s", orch.IsGameLoaded() ? "YES" : "NO");
        r += buf;
        snprintf(buf, sizeof(buf), "\n  In burst:          %s", orch.IsInBurst() ? "YES" : "NO");
        r += buf;
        snprintf(buf, sizeof(buf), "\n  Burst count:       %d", orch.GetBurstCount());
        r += buf;
        snprintf(buf, sizeof(buf), "\n  Can spawn now:     %s", AssetFacilitator::Get().CanSpawn() ? "YES" : "NO");
        r += buf;
        std::string blockReason = orch.GetSpawnBlockReason();
        snprintf(buf, sizeof(buf), "\n  Block reason:      %s", blockReason.c_str());
        r += buf;

        return r;
    });

    // /verify — Cross-verify offsets by reading a live character
    Register("verify", "Verify offsets against live character data", [](const CommandArgs&) -> std::string {
        auto& core = Core::Get();
        void* primaryChar = core.GetPlayerController().GetPrimaryCharacter();
        if (!primaryChar) return "No primary character — load a game first.";

        uintptr_t ptr = reinterpret_cast<uintptr_t>(primaryChar);
        auto& co = game::GetOffsets().character;
        std::string r = "--- Offset Verification ---";
        char buf[256];
        int pass = 0, fail = 0, skip = 0;

        auto check = [&](const char* name, int offset, auto validator) {
            if (offset < 0) { skip++; r += "\n  SKIP " + std::string(name); return; }
            if (validator(ptr + offset)) {
                pass++;
                snprintf(buf, sizeof(buf), "\n  PASS %-20s +0x%03X", name, offset);
            } else {
                fail++;
                snprintf(buf, sizeof(buf), "\n  FAIL %-20s +0x%03X", name, offset);
            }
            r += buf;
        };

        // Position: should be non-zero
        check("position", co.position, [](uintptr_t addr) {
            float x = 0, y = 0, z = 0;
            Memory::Read(addr, x); Memory::Read(addr + 4, y); Memory::Read(addr + 8, z);
            return (x != 0.f || y != 0.f || z != 0.f);
        });

        // Rotation: w should be near 1.0 for identity, and magnitude ~1
        check("rotation", co.rotation, [](uintptr_t addr) {
            float w = 0, x = 0, y = 0, z = 0;
            Memory::Read(addr, w); Memory::Read(addr + 4, x);
            Memory::Read(addr + 8, y); Memory::Read(addr + 12, z);
            float mag = w * w + x * x + y * y + z * z;
            return (mag > 0.5f && mag < 1.5f);
        });

        // Faction: should be a valid pointer
        check("faction", co.faction, [](uintptr_t addr) {
            uintptr_t val = 0;
            Memory::Read(addr, val);
            return (val > 0x10000 && val < 0x00007FFFFFFFFFFF);
        });

        // Name: should be a readable string (check SSO layout)
        check("name", co.name, [](uintptr_t addr) {
            uint64_t length = 0, capacity = 0;
            Memory::Read(addr + 0x10, length);
            Memory::Read(addr + 0x18, capacity);
            return (length > 0 && length < 200 && capacity >= length);
        });

        // GameData: should be a valid pointer
        check("gameDataPtr", co.gameDataPtr, [](uintptr_t addr) {
            uintptr_t val = 0;
            Memory::Read(addr, val);
            return (val > 0x10000 && val < 0x00007FFFFFFFFFFF);
        });

        // Inventory: should be a valid pointer
        check("inventory", co.inventory, [](uintptr_t addr) {
            uintptr_t val = 0;
            Memory::Read(addr, val);
            return (val > 0x10000 && val < 0x00007FFFFFFFFFFF);
        });

        // Stats: pointer or inline — should be non-zero region
        check("stats", co.stats, [](uintptr_t addr) {
            uintptr_t val = 0;
            Memory::Read(addr, val);
            return (val != 0);
        });

        // Health chain: follow pointer chain
        {
            r += "\n  --- Health Chain ---";
            uintptr_t step1 = 0, step2 = 0;
            float hp = 0;
            bool ok = false;
            Memory::Read(ptr + co.healthChain1, step1);
            if (step1 > 0x10000 && step1 < 0x00007FFFFFFFFFFF) {
                Memory::Read(step1 + co.healthChain2, step2);
                if (step2 > 0x10000 && step2 < 0x00007FFFFFFFFFFF) {
                    Memory::Read(step2 + co.healthBase, hp);
                    ok = (hp >= -100.f && hp <= 200.f);
                }
            }
            snprintf(buf, sizeof(buf), "\n  %s health chain: +%X -> 0x%llX -> +%X -> 0x%llX -> +%X -> %.1f",
                     ok ? "PASS" : "FAIL",
                     co.healthChain1, (unsigned long long)step1,
                     co.healthChain2, (unsigned long long)step2,
                     co.healthBase, hp);
            r += buf;
            if (ok) pass++; else fail++;
        }

        // AnimClass chain
        if (co.animClassOffset >= 0) {
            uintptr_t animClass = 0, charMov = 0;
            Memory::Read(ptr + co.animClassOffset, animClass);
            bool ok = false;
            if (animClass > 0x10000 && animClass < 0x00007FFFFFFFFFFF) {
                Memory::Read(animClass + co.charMovementOffset, charMov);
                if (charMov > 0x10000 && charMov < 0x00007FFFFFFFFFFF) {
                    float wx = 0;
                    Memory::Read(charMov + co.writablePosOffset + co.writablePosVecOffset, wx);
                    ok = (wx != 0.f); // writable position should match cached
                }
            }
            snprintf(buf, sizeof(buf), "\n  %s writePos chain: anim=0x%llX charMov=0x%llX",
                     ok ? "PASS" : "FAIL",
                     (unsigned long long)animClass, (unsigned long long)charMov);
            r += buf;
            if (ok) pass++; else fail++;
        } else {
            r += "\n  SKIP writePos chain (animClassOffset unknown)";
            skip++;
        }

        snprintf(buf, sizeof(buf), "\n--- %d PASS | %d FAIL | %d SKIP ---", pass, fail, skip);
        r += buf;
        return r;
    });

    // /validate_offsets - Compare KenshiLib-proposed character offsets against live tracked characters.
    Register("validate_offsets", "Validate proposed character offsets on tracked chars", [](const CommandArgs& args) -> std::string {
        auto tracked = char_tracker_hooks::GetTrackedSnapshot();
        if (tracked.empty()) return "No tracked characters yet. Load a save and wait for tracked > 0.";

        auto isPtr = [](uintptr_t v) {
            return v >= 0x10000 && v < 0x00007FFFFFFFFFFFULL && (v & 0x7) == 0;
        };
        auto readPtr = [&](uintptr_t base, int off, uintptr_t& out) {
            out = 0;
            return Memory::Read(base + off, out) && isPtr(out);
        };
        auto quatLooksPlausible = [&](uintptr_t addr, float* outVals) -> bool {
            float a = 0, b = 0, c = 0, d = 0;
            if (!Memory::Read(addr, a) || !Memory::Read(addr + 4, b) ||
                !Memory::Read(addr + 8, c) || !Memory::Read(addr + 12, d)) {
                return false;
            }
            if (outVals) {
                outVals[0] = a; outVals[1] = b; outVals[2] = c; outVals[3] = d;
            }
            float mag = a * a + b * b + c * c + d * d;
            auto sane = [](float v) { return std::isfinite(v) && v >= -1.05f && v <= 1.05f; };
            return sane(a) && sane(b) && sane(c) && sane(d) && mag > 0.50f && mag < 1.50f;
        };

        int sampleLimit = 8;
        if (!args.args.empty()) {
            try {
                sampleLimit = std::max(1, std::min(30, std::stoi(args.args[0])));
            } catch (...) {
                return "Usage: /validate_offsets [sample_count]";
            }
        }

        struct CandidateStats { int ok = 0; int total = 0; };
        CandidateStats rot58, rotB0, squad658, ai650, anim448;

        std::string r = "--- KenshiLib Offset Validation ---";
        r += "\nSamples: " + std::to_string(std::min<int>(sampleLimit, static_cast<int>(tracked.size()))) +
             " / tracked " + std::to_string(tracked.size());
        r += "\nCandidates: rot +0x58 vs +0xB0, anim +0x448, ai +0x650, squad +0x658";

        char buf[320];
        int shown = 0;
        for (const auto& tc : tracked) {
            if (!tc.characterPtr) continue;
            uintptr_t p = reinterpret_cast<uintptr_t>(tc.characterPtr);
            if (!isPtr(p)) continue;

            float q58[4] = {};
            float qB0[4] = {};
            bool ok58 = quatLooksPlausible(p + 0x58, q58);
            bool okB0 = quatLooksPlausible(p + 0xB0, qB0);
            rot58.total++; rotB0.total++;
            if (ok58) rot58.ok++;
            if (okB0) rotB0.ok++;

            uintptr_t vAnim = 0, vAi = 0, vSquad = 0;
            bool okAnim = readPtr(p, 0x448, vAnim);
            bool okAi = readPtr(p, 0x650, vAi);
            bool okSquad = readPtr(p, 0x658, vSquad);
            anim448.total++; ai650.total++; squad658.total++;
            if (okAnim) anim448.ok++;
            if (okAi) ai650.ok++;
            if (okSquad) squad658.ok++;

            if (shown < sampleLimit) {
                std::string nm = tc.name;
                if (nm.size() > 18) nm = nm.substr(0, 15) + "...";
                snprintf(buf, sizeof(buf),
                         "\n[%02d] 0x%012llX '%s'"
                         "\n  rot58 %s (%.2f %.2f %.2f %.2f)  rotB0 %s (%.2f %.2f %.2f %.2f)"
                         "\n  +448 anim=%s 0x%llX  +650 ai=%s 0x%llX  +658 squad=%s 0x%llX",
                         shown,
                         static_cast<unsigned long long>(p),
                         nm.empty() ? "(noname)" : nm.c_str(),
                         ok58 ? "OK" : "--", q58[0], q58[1], q58[2], q58[3],
                         okB0 ? "OK" : "--", qB0[0], qB0[1], qB0[2], qB0[3],
                         okAnim ? "OK" : "--", static_cast<unsigned long long>(vAnim),
                         okAi ? "OK" : "--", static_cast<unsigned long long>(vAi),
                         okSquad ? "OK" : "--", static_cast<unsigned long long>(vSquad));
                r += buf;
                shown++;
            }
        }

        auto addSummary = [&](const char* label, const CandidateStats& s) {
            snprintf(buf, sizeof(buf), "\n%-12s %d/%d plausible", label, s.ok, s.total);
            r += buf;
        };
        r += "\n--- Summary ---";
        addSummary("rot +0x58", rot58);
        addSummary("rot +0xB0", rotB0);
        addSummary("anim +0x448", anim448);
        addSummary("ai +0x650", ai650);
        addSummary("squad +0x658", squad658);

        if (rotB0.ok > rot58.ok) {
            r += "\nConclusion: rotation likely +0xB0.";
        } else if (rot58.ok > rotB0.ok) {
            r += "\nConclusion: rotation likely +0x58.";
        } else {
            r += "\nConclusion: rotation inconclusive; compare while rotating a character.";
        }
        return r;
    });

    // /anim_probe - Sample AnimationClassHuman memory for animation-state RE.
    Register("anim_probe", "Sample tracked AnimationClass bytes (/anim_probe [sample_count] [name_filter])", [](const CommandArgs& args) -> std::string {
        auto tracked = char_tracker_hooks::GetTrackedSnapshot();
        if (tracked.empty()) return "No tracked characters yet. Load a save and wait for tracked > 0.";

        int sampleLimit = 8;
        std::string nameFilter;
        if (!args.args.empty()) {
            size_t filterStart = 0;
            try {
                size_t consumed = 0;
                int parsedLimit = std::stoi(args.args[0], &consumed);
                if (consumed == args.args[0].size()) {
                    sampleLimit = std::max(1, std::min(30, parsedLimit));
                    filterStart = 1;
                }
            } catch (...) {
                filterStart = 0;
            }

            for (size_t i = filterStart; i < args.args.size(); ++i) {
                if (!nameFilter.empty()) nameFilter += " ";
                nameFilter += args.args[i];
            }
        }

        auto isPtr = [](uintptr_t v) {
            return v >= 0x10000 && v < 0x00007FFFFFFFFFFFULL && (v & 0x7) == 0;
        };
        auto lower = [](std::string s) {
            std::transform(s.begin(), s.end(), s.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return s;
        };
        std::string filterLower = lower(nameFilter);

        std::string r = "--- Animation Probe ---";
        r += "\nRun while idle, walking, running, and sneaking; compare KenshiOnline log lines tagged anim_probe.";
        if (!nameFilter.empty()) r += "\nFilter: " + nameFilter;
        r += "\nTracked: " + std::to_string(tracked.size());

        char buf[512];
        int shown = 0;
        for (const auto& tc : tracked) {
            if (shown >= sampleLimit) break;
            if (!filterLower.empty() && lower(tc.name).find(filterLower) == std::string::npos) {
                continue;
            }
            uintptr_t anim = reinterpret_cast<uintptr_t>(tc.animClassPtr);
            uintptr_t chr = reinterpret_cast<uintptr_t>(tc.characterPtr);
            if (!isPtr(anim) || !isPtr(chr)) continue;

            uint8_t b370[0x20] = {};
            bool okBytes = true;
            for (int i = 0; i < 0x20; ++i) {
                if (!Memory::Read(anim + 0x370 + i, b370[i])) {
                    okBytes = false;
                    break;
                }
            }

            uintptr_t movement = 0;
            bool okMovement = Memory::Read(anim + game::GetOffsets().character.charMovementOffset, movement) && isPtr(movement);
            uint8_t state37c = okBytes ? b370[0x0C] : 0;
            uint8_t state37d = okBytes ? b370[0x0D] : 0;
            uint8_t state37e = okBytes ? b370[0x0E] : 0;
            uint8_t state37f = okBytes ? b370[0x0F] : 0;

            std::string nm = tc.name;
            if (nm.size() > 18) nm = nm.substr(0, 15) + "...";

            char hex370[3 * 0x20 + 1] = {};
            int pos = 0;
            if (okBytes) {
                for (int i = 0; i < 0x20; ++i) {
                    pos += snprintf(hex370 + pos, sizeof(hex370) - pos, "%02X%s", b370[i], (i == 0x1F) ? "" : " ");
                }
            } else {
                snprintf(hex370, sizeof(hex370), "<read failed>");
            }

            snprintf(buf, sizeof(buf),
                     "anim_probe [%02d] '%s' char=0x%llX anim=0x%llX movement=%s0x%llX +37C=%u +37D=%u +37E=%u +37F=%u bytes370=%s",
                     shown,
                     nm.empty() ? "(noname)" : nm.c_str(),
                     static_cast<unsigned long long>(chr),
                     static_cast<unsigned long long>(anim),
                     okMovement ? "" : "!",
                     static_cast<unsigned long long>(movement),
                     state37c, state37d, state37e, state37f,
                     hex370);
            spdlog::info("{}", buf);

            r += "\n";
            r += buf;
            shown++;
        }

        if (shown == 0) {
            if (!nameFilter.empty()) return "No tracked characters matched '" + nameFilter + "' with valid AnimationClass pointers.";
            return "No tracked characters had valid AnimationClass pointers.";
        }
        r += "\nProbe lines were written to the KenshiOnline log.";
        return r;
    });

    // /anim_watch <name_filter> [range_hex] - Diff AnimationClass bytes between calls.
    Register("anim_watch", "Diff AnimationClass bytes across calls (/anim_watch <name> [range_hex])", [](const CommandArgs& args) -> std::string {
        if (args.args.empty()) return "Usage: /anim_watch <name_filter> [range_hex=0x800]";

        std::string nameFilter = args.args[0];
        size_t range = 0x800;
        if (args.args.size() >= 2) {
            try {
                range = static_cast<size_t>(std::stoul(args.args[1], nullptr, 16));
                range = std::max<size_t>(0x100, std::min<size_t>(0x2000, range));
            } catch (...) {
                return "Usage: /anim_watch <name_filter> [range_hex=0x800]";
            }
        }

        auto lower = [](std::string s) {
            std::transform(s.begin(), s.end(), s.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return s;
        };
        auto isPtr = [](uintptr_t v) {
            return v >= 0x10000 && v < 0x00007FFFFFFFFFFFULL && (v & 0x7) == 0;
        };

        std::string filterLower = lower(nameFilter);
        auto tracked = char_tracker_hooks::GetTrackedSnapshot();
        const char_tracker_hooks::TrackedChar* match = nullptr;
        for (const auto& tc : tracked) {
            if (lower(tc.name).find(filterLower) != std::string::npos) {
                match = &tc;
                break;
            }
        }
        if (!match) return "No tracked character matched '" + nameFilter + "'.";

        uintptr_t anim = reinterpret_cast<uintptr_t>(match->animClassPtr);
        uintptr_t chr = reinterpret_cast<uintptr_t>(match->characterPtr);
        if (!isPtr(anim) || !isPtr(chr)) return "Matched character has invalid AnimationClass pointer.";

        std::vector<uint8_t> current(range);
        for (size_t i = 0; i < range; ++i) {
            if (!Memory::Read(anim + i, current[i])) {
                return "Failed reading AnimationClass at offset 0x" + std::to_string(i) + ".";
            }
        }

        static uintptr_t s_watchAnim = 0;
        static std::string s_watchName;
        static std::vector<uint8_t> s_watchBytes;
        static int s_watchSeq = 0;

        char buf[512];
        std::string r = "--- Animation Watch ---";
        snprintf(buf, sizeof(buf), "\nname='%s' char=0x%llX anim=0x%llX range=0x%zX",
                 match->name.c_str(),
                 static_cast<unsigned long long>(chr),
                 static_cast<unsigned long long>(anim),
                 range);
        r += buf;

        if (s_watchAnim != anim || s_watchBytes.size() != current.size()) {
            s_watchAnim = anim;
            s_watchName = match->name;
            s_watchBytes = current;
            s_watchSeq = 0;
            snprintf(buf, sizeof(buf), "anim_watch baseline name='%s' char=0x%llX anim=0x%llX range=0x%zX",
                     match->name.c_str(),
                     static_cast<unsigned long long>(chr),
                     static_cast<unsigned long long>(anim),
                     range);
            spdlog::info("{}", buf);
            r += "\nBaseline captured. Run again after changing movement state.";
            return r;
        }

        struct Change {
            size_t off;
            uint8_t oldVal;
            uint8_t newVal;
        };
        std::vector<Change> changes;
        changes.reserve(128);
        for (size_t i = 0; i < current.size(); ++i) {
            if (current[i] != s_watchBytes[i]) {
                changes.push_back({i, s_watchBytes[i], current[i]});
            }
        }

        s_watchSeq++;
        snprintf(buf, sizeof(buf),
                 "anim_watch #%d name='%s' char=0x%llX anim=0x%llX changed=%zu range=0x%zX",
                 s_watchSeq,
                 match->name.c_str(),
                 static_cast<unsigned long long>(chr),
                 static_cast<unsigned long long>(anim),
                 changes.size(),
                 range);
        spdlog::info("{}", buf);
        r += "\n";
        r += buf;

        size_t maxChanges = std::min<size_t>(changes.size(), 80);
        for (size_t i = 0; i < maxChanges; ++i) {
            const auto& c = changes[i];
            float f = 0.f;
            bool hasFloat = (c.off + sizeof(float) <= current.size()) &&
                            Memory::Read(anim + c.off, f) &&
                            std::isfinite(f) && std::fabs(f) < 1000000.f;
            if (hasFloat) {
                snprintf(buf, sizeof(buf), " +0x%04zX:%02X->%02X f=%.3f",
                         c.off, c.oldVal, c.newVal, f);
            } else {
                snprintf(buf, sizeof(buf), " +0x%04zX:%02X->%02X",
                         c.off, c.oldVal, c.newVal);
            }
            spdlog::info("anim_watch{}", buf);
            r += "\n";
            r += buf;
        }
        if (changes.size() > maxChanges) {
            snprintf(buf, sizeof(buf), "\n... %zu more byte changes omitted", changes.size() - maxChanges);
            r += buf;
        }

        s_watchBytes = std::move(current);
        return r;
    });

    // /scan <charptr> [start] [end] — Scan character memory for pointers/values
    // /anim_trace <name_filter> [seconds] [interval_ms] - Timed animation-state sampler.
    Register("anim_trace", "Trace animation fields over time (/anim_trace <name|stop> [seconds] [interval_ms])", [](const CommandArgs& args) -> std::string {
        if (args.args.empty()) return "Usage: /anim_trace <name_filter|stop> [seconds=12] [interval_ms=250]";

        if (LowerCopy(args.args[0]) == "stop") {
            std::lock_guard lock(g_animTrace.mutex);
            bool wasActive = g_animTrace.active;
            g_animTrace.active = false;
            spdlog::info("anim_trace stopped by command filter='{}' samples={}",
                         g_animTrace.filter, g_animTrace.sampleIndex);
            return wasActive ? "Animation trace stopped." : "Animation trace was not active.";
        }

        float seconds = 12.0f;
        int intervalMs = 250;
        size_t nameArgCount = args.args.size();

        if (nameArgCount >= 2) {
            try {
                size_t consumed = 0;
                seconds = std::stof(args.args.back(), &consumed);
                if (consumed == args.args.back().size()) {
                    nameArgCount--;
                }
            } catch (...) {
            }
        }
        if (nameArgCount >= 2 && args.args.size() - nameArgCount == 1) {
            try {
                size_t consumed = 0;
                intervalMs = std::stoi(args.args.back(), &consumed);
                if (consumed == args.args.back().size()) {
                    nameArgCount--;
                    seconds = std::stof(args.args[nameArgCount], nullptr);
                }
            } catch (...) {
                seconds = 12.0f;
                intervalMs = 250;
                nameArgCount = args.args.size();
            }
        }

        std::string filter;
        for (size_t i = 0; i < nameArgCount; ++i) {
            if (!filter.empty()) filter += " ";
            filter += args.args[i];
        }
        if (filter.empty()) return "Usage: /anim_trace <name_filter|stop> [seconds=12] [interval_ms=250]";

        seconds = std::max(1.0f, std::min(60.0f, seconds));
        intervalMs = std::max(50, std::min(2000, intervalMs));

        auto tracked = char_tracker_hooks::GetTrackedSnapshot();
        const auto* match = FindTrackedByFilter(tracked, filter);
        if (!match) return "No tracked character matched '" + filter + "'.";
        uintptr_t anim = reinterpret_cast<uintptr_t>(match->animClassPtr);
        uintptr_t chr = reinterpret_cast<uintptr_t>(match->characterPtr);
        if (!IsLikelyPtr(anim) || !IsLikelyPtr(chr)) return "Matched character has invalid AnimationClass pointer.";

        {
            std::lock_guard lock(g_animTrace.mutex);
            g_animTrace.active = true;
            g_animTrace.filter = filter;
            g_animTrace.elapsed = 0.0f;
            g_animTrace.duration = seconds;
            g_animTrace.interval = static_cast<float>(intervalMs) / 1000.0f;
            g_animTrace.sampleAccum = g_animTrace.interval;
            g_animTrace.sampleIndex = 0;
            g_animTrace.hasPrevPos = false;
        }

        spdlog::info("anim_trace start filter='{}' matched='{}' char=0x{:X} anim=0x{:X} duration={:.2f}s interval={}ms",
                     filter, match->name, chr, anim, seconds, intervalMs);

        char buf[256];
        snprintf(buf, sizeof(buf), "Animation trace started for '%s' for %.1fs at %dms. Move normally; samples write to the log.",
                 match->name.c_str(), seconds, intervalMs);
        return buf;
    });

    // /order_trace <on|off|status> - Log selected-character order dispatches.
    Register("order_trace", "Trace selected-character orders (/order_trace on|off|status)", [](const CommandArgs& args) -> std::string {
        if (args.args.empty() || LowerCopy(args.args[0]) == "status") {
            char buf[128];
            snprintf(buf, sizeof(buf), "Order trace is %s. Local order flags: 0x%04X. Run order: %u.",
                     order_hooks::IsTraceEnabled() ? "on" : "off",
                     order_hooks::GetLocalOrderFlags(),
                     order_hooks::GetLastRunSpeedOrder());
            return buf;
        }

        std::string mode = LowerCopy(args.args[0]);
        if (mode == "on" || mode == "start" || mode == "1") {
            order_hooks::SetTraceEnabled(true);
            return "Order trace enabled. Toggle sneak/block/passive/hold/ranged/taunt/run speed; events write to the log.";
        }
        if (mode == "off" || mode == "stop" || mode == "0") {
            order_hooks::SetTraceEnabled(false);
            return "Order trace disabled.";
        }

        return "Usage: /order_trace on|off|status";
    });

    // /scan <charptr> [start] [end] — Scan character memory for pointers/values
    // /anim_state <name_filter> - Read high-signal AnimationClass candidate fields.
    Register("anim_state", "Read animation-state candidate fields (/anim_state <name>)", [](const CommandArgs& args) -> std::string {
        if (args.args.empty()) return "Usage: /anim_state <name_filter>";

        std::string nameFilter = args.args[0];
        for (size_t i = 1; i < args.args.size(); ++i) {
            nameFilter += " ";
            nameFilter += args.args[i];
        }

        auto lower = [](std::string s) {
            std::transform(s.begin(), s.end(), s.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return s;
        };
        auto isPtr = [](uintptr_t v) {
            return v >= 0x10000 && v < 0x00007FFFFFFFFFFFULL && (v & 0x7) == 0;
        };

        std::string filterLower = lower(nameFilter);
        auto tracked = char_tracker_hooks::GetTrackedSnapshot();
        const char_tracker_hooks::TrackedChar* match = nullptr;
        for (const auto& tc : tracked) {
            if (lower(tc.name).find(filterLower) != std::string::npos) {
                match = &tc;
                break;
            }
        }
        if (!match) return "No tracked character matched '" + nameFilter + "'.";

        uintptr_t anim = reinterpret_cast<uintptr_t>(match->animClassPtr);
        uintptr_t chr = reinterpret_cast<uintptr_t>(match->characterPtr);
        if (!isPtr(anim) || !isPtr(chr)) return "Matched character has invalid AnimationClass pointer.";

        game::CharacterAccessor accessor(reinterpret_cast<void*>(chr));
        Vec3 pos = accessor.GetPosition();

        struct Candidate {
            size_t off;
            const char* label;
        };
        static constexpr Candidate candidates[] = {
            {0x0C8, "anim_time"},
            {0x334, "current_kind"},
            {0x340, "active_data0"},
            {0x344, "active_data1"},
            {0x348, "active_data2"},
            {0x34C, "active_data3"},
            {0x350, "active_data4"},
            {0x020, "watch_partial"},
            {0x530, "anim_state_a"},
            {0x570, "anim_state_b"},
        };

        char buf[512];
        snprintf(buf, sizeof(buf),
                 "anim_state name='%s' char=0x%llX anim=0x%llX pos=(%.2f, %.2f, %.2f)",
                 match->name.c_str(),
                 static_cast<unsigned long long>(chr),
                 static_cast<unsigned long long>(anim),
                 pos.x, pos.y, pos.z);
        spdlog::info("{}", buf);

        std::string r = "--- Animation State ---";
        r += "\n";
        r += buf;

        for (const auto& c : candidates) {
            uint8_t u8 = 0;
            uint16_t u16 = 0;
            uint32_t u32 = 0;
            bool ok8 = Memory::Read(anim + c.off, u8);
            bool ok16 = Memory::Read(anim + c.off, u16);
            bool ok32 = Memory::Read(anim + c.off, u32);
            if (!ok8 || !ok16 || !ok32) {
                snprintf(buf, sizeof(buf), "anim_state +0x%03zX %-14s READ_FAIL", c.off, c.label);
            } else {
                snprintf(buf, sizeof(buf),
                         "anim_state +0x%03zX %-14s u8=%u u16=%u u32=%u hex=%02X %02X %02X %02X",
                         c.off, c.label,
                         static_cast<unsigned>(u8),
                         static_cast<unsigned>(u16),
                         static_cast<unsigned>(u32),
                         static_cast<unsigned>(u32 & 0xFF),
                         static_cast<unsigned>((u32 >> 8) & 0xFF),
                         static_cast<unsigned>((u32 >> 16) & 0xFF),
                         static_cast<unsigned>((u32 >> 24) & 0xFF));
            }
            spdlog::info("{}", buf);
            r += "\n";
            r += buf;
        }

        return r;
    });

    Register("scan", "Scan char struct for pointers (/scan <addr> [start] [end])", [](const CommandArgs& args) -> std::string {
        if (args.args.empty()) return "Usage: /scan <hex_addr> [start_offset=0] [end_offset=0x200]";

        uintptr_t addr = 0;
        try { addr = std::stoull(args.args[0], nullptr, 16); }
        catch (...) { return "Invalid hex address."; }

        int startOff = 0, endOff = 0x200;
        if (args.args.size() >= 2)
            try { startOff = std::stoi(args.args[1], nullptr, 16); } catch (...) {}
        if (args.args.size() >= 3)
            try { endOff = std::stoi(args.args[2], nullptr, 16); } catch (...) {}

        if (endOff > 0x1000) endOff = 0x1000;
        if (endOff - startOff > 0x400) endOff = startOff + 0x400; // Max 64 lines

        std::string r = "--- Pointer scan 0x" + args.args[0] + " ---";
        char buf[256];

        for (int off = startOff; off < endOff; off += 8) {
            uintptr_t val = 0;
            if (!Memory::Read(addr + off, val)) {
                snprintf(buf, sizeof(buf), "\n  +0x%03X: READ FAIL", off);
                r += buf;
                break;
            }

            // Classify the value
            const char* tag = "";
            if (val == 0) {
                tag = "(null)";
            } else if (val > 0x10000 && val < 0x00007FFFFFFFFFFF) {
                // Looks like a pointer — try to read a string at val+0x28 (GameData name)
                std::string name = SpawnManager::ReadKenshiString(val + 0x28);
                if (!name.empty() && name.length() > 1 && name.length() < 100) {
                    snprintf(buf, sizeof(buf), "\n  +0x%03X: 0x%012llX  PTR -> name='%s'",
                             off, (unsigned long long)val, name.c_str());
                    r += buf;
                    continue;
                }
                // Try reading name at val+0x10 (Kenshi std::string at different layout)
                name = SpawnManager::ReadKenshiString(val + 0x10);
                if (!name.empty() && name.length() > 1 && name.length() < 100) {
                    snprintf(buf, sizeof(buf), "\n  +0x%03X: 0x%012llX  PTR -> +10='%s'",
                             off, (unsigned long long)val, name.c_str());
                    r += buf;
                    continue;
                }
                tag = "PTR";
            } else {
                // Try interpreting as float pair
                float f1 = 0, f2 = 0;
                memcpy(&f1, &val, 4);
                memcpy(&f2, reinterpret_cast<const char*>(&val) + 4, 4);
                if (std::abs(f1) > 0.001f && std::abs(f1) < 1e6f &&
                    std::abs(f2) > 0.001f && std::abs(f2) < 1e6f) {
                    snprintf(buf, sizeof(buf), "\n  +0x%03X: 0x%016llX  float(%.2f, %.2f)",
                             off, (unsigned long long)val, f1, f2);
                    r += buf;
                    continue;
                }
                tag = "";
            }

            snprintf(buf, sizeof(buf), "\n  +0x%03X: 0x%016llX  %s",
                     off, (unsigned long long)val, tag);
            r += buf;
        }
        return r;
    });

    // /hooks — Hook status dashboard (debug tool)
    Register("hooks", "Show all hook status and prologue bytes", [](const CommandArgs&) -> std::string {
        auto diags = HookManager::Get().GetDiagnostics();
        if (diags.empty()) return "No hooks installed.";

        std::string result = "--- Hook Status ---";
        int active = 0, movRaxCount = 0;

        for (auto& d : diags) {
            char line[256];
            char prologueStr[32];
            snprintf(prologueStr, sizeof(prologueStr),
                     "%02X %02X %02X %02X %02X %02X %02X %02X",
                     d.prologue[0], d.prologue[1], d.prologue[2], d.prologue[3],
                     d.prologue[4], d.prologue[5], d.prologue[6], d.prologue[7]);

            const char* mode = "trampoline";
            if (d.isVtable) mode = "vtable";
            // Check if prologue starts with mov rax, rsp (48 8B C4)
            bool isMovRax = (d.prologue[0] == 0x48 && d.prologue[1] == 0x8B && d.prologue[2] == 0xC4);
            if (isMovRax) { mode = "tramp+movrax"; movRaxCount++; }

            snprintf(line, sizeof(line), "\n%-20s 0x%012llX  %s  [%s]  %s  calls:%d crash:%d",
                     d.name.c_str(),
                     static_cast<unsigned long long>(d.targetAddr),
                     d.enabled ? "ON " : "OFF",
                     prologueStr,
                     mode,
                     d.callCount,
                     d.crashCount);
            result += line;
            if (d.enabled) active++;
        }

        char summary[128];
        snprintf(summary, sizeof(summary),
                 "\n--- %d total | %d active | %d mov-rax-rsp ---",
                 (int)diags.size(), active, movRaxCount);
        result += summary;
        return result;
    });

    // ── Pipeline debugger ──
    Register("pipeline", "Pipeline debugger (/pipeline [status|entity <id>])",
        [](const CommandArgs& args) -> std::string {
            auto& pipe = Core::Get().GetPipelineOrch();

            if (args.args.empty()) {
                pipe.ToggleHud();
                return pipe.IsHudVisible() ? "Pipeline HUD enabled." : "Pipeline HUD disabled.";
            }

            if (args.args[0] == "status") {
                return pipe.FormatStatusDump();
            }

            if (args.args[0] == "entity" && args.args.size() >= 2) {
                try {
                    EntityID eid = static_cast<EntityID>(std::stoul(args.args[1]));
                    return pipe.FormatEntityTrack(eid);
                } catch (...) {
                    return "Invalid entity ID. Usage: /pipeline entity <id>";
                }
            }

            return "Usage: /pipeline [status|entity <id>]";
        });

    // /discover — Runtime offset discovery using anchor-based scanning
    // Scans the character struct to FIND where fields actually live,
    // instead of assuming hardcoded GOG offsets are correct.
    Register("discover", "Discover character offsets by scanning struct memory", [](const CommandArgs& args) -> std::string {
        auto& core = Core::Get();
        void* primaryChar = core.GetPlayerController().GetPrimaryCharacter();
        if (!primaryChar) return "No primary character — load a game first.";

        uintptr_t charPtr = reinterpret_cast<uintptr_t>(primaryChar);
        auto& co = game::GetOffsets().character;
        char buf[512];
        std::string r = "--- Offset Discovery ---";
        snprintf(buf, sizeof(buf), "\n  Character at 0x%012llX", (unsigned long long)charPtr);
        r += buf;

        // ══════════════════════════════════════════════════════════════
        //  STEP 0: Hex dump first 0x500 bytes to LOG FILE for analysis
        // ══════════════════════════════════════════════════════════════
        spdlog::info("=== CHARACTER STRUCT HEX DUMP (0x500 bytes) at 0x{:012X} ===", charPtr);
        for (int row = 0; row < 0x500; row += 16) {
            char hexLine[128] = {};
            char asciiLine[20] = {};
            int pos = 0;
            for (int b = 0; b < 16; b++) {
                uint8_t byte = 0;
                Memory::Read(charPtr + row + b, byte);
                pos += sprintf_s(hexLine + pos, sizeof(hexLine) - pos, "%02X ", byte);
                asciiLine[b] = (byte >= 0x20 && byte <= 0x7E) ? (char)byte : '.';
            }
            asciiLine[16] = '\0';
            spdlog::info("  +{:03X}: {} |{}|", row, hexLine, asciiLine);
        }
        spdlog::info("=== END HEX DUMP ===");
        r += "\n  Hex dump (0x500 bytes) written to log file.";

        // Helper: check if value looks like a valid heap pointer
        uintptr_t modBase = Memory::GetModuleBase();
        auto isHeapPtr = [modBase](uintptr_t val) -> bool {
            if (val < 0x10000 || val >= 0x00007FFFFFFFFFFF) return false;
            if ((val & 0x7) != 0) return false;
            if (val >= modBase && val < modBase + 0x4000000) return false;
            return true;
        };

        // Read known anchor: position from the existing accessor
        game::CharacterAccessor accessor(primaryChar);
        Vec3 knownPos = accessor.GetPosition();
        std::string knownName = accessor.GetName();

        snprintf(buf, sizeof(buf), "\n  Known position: (%.1f, %.1f, %.1f)", knownPos.x, knownPos.y, knownPos.z);
        r += buf;
        r += "\n  Known name: " + knownName;

        int matches = 0, mismatches = 0;

        // ══════════════════════════════════════════════════════════════
        //  STEP 1: Scan for POSITION (3 consecutive floats)
        // ══════════════════════════════════════════════════════════════
        r += "\n\n  --- Position Scan ---";
        bool posNonZero = (knownPos.x != 0.f || knownPos.y != 0.f || knownPos.z != 0.f);
        int discoveredPosOffset = -1;
        if (posNonZero) {
            for (int off = 0; off < 0x400; off += 4) {
                float fx = 0, fy = 0, fz = 0;
                Memory::Read(charPtr + off, fx);
                Memory::Read(charPtr + off + 4, fy);
                Memory::Read(charPtr + off + 8, fz);
                if (std::abs(fx - knownPos.x) < 0.5f &&
                    std::abs(fy - knownPos.y) < 0.5f &&
                    std::abs(fz - knownPos.z) < 0.5f) {
                    snprintf(buf, sizeof(buf), "\n  FOUND position at +0x%03X (%.1f, %.1f, %.1f)", off, fx, fy, fz);
                    r += buf;
                    spdlog::info("DISCOVER: Position at +0x{:03X} = ({:.1f}, {:.1f}, {:.1f})", off, fx, fy, fz);
                    if (discoveredPosOffset == -1) discoveredPosOffset = off;
                }
            }
            if (discoveredPosOffset == co.position) {
                r += "\n  OK: Matches hardcoded +0x" + std::to_string(co.position);
                matches++;
            } else if (discoveredPosOffset >= 0) {
                snprintf(buf, sizeof(buf), "\n  MISMATCH: Hardcoded=+0x%03X, Found=+0x%03X", co.position, discoveredPosOffset);
                r += buf;
                mismatches++;
            } else {
                r += "\n  NOT FOUND";
                mismatches++;
            }
        } else {
            r += "\n  SKIP (position is zero — character may not be loaded)";
        }

        // ══════════════════════════════════════════════════════════════
        //  STEP 2: Scan for NAME (MSVC std::string pattern)
        //  Look for size/capacity pair where size matches known name length
        // ══════════════════════════════════════════════════════════════
        r += "\n\n  --- Name Scan ---";
        int discoveredNameOffset = -1;
        if (!knownName.empty() && knownName != "Unknown") {
            uint64_t expectedSize = knownName.size();
            for (int off = 0; off < 0x400; off += 8) {
                uint64_t size = 0, capacity = 0;
                Memory::Read(charPtr + off + 0x10, size);
                Memory::Read(charPtr + off + 0x18, capacity);
                if (size != expectedSize || capacity < size || capacity > 256) continue;

                // Try reading the actual string to verify
                char testBuf[257] = {};
                bool readable = true;
                if (capacity <= 15) {
                    // SSO: inline
                    for (size_t i = 0; i < size && i < 256; i++) {
                        if (!Memory::Read(charPtr + off + i, testBuf[i])) { readable = false; break; }
                    }
                } else {
                    // Heap: pointer at +0x00
                    uintptr_t dataPtr = 0;
                    Memory::Read(charPtr + off, dataPtr);
                    if (dataPtr < 0x10000 || dataPtr >= 0x00007FFFFFFFFFFF) continue;
                    for (size_t i = 0; i < size && i < 256; i++) {
                        if (!Memory::Read(dataPtr + i, testBuf[i])) { readable = false; break; }
                    }
                }
                if (!readable) continue;
                std::string foundName(testBuf, (size_t)size);
                if (foundName == knownName) {
                    snprintf(buf, sizeof(buf), "\n  FOUND name at +0x%03X = '%s'", off, foundName.c_str());
                    r += buf;
                    spdlog::info("DISCOVER: Name at +0x{:03X} = '{}'", off, foundName);
                    if (discoveredNameOffset == -1) discoveredNameOffset = off;
                }
            }
            if (discoveredNameOffset == co.name) {
                matches++;
                r += "\n  OK: Matches hardcoded";
            } else if (discoveredNameOffset >= 0) {
                snprintf(buf, sizeof(buf), "\n  MISMATCH: Hardcoded=+0x%03X, Found=+0x%03X", co.name, discoveredNameOffset);
                r += buf;
                mismatches++;
            } else {
                r += "\n  NOT FOUND";
                mismatches++;
            }
        } else {
            r += "\n  SKIP (name unknown)";
        }

        // ══════════════════════════════════════════════════════════════
        //  STEP 3: Scan for FACTION (valid heap pointer with name at +0x10)
        // ══════════════════════════════════════════════════════════════
        r += "\n\n  --- Faction Pointer Scan ---";
        int discoveredFactionOffset = -1;
        for (int off = 0; off < 0x100; off += 8) {
            uintptr_t candidate = 0;
            Memory::Read(charPtr + off, candidate);
            if (!isHeapPtr(candidate)) continue;

            // A faction should have a readable name at +0x10
            uint64_t fNameSize = 0, fNameCap = 0;
            Memory::Read(candidate + 0x10 + 0x10, fNameSize);
            Memory::Read(candidate + 0x10 + 0x18, fNameCap);
            if (fNameSize < 1 || fNameSize > 100 || fNameCap < fNameSize || fNameCap > 256) continue;

            // Read the faction name
            char fNameBuf[101] = {};
            bool fReadable = true;
            if (fNameCap <= 15) {
                for (size_t i = 0; i < fNameSize && i < 100; i++)
                    if (!Memory::Read(candidate + 0x10 + i, fNameBuf[i])) { fReadable = false; break; }
            } else {
                uintptr_t fDataPtr = 0;
                Memory::Read(candidate + 0x10, fDataPtr);
                if (fDataPtr < 0x10000) continue;
                for (size_t i = 0; i < fNameSize && i < 100; i++)
                    if (!Memory::Read(fDataPtr + i, fNameBuf[i])) { fReadable = false; break; }
            }
            if (!fReadable) continue;

            // Validate: name should be printable ASCII
            bool allAscii = true;
            for (size_t i = 0; i < fNameSize; i++)
                if (fNameBuf[i] < 0x20 || fNameBuf[i] > 0x7E) { allAscii = false; break; }
            if (!allAscii) continue;

            snprintf(buf, sizeof(buf), "\n  FOUND faction ptr at +0x%03X -> 0x%llX name='%s'",
                     off, (unsigned long long)candidate, fNameBuf);
            r += buf;
            spdlog::info("DISCOVER: Faction at +0x{:03X} -> 0x{:X} name='{}'", off, candidate, fNameBuf);

            // Also try reading faction ID at candidate+0x08
            uint32_t factionId = 0;
            const int fIdOff = game::GetOffsets().faction.id;
            if (fIdOff >= 0) {
                Memory::Read(candidate + fIdOff, factionId);
            }
            snprintf(buf, sizeof(buf), " (id=%u)", factionId);
            r += buf;

            if (discoveredFactionOffset == -1) discoveredFactionOffset = off;
        }
        if (discoveredFactionOffset == co.faction) {
            matches++;
            r += "\n  OK: Matches hardcoded +0x" + std::to_string(co.faction);
        } else if (discoveredFactionOffset >= 0) {
            snprintf(buf, sizeof(buf), "\n  MISMATCH: Hardcoded=+0x%03X, Found=+0x%03X", co.faction, discoveredFactionOffset);
            r += buf;
            mismatches++;
        } else {
            r += "\n  NOT FOUND (no pointer with readable name at +0x10)";
            mismatches++;
        }

        // ══════════════════════════════════════════════════════════════
        //  STEP 4: Scan for GAMEDATA (pointer to object with name at +0x28)
        // ══════════════════════════════════════════════════════════════
        r += "\n\n  --- GameData Pointer Scan ---";
        int discoveredGDOffset = -1;
        for (int off = 0; off < 0x200; off += 8) {
            uintptr_t candidate = 0;
            Memory::Read(charPtr + off, candidate);
            if (!isHeapPtr(candidate)) continue;

            std::string gdName = SpawnManager::ReadKenshiString(candidate + 0x28);
            if (gdName.empty() || gdName.size() < 2 || gdName.size() > 64) continue;

            // Validate: printable ASCII
            bool ok = true;
            for (char c : gdName) if (c < 0x20 || c > 0x7E) { ok = false; break; }
            if (!ok) continue;

            // Check for GameDataManager* at candidate+0x10 (should be consistent)
            uintptr_t gdmPtr = 0;
            Memory::Read(candidate + 0x10, gdmPtr);

            snprintf(buf, sizeof(buf), "\n  FOUND GameData at +0x%03X -> 0x%llX name='%s' mgr=0x%llX",
                     off, (unsigned long long)candidate, gdName.c_str(), (unsigned long long)gdmPtr);
            r += buf;
            spdlog::info("DISCOVER: GameData at +0x{:03X} -> 0x{:X} name='{}' mgr=0x{:X}", off, candidate, gdName, gdmPtr);
            if (discoveredGDOffset == -1) discoveredGDOffset = off;
        }
        if (discoveredGDOffset == co.gameDataPtr) {
            matches++;
            r += "\n  OK: Matches hardcoded";
        } else if (discoveredGDOffset >= 0) {
            snprintf(buf, sizeof(buf), "\n  MISMATCH: Hardcoded=+0x%03X, Found=+0x%03X", co.gameDataPtr, discoveredGDOffset);
            r += buf;
            mismatches++;
        } else {
            r += "\n  NOT FOUND";
            mismatches++;
        }

        // ══════════════════════════════════════════════════════════════
        //  STEP 5: Scan for HEALTH CHAIN
        //  Follow every pointer in the struct, then follow sub-pointers,
        //  looking for arrays of floats near 100.0
        // ══════════════════════════════════════════════════════════════
        r += "\n\n  --- Health Chain Discovery ---";
        int discoveredHC1 = -1, discoveredHC2 = -1, discoveredHBase = -1;
        spdlog::info("DISCOVER: Starting health chain scan...");

        for (int off1 = 0x100; off1 < 0x400; off1 += 8) {
            uintptr_t ptr1 = 0;
            Memory::Read(charPtr + off1, ptr1);
            if (!isHeapPtr(ptr1)) continue;

            // Follow sub-pointers from ptr1
            for (int off2 = 0; off2 < 0x800; off2 += 8) {
                uintptr_t ptr2 = 0;
                if (!Memory::Read(ptr1 + off2, ptr2)) continue;
                if (!isHeapPtr(ptr2)) continue;

                // Look for 7 consecutive floats near 100.0 (full health character)
                for (int base = 0; base < 0x200; base += 4) {
                    int goodCount = 0;
                    for (int part = 0; part < 7; part++) {
                        float hp = 0;
                        if (!Memory::Read(ptr2 + base + part * 8, hp)) break;
                        // Health values for a loaded char are typically 50-200 range
                        if (hp > 10.f && hp < 300.f) goodCount++;
                    }
                    if (goodCount >= 5) {
                        // Found a candidate! Read all 7 values
                        float healthVals[7] = {};
                        for (int p = 0; p < 7; p++)
                            Memory::Read(ptr2 + base + p * 8, healthVals[p]);

                        snprintf(buf, sizeof(buf),
                                 "\n  FOUND health chain: +0x%03X -> +0x%03X -> +0x%03X"
                                 "\n    [%.0f, %.0f, %.0f, %.0f, %.0f, %.0f, %.0f]",
                                 off1, off2, base,
                                 healthVals[0], healthVals[1], healthVals[2], healthVals[3],
                                 healthVals[4], healthVals[5], healthVals[6]);
                        r += buf;
                        spdlog::info("DISCOVER: Health chain +0x{:03X} -> +0x{:03X} -> +0x{:03X} = "
                                     "[{:.0f}, {:.0f}, {:.0f}, {:.0f}, {:.0f}, {:.0f}, {:.0f}]",
                                     off1, off2, base,
                                     healthVals[0], healthVals[1], healthVals[2], healthVals[3],
                                     healthVals[4], healthVals[5], healthVals[6]);

                        if (discoveredHC1 == -1) {
                            discoveredHC1 = off1;
                            discoveredHC2 = off2;
                            discoveredHBase = base;
                        }
                    }
                }
            }
        }

        if (discoveredHC1 == co.healthChain1 && discoveredHC2 == co.healthChain2 && discoveredHBase == co.healthBase) {
            matches++;
            r += "\n  OK: Matches hardcoded chain (0x2B8->0x5F8->0x40)";
        } else if (discoveredHC1 >= 0) {
            snprintf(buf, sizeof(buf), "\n  MISMATCH: Hardcoded=(0x%X->0x%X->0x%X), Found=(0x%X->0x%X->0x%X)",
                     co.healthChain1, co.healthChain2, co.healthBase,
                     discoveredHC1, discoveredHC2, discoveredHBase);
            r += buf;
            mismatches++;
        } else {
            r += "\n  NOT FOUND (no 7-float health array discovered)";
            mismatches++;
        }

        // ══════════════════════════════════════════════════════════════
        //  STEP 6: Scan for INVENTORY pointer
        //  Valid heap pointer, with owner backpointer and item count
        // ══════════════════════════════════════════════════════════════
        r += "\n\n  --- Inventory Pointer Scan ---";
        int discoveredInvOffset = -1;
        for (int off = 0x200; off < 0x400; off += 8) {
            uintptr_t candidate = 0;
            Memory::Read(charPtr + off, candidate);
            if (!isHeapPtr(candidate)) continue;

            // Inventory should have: items ptr at +0x10, itemCount at +0x18
            // and owner backpointer at +0x28 should point back to our character
            uintptr_t ownerPtr = 0;
            Memory::Read(candidate + 0x28, ownerPtr);
            if (ownerPtr != charPtr) continue;  // Owner must be THIS character

            int itemCount = 0;
            Memory::Read(candidate + 0x18, itemCount);
            if (itemCount < 0 || itemCount > 10000) continue;

            snprintf(buf, sizeof(buf), "\n  FOUND inventory at +0x%03X -> 0x%llX (owner=self, items=%d)",
                     off, (unsigned long long)candidate, itemCount);
            r += buf;
            spdlog::info("DISCOVER: Inventory at +0x{:03X} -> 0x{:X} (items={})", off, candidate, itemCount);
            if (discoveredInvOffset == -1) discoveredInvOffset = off;
        }
        if (discoveredInvOffset == co.inventory) {
            matches++;
            r += "\n  OK: Matches hardcoded";
        } else if (discoveredInvOffset >= 0) {
            snprintf(buf, sizeof(buf), "\n  MISMATCH: Hardcoded=+0x%03X, Found=+0x%03X", co.inventory, discoveredInvOffset);
            r += buf;
            mismatches++;
        } else {
            // Try without owner check (owner offset might be different)
            r += "\n  NOT FOUND with owner check. Scanning without...";
            for (int off = 0x200; off < 0x400; off += 8) {
                uintptr_t candidate = 0;
                Memory::Read(charPtr + off, candidate);
                if (!isHeapPtr(candidate)) continue;

                // Look for reasonable item count at any nearby offset
                for (int countOff = 0x10; countOff <= 0x30; countOff += 8) {
                    int itemCount = 0;
                    Memory::Read(candidate + countOff, itemCount);
                    if (itemCount >= 0 && itemCount < 500) {
                        // Check if items pointer looks valid
                        uintptr_t itemsPtr = 0;
                        Memory::Read(candidate + countOff - 8, itemsPtr);
                        if (itemCount == 0 || isHeapPtr(itemsPtr)) {
                            snprintf(buf, sizeof(buf), "\n  CANDIDATE inv at +0x%03X -> 0x%llX (count=%d at +0x%02X)",
                                     off, (unsigned long long)candidate, itemCount, countOff);
                            r += buf;
                            spdlog::info("DISCOVER: Inventory candidate at +0x{:03X} -> 0x{:X} (count={} at +0x{:02X})",
                                         off, candidate, itemCount, countOff);
                            if (discoveredInvOffset == -1) discoveredInvOffset = off;
                            break;
                        }
                    }
                }
            }
        }

        // ══════════════════════════════════════════════════════════════
        //  STEP 7: Scan for STATS (inline block of floats in skill range)
        // ══════════════════════════════════════════════════════════════
        r += "\n\n  --- Stats Block Scan ---";
        int discoveredStatsOffset = -1;
        for (int off = 0x300; off < 0x500; off += 4) {
            // Check 10 consecutive floats in reasonable stat range (1-100)
            int statCount = 0;
            for (int s = 0; s < 10; s++) {
                float val = 0;
                Memory::Read(charPtr + off + s * 4, val);
                if (val >= 1.f && val <= 100.f) statCount++;
            }
            if (statCount >= 7) {
                // Read first 5 stats for display
                float s0 = 0, s1 = 0, s2 = 0, s3 = 0, s4 = 0;
                Memory::Read(charPtr + off, s0);
                Memory::Read(charPtr + off + 4, s1);
                Memory::Read(charPtr + off + 8, s2);
                Memory::Read(charPtr + off + 12, s3);
                Memory::Read(charPtr + off + 16, s4);
                snprintf(buf, sizeof(buf), "\n  FOUND stats block at +0x%03X [%.1f, %.1f, %.1f, %.1f, %.1f, ...]",
                         off, s0, s1, s2, s3, s4);
                r += buf;
                spdlog::info("DISCOVER: Stats block at +0x{:03X} [{:.1f}, {:.1f}, {:.1f}, {:.1f}, {:.1f}]",
                             off, s0, s1, s2, s3, s4);
                if (discoveredStatsOffset == -1) discoveredStatsOffset = off;
            }
        }
        if (discoveredStatsOffset == co.stats) {
            matches++;
            r += "\n  OK: Matches hardcoded +0x450";
        } else if (discoveredStatsOffset >= 0) {
            snprintf(buf, sizeof(buf), "\n  MISMATCH: Hardcoded=+0x%03X, Found=+0x%03X", co.stats, discoveredStatsOffset);
            r += buf;
            mismatches++;
        } else {
            r += "\n  NOT FOUND";
        }

        // ══════════════════════════════════════════════════════════════
        //  STEP 8: Scan for MONEY CHAIN
        // ══════════════════════════════════════════════════════════════
        r += "\n\n  --- Money Chain Scan ---";
        int knownMoney = accessor.GetMoney();
        snprintf(buf, sizeof(buf), "\n  Current money (via hardcoded chain): %d cats", knownMoney);
        r += buf;

        // Also dump raw pointers at the hardcoded offsets for debugging
        {
            uintptr_t mc1 = 0, mc2 = 0;
            int moneyVal = 0;
            Memory::Read(charPtr + co.moneyChain1, mc1);
            if (mc1) Memory::Read(mc1 + co.moneyChain2, mc2);
            if (mc2) Memory::Read(mc2 + co.moneyBase, moneyVal);
            snprintf(buf, sizeof(buf), "\n  Chain check: +0x%X->0x%llX, +0x%X->0x%llX, +0x%X->%d",
                     co.moneyChain1, (unsigned long long)mc1,
                     co.moneyChain2, (unsigned long long)mc2,
                     co.moneyBase, moneyVal);
            r += buf;
        }

        // ══════════════════════════════════════════════════════════════
        //  SUMMARY
        // ══════════════════════════════════════════════════════════════
        snprintf(buf, sizeof(buf), "\n\n--- SUMMARY: %d matches | %d mismatches ---", matches, mismatches);
        r += buf;

        if (mismatches > 0) {
            r += "\n  WARNING: Some offsets do not match hardcoded values!";
            r += "\n  Check log file for hex dump and details.";
            r += "\n  Update offsets in game_types.h if mismatches are confirmed.";
        } else if (matches > 0) {
            r += "\n  All verified offsets match hardcoded values.";
        }

        spdlog::info("DISCOVER: Complete — {} matches, {} mismatches", matches, mismatches);
        return r;
    });

    // /discover update — Apply discovered offsets to the live offset table
    Register("discover_apply", "Apply discovered offsets (run /discover first)", [](const CommandArgs& args) -> std::string {
        // This is a placeholder — after /discover confirms correct offsets,
        // this command would update GetOffsets() with the discovered values.
        // For now, users should manually update game_types.h defaults.
        return "Not yet implemented. Run /discover first, check log, then update game_types.h.";
    });

    // /forcespawn — Bypass all gates and force-spawn pending remote characters
    Register("forcespawn", "Force-spawn pending remote characters (bypass gates)", [](const CommandArgs&) -> std::string {
        auto& core = Core::Get();
        auto& sm = core.GetSpawnManager();

        if (!core.IsConnected()) return "Not connected to a server.";
        if (!core.IsGameLoaded()) return "Game not loaded yet.";

        size_t pending = sm.GetPendingSpawnCount();

        // If no pending spawns, check for stuck remote entities and re-queue them
        if (pending == 0) {
            auto remoteEntities = core.GetEntityRegistry().GetRemoteEntities();
            int requeued = 0;
            for (auto eid : remoteEntities) {
                if (!core.GetEntityRegistry().GetGameObject(eid)) {
                    auto infoCopy = core.GetEntityRegistry().GetInfo(eid);
                    if (infoCopy) {
                        SpawnRequest req;
                        req.netId = eid;
                        req.owner = infoCopy->ownerPlayerId;
                        req.type = infoCopy->type;
                        req.position = infoCopy->lastPosition;
                        sm.QueueSpawn(req);
                        requeued++;
                    }
                }
            }
            if (requeued > 0) {
                pending = requeued;
            } else {
                return "No pending spawns and no stuck remote entities.";
            }
        }

        if (!sm.IsReady()) {
            // Try to use ForceSpawnRemotePlayers which sets the bypass flag
            core.ForceSpawnRemotePlayers();
            return "SpawnManager not fully ready — forcing bypass for next tick.";
        }

        int spawned = 0, failed = 0;
        for (size_t i = 0; i < pending && i < 16; i++) {
            SpawnRequest req;
            if (!sm.PopNextSpawn(req)) break;

            // Map owner to mod template slot
            int templateCount = sm.GetModTemplateCount();
            int modSlot = 0;
            if (templateCount > 0 && req.owner > 0) {
                modSlot = (static_cast<int>(req.owner) - 1) % templateCount;
            }
            if (modSlot < 0 || modSlot >= templateCount) modSlot = 0;

            // Try mod template first, then createRandomChar fallback
            void* newChar = nullptr;
            if (templateCount > 0) {
                newChar = sm.SpawnCharacterDirect(&req.position, modSlot);
            }
            if (!newChar) {
                newChar = entity_hooks::CallFactoryCreateRandom(sm.GetFactory());
            }

            uintptr_t addr = reinterpret_cast<uintptr_t>(newChar);
            if (newChar && addr > 0x10000 && addr < 0x00007FFFFFFFFFFF && (addr & 0x7) == 0) {
                core.GetEntityRegistry().SetGameObject(req.netId, newChar);
                core.GetEntityRegistry().UpdatePosition(req.netId, req.position);

                // Full post-spawn setup (position, rename, AI suppress, faction fix)
                game::CharacterAccessor accessor(newChar);
                if (req.position.x != 0.f || req.position.y != 0.f || req.position.z != 0.f) {
                    accessor.WritePosition(req.position);
                }

                // Fix faction pointer to prevent crash on faction+0x250 access
                uintptr_t localFaction = entity_hooks::GetEarlyPlayerFaction();
                if (localFaction == 0) localFaction = entity_hooks::GetFallbackFaction();
                if (localFaction != 0) {
                    accessor.WriteFaction(localFaction);
                }

                core.GetPlayerController().OnRemoteCharacterSpawned(req.netId, newChar, req.owner);
                ai_hooks::MarkRemoteControlled(newChar);
                game::ScheduleDeferredAnimClassProbe(addr);

                spdlog::info("forcespawn: Spawned entity {} at 0x{:X}", req.netId, addr);
                spawned++;
            } else {
                spdlog::warn("forcespawn: All spawn methods returned null for entity {}", req.netId);
                failed++;
            }
        }

        char buf[128];
        snprintf(buf, sizeof(buf), "Force-spawned %d characters (%d failed, %d remaining)",
                 spawned, failed, (int)sm.GetPendingSpawnCount());
        return buf;
    });

    // /fulldiag — Comprehensive diagnostic dump (all systems)
    Register("fulldiag", "Full diagnostic dump of all systems", [](const CommandArgs&) -> std::string {
        auto& core = Core::Get();
        auto& sm = core.GetSpawnManager();
        auto& reg = core.GetEntityRegistry();
        auto& orch = core.GetLoadingOrch();
        char buf[256];
        std::string r;

        // ── Connection ──
        r += "=== CONNECTION ===";
        snprintf(buf, sizeof(buf), "\n  Connected: %s  |  Player ID: %u  |  Game loaded: %s",
                 core.IsConnected() ? "YES" : "NO",
                 core.GetLocalPlayerId(),
                 core.IsGameLoaded() ? "YES" : "NO");
        r += buf;

        // ── Entity Registry ──
        r += "\n=== ENTITIES ===";
        size_t total = reg.GetEntityCount();
        size_t remote = reg.GetRemoteCount();
        size_t spawned = reg.GetSpawnedRemoteCount();
        snprintf(buf, sizeof(buf), "\n  Total: %d  |  Remote: %d  |  Remote spawned: %d",
                 (int)total, (int)remote, (int)spawned);
        r += buf;

        // ── Spawn System ──
        r += "\n=== SPAWN SYSTEM ===";
        snprintf(buf, sizeof(buf), "\n  Factory: %s  |  PreCall: %s  |  Pending: %d",
                 sm.IsReady() ? "YES" : "NO",
                 sm.HasPreCallData() ? "YES" : "NO",
                 (int)sm.GetPendingSpawnCount());
        r += buf;
        snprintf(buf, sizeof(buf), "\n  Templates: %d total, %d char, %d factory",
                 (int)sm.GetTemplateCount(),
                 (int)sm.GetCharacterTemplateCount(),
                 (int)sm.GetFactoryTemplateCount());
        r += buf;
        snprintf(buf, sizeof(buf), "\n  In-place spawns: %d", entity_hooks::GetInPlaceSpawnCount());
        r += buf;

        // ── Spawn Gate ──
        r += "\n=== SPAWN GATE ===";
        const char* phaseName = "?";
        switch (orch.GetPhase()) {
            case LoadingPhase::Idle: phaseName = "Idle"; break;
            case LoadingPhase::InitialLoad: phaseName = "InitialLoad"; break;
            case LoadingPhase::ZoneTransition: phaseName = "ZoneTransition"; break;
            case LoadingPhase::SpawnLoad: phaseName = "SpawnLoad"; break;
        }
        snprintf(buf, sizeof(buf), "\n  Phase: %s  |  GameLoaded: %s  |  Burst: %s  |  CanSpawn: %s",
                 phaseName,
                 orch.IsGameLoaded() ? "Y" : "N",
                 orch.IsInBurst() ? "Y" : "N",
                 AssetFacilitator::Get().CanSpawn() ? "Y" : "N");
        r += buf;
        std::string blockReason = orch.GetSpawnBlockReason();
        snprintf(buf, sizeof(buf), "\n  Block reason: %s", blockReason.c_str());
        r += buf;

        // ── Hooks ──
        r += "\n=== HOOKS ===";
        auto diags = HookManager::Get().GetDiagnostics();
        int active = 0, movrax = 0;
        for (auto& d : diags) {
            if (d.enabled) active++;
            if (d.prologue[0] == 0x48 && d.prologue[1] == 0x8B && d.prologue[2] == 0xC4) movrax++;
        }
        snprintf(buf, sizeof(buf), "\n  Total: %d  |  Active: %d  |  MovRaxRsp: %d",
                 (int)diags.size(), active, movrax);
        r += buf;

        // Show any crashed hooks
        for (auto& d : diags) {
            if (d.crashCount > 0) {
                snprintf(buf, sizeof(buf), "\n  CRASHED: %s (crashes: %d)", d.name.c_str(), d.crashCount);
                r += buf;
            }
        }

        // ── Primary Character ──
        r += "\n=== PRIMARY CHAR ===";
        void* primary = core.GetPlayerController().GetPrimaryCharacter();
        if (primary) {
            uintptr_t p = reinterpret_cast<uintptr_t>(primary);
            float px = 0, py = 0, pz = 0;
            uintptr_t fac = 0;
            Memory::Read(p + 0x48, px); Memory::Read(p + 0x4C, py); Memory::Read(p + 0x50, pz);
            Memory::Read(p + 0x10, fac);
            snprintf(buf, sizeof(buf), "\n  Addr: 0x%llX  Pos: (%.0f, %.0f, %.0f)  Faction: 0x%llX",
                     (unsigned long long)p, px, py, pz, (unsigned long long)fac);
            r += buf;
        } else {
            r += "\n  (none)";
        }

        r += "\n--- Use /forcespawn to bypass gates ---";
        return r;
    });

    // /instance — Launch a second Kenshi instance for testing multiplayer
    Register("instance", "Launch a second Kenshi instance (bypasses Steam single-instance lock)", [](const CommandArgs&) -> std::string {
        // Find kenshi_x64.exe relative to our DLL
        char modulePath[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, modulePath, MAX_PATH);

        // Launch with -nosteam flag to bypass Steam's single-instance check
        STARTUPINFOA si = {};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi = {};

        std::string cmdLine = std::string("\"") + modulePath + "\" -nosteam";
        if (CreateProcessA(nullptr, cmdLine.data(), nullptr, nullptr, FALSE,
                           0, nullptr, nullptr, &si, &pi)) {
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            return "Launched second Kenshi instance (PID: " + std::to_string(pi.dwProcessId) + ")";
        }

        // Fallback: try without -nosteam
        cmdLine = std::string("\"") + modulePath + "\"";
        if (CreateProcessA(nullptr, cmdLine.data(), nullptr, nullptr, FALSE,
                           0, nullptr, nullptr, &si, &pi)) {
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            return "Launched second Kenshi instance (PID: " + std::to_string(pi.dwProcessId) + ")";
        }

        return "Failed to launch Kenshi: " + std::to_string(GetLastError());
    });

    // /syncstatus — Show shared-save sync status
    Register("syncstatus", "Show shared-save sync status", [](const CommandArgs&) -> std::string {
        std::string r = "=== SHARED-SAVE SYNC ===";
        r += "\nOwn character: " + shared_save_sync::GetOwnCharacterName();
        r += " — " + std::string(shared_save_sync::IsOwnCharacterFound() ? "FOUND" : "searching...");
        r += "\nOther character: " + shared_save_sync::GetOtherCharacterName();
        r += " — " + std::string(shared_save_sync::IsOtherCharacterFound() ? "FOUND" : "searching...");
        r += "\nTracked characters: " + std::to_string(char_tracker_hooks::GetTrackedCount());
        return r;
    });

    // /ready — Mark as ready in lobby (triggers game start when all ready)
    Register("ready", "Mark as ready in lobby", [](const CommandArgs&) -> std::string {
        auto& core = Core::Get();
        if (!core.IsConnected()) return "Not connected to a server.";
        if (!core.GetLobbyManager().HasFaction()) return "No faction assigned yet — wait for server.";

        PacketWriter writer;
        writer.WriteHeader(MessageType::C2S_LobbyReady);
        core.GetClient().SendReliable(writer.Data(), writer.Size());

        int slot = core.GetLobbyManager().GetPlayerSlot();
        return "Ready! You are Player " + std::to_string(slot) + ". Waiting for other players...";
    });

    // /claim — Manually scan for mod characters and claim yours
    Register("claim", "Scan for mod characters (Player 1-16) and claim yours", [](const CommandArgs&) -> std::string {
        auto& core = Core::Get();
        if (!core.IsConnected()) return "Not connected to a server.";
        if (!core.GetLobbyManager().HasFaction()) return "No faction assigned — connect first.";

        core.FindAndClaimModCharacters();

        auto localCount = core.GetEntityRegistry().GetPlayerEntities(core.GetLocalPlayerId()).size();
        if (localCount > 0) {
            return "Claimed " + std::to_string(localCount) + " character(s) as Player " +
                   std::to_string(core.GetLobbyManager().GetPlayerSlot());
        }
        return "No mod characters found — is kenshi-online.mod active?";
    });

    spdlog::info("CommandRegistry: {} built-in commands registered", GetAll().size());
}

} // namespace kmp
