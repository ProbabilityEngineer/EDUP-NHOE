#pragma once
#include "edtrace/motor_dispatch.hpp"
#include "edtrace/haptic_frame.hpp"
#include <cstddef>
#include <cstdint>

namespace edtrace {
struct XInputOutputStats {
  DWORD error;
  bool installed;
  bool ready;
  std::uint64_t calls;
  std::uint64_t wwise_calls;
  std::uint64_t runtime_calls, runtime_nonzero_calls;
  DWORD runtime_last_result;
};
// Private to the DLL/helper. The caller verifies the full executable and that
// import and dynamic lookups match their expected output addresses. CrossOver
// may use distinct interop thunks; that profile binds only the game-side slots.
DWORD XInputOutputStart(MotorBackend target, std::uintptr_t image, std::size_t image_size, bool verified,
                       MotorBackend dynamic_target = nullptr) noexcept;
bool XInputOutputRefresh() noexcept;
#ifdef ED_CROSSOVER_COMPAT
void XInputOutputRequireScopedWwiseFrames() noexcept;
#endif
// gain_enabled includes the chosen haptic route's focus. Gamepad focus is
// separate: a focused VR session must not enable background XInput fallback.
void XInputOutputSetReady(bool gain_enabled, bool controller_connected, bool gamepad_focused = true,
                         bool unique_gamepad = true) noexcept;
XInputOutputStats XInputOutputGetStats() noexcept;
MotorBackend XInputOutputNativeBackend() noexcept;
// Supplemental output is mixed per motor with the latest authored frame.
// ready=false removes only supplemental output. Global route cancellation
// uses SetReady and also discards cached authored motion.
void XInputOutputSetOnFootFrame(HapticFrame frame, bool ready) noexcept;
}
