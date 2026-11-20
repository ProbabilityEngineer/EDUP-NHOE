#pragma once
#include "edtrace/onfoot_haptics.hpp"
#include <cstdint>

namespace edtrace {
struct OnFootRuntimeStats {
  bool configured = false, active = false, faulted = false;
  std::uint64_t queued = 0, processed = 0, dropped = 0, accepted = 0, rejected = 0;
  std::uint64_t duplicate_cues = 0, expired_holds = 0, actor_overflows = 0, output_frames = 0;
  bool ship_configured = false, ship_active = false;
};
void StartOnFootFeedback(float strength, bool ship_enabled = false, float ship_strength = 0,
                         bool onfoot_enabled = true) noexcept;
void SetOnFootFeedbackStrength(float strength) noexcept;
void SetShipWeaponFeedbackStrength(float strength) noexcept;
// Scales the ship weapons' trigger-motor kick (0 disables it). Default 1.
void SetShipWeaponTriggerStrength(float strength) noexcept;
ShipWeaponAdmission BeginShipWeaponInput(OnFootInputKind kind, std::uint64_t object, std::uint32_t id) noexcept;
void QueueOnFootInput(const OnFootInput& input) noexcept;
// One existing restoration worker owns policy and output. Time is supplied
// explicitly so event lifetime behavior has a deterministic verification seam.
void TickOnFootFeedback(bool ready, std::uint64_t milliseconds, HapticFrame travel = {},
                        bool ship_ready = false) noexcept;
void StopOnFootFeedback() noexcept;
OnFootRuntimeStats GetOnFootRuntimeStats() noexcept;
}  // namespace edtrace
