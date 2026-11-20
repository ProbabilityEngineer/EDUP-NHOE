#pragma once
#include "edtrace/onfoot_haptics.hpp"
#include "edtrace/ship_weapon_haptics.hpp"
#include <cstdint>

namespace edtrace {
struct OnFootRuntimeStats {
  bool configured = false, active = false, faulted = false;
  std::uint64_t queued = 0, processed = 0, dropped = 0, accepted = 0, rejected = 0;
  std::uint64_t duplicate_cues = 0, expired_holds = 0, actor_overflows = 0, output_frames = 0;
  bool ship_configured = false, ship_active = false;
  std::uint64_t ship_motion_posts = 0, ship_motion_rejected = 0;
};
void StartOnFootFeedback(float strength, bool ship_enabled = false, float ship_strength = 0,
                         bool onfoot_enabled = true, ShipMotionPost post_motion = nullptr,
                         ShipMotionCancel cancel_motion = nullptr) noexcept;
void SetOnFootFeedbackStrength(float strength) noexcept;
void SetShipWeaponFeedbackStrength(float strength) noexcept;
// Scales the ship weapons' trigger-motor kick: 0 disables, 1 default, up to 3.
// Scaled channels still clip at full strength.
void SetShipWeaponTriggerStrength(float strength) noexcept;
ShipWeaponAdmission BeginShipWeaponInput(OnFootInputKind kind, std::uint64_t object, std::uint32_t id) noexcept;
// Policy-worker check while its submission gate is held. Native adapters
// recheck after any stop servicing and immediately before enqueueing a cue.
bool ShipMotionAdmissionOpen(std::uint64_t object) noexcept;
void QueueOnFootInput(const OnFootInput& input) noexcept;
// One existing restoration worker owns policy and output. Time is supplied
// explicitly so event lifetime behavior has a deterministic verification seam.
void TickOnFootFeedback(bool ready, std::uint64_t milliseconds, HapticFrame travel = {},
                        bool ship_ready = false) noexcept;
bool StopOnFootFeedback() noexcept;
// Peaks of each mod effect source since the previous call. Call from the
// worker thread that calls TickOnFootFeedback.
struct ModRumblePeaks { HapticFrame onfoot, ship, sco; };
ModRumblePeaks TakeModRumblePeaks() noexcept;
OnFootRuntimeStats GetOnFootRuntimeStats() noexcept;
}  // namespace edtrace
