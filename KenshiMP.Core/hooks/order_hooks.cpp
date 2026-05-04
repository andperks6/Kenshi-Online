#include "order_hooks.h"
#include "../core.h"
#include "kmp/hook_manager.h"
#include <Windows.h>
#include <array>
#include <atomic>
#include <cstdint>
#include "kmp/messages.h"
#include <spdlog/spdlog.h>

namespace kmp::order_hooks {

namespace {

using OrderDispatchFn = void(__fastcall*)(void* orderManager, int orderId);

static constexpr uintptr_t RVA_ORDER_DISPATCH = 0x007F3880;
static constexpr size_t kQueueSize = 128;

struct DeferredOrderEvent {
    uint64_t tick = 0;
    uintptr_t orderManager = 0;
    int orderId = 0;
};

std::array<DeferredOrderEvent, kQueueSize> s_queue{};
std::atomic<uint32_t> s_writeIndex{0};
std::atomic<uint32_t> s_readIndex{0};
std::atomic<uint32_t> s_dropCount{0};
std::atomic<bool> s_traceEnabled{false};
std::atomic<uint16_t> s_localOrderFlags{0};
std::atomic<uint8_t> s_lastRunSpeedOrder{0};

OrderDispatchFn s_origOrderDispatch = nullptr;

const char* OrderName(int orderId) {
    switch (orderId) {
    case 0: return "run_speed_0";
    case 1: return "run_speed_1";
    case 2: return "run_speed_2";
    case 3: return "sneak_on";
    case 4: return "sneak_off";
    case 0x0B: return "block";
    case 0x0C: return "hold";
    case 0x0D: return "passive";
    case 0x0E: return "taunt";
    case 0x0F: return "jobs";
    case 0x10: return "run_speed_16";
    case 0x11: return "ranged";
    default: return "unknown";
    }
}

bool PushEvent(const DeferredOrderEvent& evt) {
    uint32_t write = s_writeIndex.load(std::memory_order_relaxed);
    uint32_t next = (write + 1) % static_cast<uint32_t>(kQueueSize);
    uint32_t read = s_readIndex.load(std::memory_order_acquire);
    if (next == read) {
        s_dropCount.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    s_queue[write] = evt;
    s_writeIndex.store(next, std::memory_order_release);
    return true;
}

bool PopEvent(DeferredOrderEvent& evt) {
    uint32_t read = s_readIndex.load(std::memory_order_relaxed);
    uint32_t write = s_writeIndex.load(std::memory_order_acquire);
    if (read == write) return false;

    evt = s_queue[read];
    s_readIndex.store((read + 1) % static_cast<uint32_t>(kQueueSize), std::memory_order_release);
    return true;
}

void ToggleFlag(uint16_t flag) {
    uint16_t oldFlags = s_localOrderFlags.load(std::memory_order_relaxed);
    uint16_t newFlags;
    do {
        newFlags = oldFlags ^ flag;
    } while (!s_localOrderFlags.compare_exchange_weak(
        oldFlags, newFlags, std::memory_order_relaxed, std::memory_order_relaxed));
}

void UpdateLocalOrderState(int orderId) {
    switch (orderId) {
    case 0:
    case 1:
    case 2:
    case 0x10:
        s_lastRunSpeedOrder.store(static_cast<uint8_t>(orderId), std::memory_order_relaxed);
        s_localOrderFlags.fetch_and(static_cast<uint16_t>(~(CPF_RunSpeed0 | CPF_RunSpeed1 | CPF_RunSpeed2 | CPF_RunSpeed16)),
                                    std::memory_order_relaxed);
        switch (orderId) {
        case 0:
            s_localOrderFlags.fetch_or(CPF_RunSpeed0, std::memory_order_relaxed);
            break;
        case 1:
            s_localOrderFlags.fetch_or(CPF_RunSpeed1, std::memory_order_relaxed);
            break;
        case 2:
            s_localOrderFlags.fetch_or(CPF_RunSpeed2, std::memory_order_relaxed);
            break;
        case 0x10:
            s_localOrderFlags.fetch_or(CPF_RunSpeed16, std::memory_order_relaxed);
            break;
        default:
            break;
        }
        break;
    case 3:
        s_localOrderFlags.fetch_or(CPF_Sneaking, std::memory_order_relaxed);
        break;
    case 4:
        s_localOrderFlags.fetch_and(static_cast<uint16_t>(~CPF_Sneaking), std::memory_order_relaxed);
        break;
    case 0x0B:
        ToggleFlag(CPF_Block);
        break;
    case 0x0C:
        ToggleFlag(CPF_Hold);
        break;
    case 0x0D:
        ToggleFlag(CPF_Passive);
        break;
    case 0x0E:
        ToggleFlag(CPF_Taunt);
        break;
    case 0x0F:
        ToggleFlag(CPF_Jobs);
        break;
    case 0x11:
        ToggleFlag(CPF_Ranged);
        break;
    default:
        break;
    }
}

void __fastcall Hook_OrderDispatch(void* orderManager, int orderId) {
    s_origOrderDispatch(orderManager, orderId);
    UpdateLocalOrderState(orderId);

    if (!s_traceEnabled.load(std::memory_order_relaxed)) {
        return;
    }

    DeferredOrderEvent evt;
    evt.tick = GetTickCount64();
    evt.orderManager = reinterpret_cast<uintptr_t>(orderManager);
    evt.orderId = orderId;
    PushEvent(evt);
}

} // namespace

void SetTraceEnabled(bool enabled) {
    s_traceEnabled.store(enabled, std::memory_order_relaxed);
    spdlog::info("order_hooks: trace {}", enabled ? "enabled" : "disabled");
}

bool IsTraceEnabled() {
    return s_traceEnabled.load(std::memory_order_relaxed);
}

uint16_t GetLocalOrderFlags() {
    return s_localOrderFlags.load(std::memory_order_relaxed);
}

uint8_t GetLastRunSpeedOrder() {
    return s_lastRunSpeedOrder.load(std::memory_order_relaxed);
}

void ProcessDeferredEvents() {
    int dropped = s_dropCount.exchange(0, std::memory_order_relaxed);
    if (dropped > 0) {
        spdlog::warn("order_hooks: {} order events dropped", dropped);
    }

    DeferredOrderEvent evt;
    int processed = 0;
    while (PopEvent(evt) && processed < 64) {
        processed++;
        spdlog::info("order_trace: orderId={} name={} flags=0x{:04X} runOrder={} manager=0x{:X} tick={}",
                     evt.orderId, OrderName(evt.orderId), GetLocalOrderFlags(),
                     GetLastRunSpeedOrder(), evt.orderManager, evt.tick);
    }
}

bool Install() {
    uintptr_t base = Core::Get().GetScanner().GetBase();
    if (!base) {
        spdlog::warn("order_hooks: scanner base unavailable");
        return false;
    }

    bool ok = HookManager::Get().InstallAt("OrderDispatch",
                                           base + RVA_ORDER_DISPATCH,
                                           &Hook_OrderDispatch,
                                           &s_origOrderDispatch);
    spdlog::info("order_hooks: install {} at 0x{:X}", ok ? "ok" : "failed", base + RVA_ORDER_DISPATCH);
    return ok;
}

} // namespace kmp::order_hooks
