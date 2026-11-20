#pragma once
#include "edtrace/motor_dispatch.hpp"
#include <cstddef>
#include <cstdint>

namespace edtrace {
// Version-specific game-side XInputSetState bindings. This route never
// rewrites Wine's implementation, which may contain native ARM64EC code.
inline constexpr std::uintptr_t kMotorImportSlotRva = 0x4db6c60;
inline constexpr std::uintptr_t kMotorWwiseSlotRva = 0x6056a30;
DWORD OutputSlotsStart(std::uintptr_t image, std::size_t size, bool verified,
                       MotorBackend imported, MotorBackend dynamic, MotorBackend wrapper) noexcept;
bool OutputSlotsRefresh() noexcept;
bool OutputSlotsReady() noexcept;
DWORD OutputSlotsError() noexcept;
DWORD OutputSlotsStop() noexcept;
}
