#pragma once

#include <windows.h>
#include <cstddef>
#include <cstdint>

namespace edtrace {
struct MotorHeadroomStats {
  DWORD installation_error = ERROR_NOT_READY;
  DWORD frame_slot_rollback_error = ERROR_SUCCESS;
  int minhook_status = -1;
  bool active = false;
  bool frame_slot = false;
  bool controlled_fixture = false;
  std::uint64_t calls = 0;
  std::uint64_t capped_channels = 0;
  std::uint64_t invalid_channels = 0;
};

// Carries the verified sender's capped inputs across its synchronous XInput
// call. Owned fixtures use the same scope around their synthetic sender.
class MotorSenderScope {
 public:
  struct Amplitudes { float left = 0; float right = 0; };
  MotorSenderScope(float left, float right) noexcept
      : previous_(current_), amplitudes_{left, right} { current_ = this; }
  ~MotorSenderScope() { current_ = previous_; }
  MotorSenderScope(const MotorSenderScope&) = delete;
  MotorSenderScope& operator=(const MotorSenderScope&) = delete;

  static Amplitudes Current() noexcept { return current_ ? current_->amplitudes_ : Amplitudes{}; }
  static bool Engaged() noexcept { return current_ != nullptr; }

 private:
  inline static thread_local const MotorSenderScope* current_ = nullptr;
  const MotorSenderScope* previous_;
  Amplitudes amplitudes_;
};

// Outside DllMain. verified requires the complete known executable SHA-256.
// Independently verifies the entire sender and all eight conversion constants.
// The hook and its cap remain resident until process exit, including after capture.
DWORD MotorHeadroomStart(std::uintptr_t imageBase, std::size_t size, bool verified) noexcept;
bool MotorHeadroomReady() noexcept;
#ifdef ED_CROSSOVER_COMPAT
void MotorHeadroomRetryFrameSlotRollback() noexcept;
#endif
MotorHeadroomStats MotorHeadroomGetStats() noexcept;

// Isolated owned code only; never exported by the proxy.
DWORD MotorHeadroomStartControlledFixture(std::uintptr_t sender) noexcept;
}  // namespace edtrace
