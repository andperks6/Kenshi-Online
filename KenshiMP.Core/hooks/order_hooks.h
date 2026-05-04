#pragma once

#include <cstdint>

namespace kmp::order_hooks {

bool Install();
void ProcessDeferredEvents();

void SetTraceEnabled(bool enabled);
bool IsTraceEnabled();
uint16_t GetLocalOrderFlags();
uint8_t GetLastRunSpeedOrder();

} // namespace kmp::order_hooks
