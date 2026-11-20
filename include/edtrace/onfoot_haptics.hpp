#pragma once
#include "edtrace/haptic_frame.hpp"
#include <array>
#include <cstdint>

namespace edtrace {
enum class OnFootInputKind : std::uint32_t { Post, Parameter, Switch, Register, Unregister, StopPlayedEvent };
struct ShipWeaponAdmission {
  std::uint64_t activity = 0;
  std::uint32_t generation = 0;
  bool tracked = false, overlapping = false, owns_gate = false;
};
struct OnFootInput {
  OnFootInputKind kind = OnFootInputKind::Post;
  std::uint64_t object = 0, milliseconds = 0;
  std::uint32_t id = 0, state = 0, playing = 0;
  std::int32_t result = 0;
  float value = 0;
  ShipWeaponAdmission ship{};
};
struct OnFootHapticsStats {
  std::uint64_t accepted = 0, rejected = 0, duplicate_cues = 0, expired_holds = 0, actor_overflows = 0;
};

// Pure event/clock policy. Call from one worker; callbacks enqueue records.
// Marker requests never establish ownership from an authored default.
class OnFootHaptics {
 public:
  void SetActive(bool active) noexcept;
  void Accept(const OnFootInput& input, bool effects_permitted = true) noexcept;
  HapticFrame Sample(std::uint64_t milliseconds) noexcept;
  void Reset() noexcept;
  OnFootHapticsStats Stats() const noexcept { return stats_; }
 private:
  struct Actor {
    std::uint64_t object = 0;
    bool registered = false, attached = false, inside = false, first = false, vehicle_view = false;
    bool ammo_known = false, fired = false, speed_known = false;
    float ammo = 0, speed = 0;
    std::uint64_t last_start = 0;
    bool started = false;
  };
  struct Pulse { std::uint64_t object = 0, start = 0, end = 0; HapticFrame peak{}; std::uint64_t attack = 0, plateau = 0; };
  struct Held { std::uint64_t object = 0, deadline = 0; HapticFrame value{}; };
  struct Jetpack {
    std::uint64_t object = 0, start = 0, deadline = 0, cutoff = 0;
    bool stopping = false;
    HapticFrame cutoff_value{};
  };
  Actor* Find(std::uint64_t object) noexcept;
  void ClearOwner(std::uint64_t object) noexcept;
  void PulseFor(std::uint64_t object, std::uint64_t now, std::uint64_t duration, HapticFrame peak,
                std::uint64_t attack = 0, std::uint64_t plateau = 0) noexcept;
  void StopHold(std::uint64_t object) noexcept;
  bool Cue(std::uint64_t now, unsigned group) noexcept;
  static HapticFrame JetpackValue(const Jetpack& jetpack, std::uint64_t now) noexcept;
  void StartJetpack(std::uint64_t object, std::uint64_t now) noexcept;
  void StopJetpack(std::uint64_t object, std::uint64_t now) noexcept;
  std::array<Actor, 1024> actors_{};
  std::array<Pulse, 64> pulses_{};
  std::array<Held, 8> held_{};
  std::array<Jetpack, 8> jetpacks_{};
  std::array<std::uint64_t, 8> last_cue_{};
  std::array<bool, 8> cue_seen_{};
  bool active_ = false;
  OnFootHapticsStats stats_{};
};
}  // namespace edtrace
