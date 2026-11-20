#pragma once

#include <windows.h>
#include <cstdint>

namespace edtrace {
struct MotorHeadroomStats {
  DWORD installation_error = ERROR_NOT_READY;
  bool active = false;
  bool controlled_fixture = false;
  std::uint64_t calls = 0;
  std::uint64_t capped_channels = 0;
  std::uint64_t invalid_channels = 0;
  // Install diagnostics: stage 0 none, 1 MH_Initialize, 2 MH_CreateHook,
  // 3 MH_EnableHook; mh_status is that call's MH_STATUS. Protections are
  // VirtualQuery results for the hook target and the replacement function.
  int mh_stage = 0, mh_status = 0;
  DWORD target_protect = 0, detour_protect = 0, target_state = 0, detour_state = 0;
  std::uintptr_t target = 0, detour = 0, trampoline = 0;
  // Allocation probe, run only when MH_CreateHook reports MH_ERROR_MEMORY_ALLOC.
  // Repeats MinHook's +/-1 GB search around the target. All probe memory is
  // released immediately. Errors are GetLastError values (0 = succeeded).
  struct AllocationProbe {
    bool ran = false;
    std::uintptr_t range_min = 0, range_max = 0, granularity = 0;
    std::uintptr_t app_min = 0, app_max = 0;
    std::uint64_t free_regions = 0, reserved_regions = 0, committed_regions = 0;
    std::uint64_t free_bytes = 0, largest_free = 0;
    std::uintptr_t probe_address = 0;  // first aligned free address, nearest below the target first
    DWORD rwx_error = ERROR_NOT_READY;       // VirtualAlloc(probe, PAGE_EXECUTE_READWRITE)
    DWORD rw_error = ERROR_NOT_READY;        // VirtualAlloc(probe, PAGE_READWRITE)
    DWORD rw_to_rx_error = ERROR_NOT_READY;  // VirtualProtect of that RW block to PAGE_EXECUTE_READ
    DWORD rw_to_rwx_error = ERROR_NOT_READY; // VirtualProtect of that RW block to PAGE_EXECUTE_READWRITE
    DWORD rwx_anywhere_error = ERROR_NOT_READY; // VirtualAlloc(nullptr, PAGE_EXECUTE_READWRITE)
    std::uintptr_t rwx_anywhere_address = 0;
  } alloc;
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

 private:
  inline static thread_local const MotorSenderScope* current_ = nullptr;
  const MotorSenderScope* previous_;
  Amplitudes amplitudes_;
};

// Outside DllMain. verified requires the complete known executable SHA-256.
// Independently verifies the entire sender and all eight conversion constants.
// The hook and its cap remain resident until process exit, including after capture.
DWORD MotorHeadroomStart(std::uintptr_t imageBase, bool verified) noexcept;
MotorHeadroomStats MotorHeadroomGetStats() noexcept;

// Isolated owned code only; never exported by the proxy.
DWORD MotorHeadroomStartControlledFixture(std::uintptr_t sender) noexcept;
}  // namespace edtrace
