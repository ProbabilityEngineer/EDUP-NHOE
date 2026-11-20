#pragma once

#include "edtrace/onfoot_haptics.hpp"
#include "edtrace/ship_weapon_routes.hpp"

#include <array>
#include <cstdint>

namespace edtrace {
enum class ShipMotionResult { Deferred, Posted, Rejected };
using ShipMotionPost = ShipMotionResult (*)(std::uint32_t event, std::uint64_t object) noexcept;
using ShipMotionCancel = bool (*)(std::uint64_t object) noexcept;
struct ShipMotionDispatch {
  unsigned posted = 0, rejected = 0;
};

// Share the event ingress and output mixer with the on-foot effects. Ship
// ownership uses a fresh registration and an explicit player attachment.
class ShipWeaponHaptics {
 public:
  static bool Observes(OnFootInputKind kind, std::uint32_t id) noexcept;
  void SetActive(bool active) noexcept;
  void SetMotionCancel(ShipMotionCancel cancel) noexcept { cancel_motion_ = cancel; }
  void Accept(const OnFootInput& input, bool permitted, bool overlapping) noexcept;
  using PendingCall = bool (*)(std::uint64_t object) noexcept;
  ShipMotionDispatch DispatchMotion(std::uint64_t now, ShipMotionPost post, PendingCall pending) noexcept;
  HapticFrame Sample(std::uint64_t now, PendingCall pending = nullptr) noexcept;
  bool Overflowed() const noexcept { return overflowed_; }

 private:
  struct Voice {
    const ShipWeaponProfile* profile = nullptr;
    std::uint64_t cue = 0, deadline = 0;
    bool fired = false;
    std::uint32_t event = 0, playing = 0;
  };
  struct Actor {
    std::uint64_t object = 0;
    bool attached = false;
    bool owned_motion = false;
    std::uint64_t motion_quiet_until = 0;
    Voice voice{};
    struct Motion {
      std::uint32_t event = 0;
      std::uint64_t cue = 0;
      bool pending = false, native = false, paired = false;
    };
    std::array<Motion, 8> motions{};
  };
  Actor* Find(std::uint64_t object) noexcept;
  void CancelMotion(Actor& actor) noexcept;
  void Register(std::uint64_t object, Actor* existing) noexcept;
  static HapticFrame SampleVoice(Voice& voice, std::uint64_t now) noexcept;

  std::array<Actor, 1024> actors_{};
  bool active_ = false, overflowed_ = false;
  ShipMotionCancel cancel_motion_ = nullptr;
};
}  // namespace edtrace
