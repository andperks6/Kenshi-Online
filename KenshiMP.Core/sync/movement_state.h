#pragma once

#include "kmp/messages.h"
#include <algorithm>
#include <cstdint>

namespace kmp::movement_state {

// Observed Steam 1.0.68 bands from Core::PollLocalPositions with weighted
// Freedom 5 characters: slow movement ~= 2.5-3.0, faster movement ~= 5.0-6.8.
// Intent flags, especially sneak, carry stance when speed alone is ambiguous.
inline uint8_t ClassifyAnimState(float speed, uint16_t flags) {
    if ((flags & CPF_Sneaking) != 0) return 4;
    if (speed < 0.5f) return 0;
    if (speed < 3.5f) return 1;
    if (speed < 6.5f) return 2;
    return 3;
}

inline uint8_t EncodeMoveSpeed(float speed) {
    float clamped = std::max(0.0f, std::min(8.0f, speed));
    return static_cast<uint8_t>(std::min(255.0f, (clamped / 8.0f) * 255.0f));
}

inline uint16_t BuildPositionFlags(float speed, uint16_t orderFlags) {
    uint16_t flags = orderFlags;
    if (speed >= 3.5f) flags |= CPF_MovingFast;
    return flags;
}

} // namespace kmp::movement_state
