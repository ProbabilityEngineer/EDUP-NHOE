#include "edtrace/onfoot_runtime.hpp"
#include "edtrace/xinput_output.hpp"
#include <array>
#include <cmath>
#include <atomic>
#include <mutex>

namespace edtrace {
namespace {
constexpr std::size_t kCapacity = 2048;
struct Slot { std::atomic<std::size_t> sequence{0}; OnFootInput input{}; std::uint64_t activity = 0; };
std::array<Slot,kCapacity> queue{};
std::atomic<std::size_t> write_position{0};
std::size_t read_position = 0;
std::atomic<bool> configured{false}, accepting{false}, active{false}, faulted{false};
std::atomic<bool> onfoot_configured{false};
std::atomic<bool> ship_configured{false}, ship_active{false};
// One worker advances this epoch; callbacks tag admission before reserving a
// slot. A producer delayed across a focus/enable transition cannot revive cues.
std::atomic<std::uint64_t> activity{0}, ship_activity{0};
// A stripe keeps the in-flight count in the low word and advances a
// generation on overlap. Calls from colliding emitters fail quiet too.
constexpr std::uint64_t kNextGeneration = 1ULL << 32;
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
struct ShipCallbackState {
  std::atomic<std::uint64_t> in_flight{0};
  std::atomic<std::uint32_t> queued{0};
};
std::array<ShipCallbackState, 4096> ship_calls{};
// A single recursive ordering gate avoids cross-emitter lock cycles when a
// native call reenters a hook for another object. The worker never waits for it.
std::recursive_mutex ship_gate;
std::atomic<std::uint64_t> queued{0}, dropped{0}, processed{0}, accepted{0}, rejected{0};
std::atomic<std::uint64_t> duplicate_cues{0}, expired_holds{0}, actor_overflows{0}, output_frames{0};
std::atomic<std::uint64_t> ship_motion_posts{0}, ship_motion_rejected{0};
OnFootHaptics engine;
ShipWeaponHaptics ship_engine;
float strength = 0, ship_strength = 0, ship_trigger_strength = 1;
ModRumblePeaks mod_peaks{};  // Worker thread only, like every Tick caller.
ShipMotionPost post_ship_motion = nullptr;
ShipMotionCancel cancel_ship_motion = nullptr;

ShipCallbackState& ShipCalls(std::uint64_t object) noexcept {
  return ship_calls[(object ^ (object >> 12) ^ (object >> 24) ^ (object >> 36)) % ship_calls.size()];
}
bool ShipCallPending(std::uint64_t object) noexcept {
  const auto& stripe = ShipCalls(object);
  return static_cast<std::uint32_t>(stripe.in_flight.load(std::memory_order_acquire)) != 0 ||
         stripe.queued.load(std::memory_order_acquire) != 0;
}
bool ShipMotionClosed() noexcept {
  return dropped.load(std::memory_order_acquire) || faulted.load(std::memory_order_acquire) ||
         !accepting.load(std::memory_order_acquire);
}
ShipMotionResult PostShipMotion(std::uint32_t event, std::uint64_t object) noexcept {
  try {
    if (ShipMotionClosed()) return ShipMotionResult::Rejected;
    std::unique_lock<std::recursive_mutex> lock(ship_gate, std::try_to_lock);
    if (!lock.owns_lock() || ShipCallPending(object)) return ShipMotionResult::Deferred;
    if (ShipMotionClosed()) return ShipMotionResult::Rejected;
    // Lifecycle hooks use this gate through native completion and publication.
    // The native post may reenter a hook on this thread.
    const auto result = post_ship_motion ? post_ship_motion(event,object) : ShipMotionResult::Rejected;
    if (result == ShipMotionResult::Posted && (ShipCallPending(object) || ShipMotionClosed())) {
      if (cancel_ship_motion) cancel_ship_motion(object);
      return ShipMotionResult::Rejected;
    }
    return result;
  } catch (...) {
    faulted.store(true,std::memory_order_release);
    return ShipMotionResult::Rejected;
  }
}
std::uint64_t SetActivity(std::atomic<std::uint64_t>& state, bool ready) noexcept {
  auto value = state.load(std::memory_order_relaxed);
  if (static_cast<bool>(value & 1) != ready) state.store(++value,std::memory_order_release);
  return value;
}
bool Pop(OnFootInput& input, std::uint64_t& admitted) noexcept {
  auto& slot = queue[read_position % kCapacity];
  if (slot.sequence.load(std::memory_order_acquire) != read_position + 1) return false;
  input = slot.input; admitted = slot.activity;
  slot.sequence.store(read_position + kCapacity,std::memory_order_release); ++read_position;
  return true;
}
}
void StartOnFootFeedback(float value, bool ship_enabled, float ship_value, bool onfoot_enabled,
                         ShipMotionPost post_motion, ShipMotionCancel cancel_motion) noexcept {
  if (configured.load(std::memory_order_acquire)) return;
  strength = HapticUnit(value);
  ship_strength = HapticUnit(ship_value);
  post_ship_motion = post_motion;
  cancel_ship_motion = cancel_motion;
  ship_engine.SetMotionCancel(cancel_motion);
  onfoot_configured.store(onfoot_enabled,std::memory_order_relaxed);
  ship_configured.store(ship_enabled,std::memory_order_relaxed);
  for (std::size_t i = 0; i < queue.size(); ++i) queue[i].sequence.store(i,std::memory_order_relaxed);
  configured.store(true,std::memory_order_release); accepting.store(true,std::memory_order_release);
}
ShipWeaponAdmission BeginShipWeaponInput(OnFootInputKind kind, std::uint64_t object, std::uint32_t id) noexcept {
  if (!accepting.load(std::memory_order_acquire) || !ship_configured.load(std::memory_order_relaxed) ||
      !object || object == UINT64_MAX || !ShipWeaponHaptics::Observes(kind,id)) return {};
  const auto admitted = ship_activity.load(std::memory_order_acquire);
  auto& calls = ShipCalls(object).in_flight;
  const auto state = calls.fetch_add(1,std::memory_order_acq_rel);
  const bool overlapping = static_cast<std::uint32_t>(state) != 0;
  if (overlapping) calls.fetch_add(kNextGeneration,std::memory_order_acq_rel);
  try {
    ship_gate.lock();
  } catch (...) {
    calls.fetch_sub(1,std::memory_order_acq_rel);
    faulted.store(true,std::memory_order_release);
    return {};
  }
  return {admitted,static_cast<std::uint32_t>(state >> 32),true,overlapping,true};
}
static void Enqueue(const OnFootInput& input) noexcept {
  if (!accepting.load(std::memory_order_acquire)) {
    if (faulted.load(std::memory_order_acquire)) dropped.fetch_add(1,std::memory_order_relaxed);
    return;
  }
  if (!input.ship.tracked && !onfoot_configured.load(std::memory_order_relaxed)) return;
  const auto admitted = activity.load(std::memory_order_acquire);
  auto position = write_position.load(std::memory_order_relaxed);
  for (unsigned attempt = 0; attempt < 16; ++attempt) {
    auto& slot = queue[position % kCapacity];
    const auto difference = static_cast<std::intptr_t>(slot.sequence.load(std::memory_order_acquire)) - static_cast<std::intptr_t>(position);
    if (difference == 0) {
      if (write_position.compare_exchange_weak(position,position+1,std::memory_order_relaxed)) {
        slot.input = input;
        if (input.ship.tracked) {
          // Check overlap after reserving queue order. A later overlapping
          // call either already invalidated this record, or queues behind it.
          auto& stripe = ShipCalls(input.object);
          const auto latest = stripe.in_flight.load(std::memory_order_acquire) >> 32;
          slot.input.ship.overlapping |= input.ship.generation != latest;
          stripe.queued.fetch_add(1,std::memory_order_release);
        }
        slot.activity = admitted;
        slot.sequence.store(position+1,std::memory_order_release);
        queued.fetch_add(1,std::memory_order_relaxed); return;
      }
    } else if (difference < 0) break;
    else position = write_position.load(std::memory_order_relaxed);
  }
  dropped.fetch_add(1,std::memory_order_release);
  faulted.store(true,std::memory_order_release);
  accepting.store(false,std::memory_order_release);
}
void QueueOnFootInput(const OnFootInput& input) noexcept {
  Enqueue(input);
  // Leave the stripe after publication. A later isolated call can never
  // overtake the native result or lifecycle record of an earlier one.
  if (input.ship.tracked) {
    auto& stripe = ShipCalls(input.object);
    stripe.in_flight.fetch_sub(1,std::memory_order_acq_rel);
    if (input.ship.owns_gate) ship_gate.unlock();
  }
}
bool ShipMotionAdmissionOpen(std::uint64_t object) noexcept {
  return !ShipMotionClosed() && ship_strength > 0 &&
         (ship_activity.load(std::memory_order_acquire) & 1) && !ShipCallPending(object);
}
void SetOnFootFeedbackStrength(float value) noexcept { strength = HapticUnit(value); }
void SetShipWeaponFeedbackStrength(float value) noexcept {
  const auto next = HapticUnit(value);
  if ((ship_strength > 0) != (next > 0)) {
    ship_activity.fetch_add(2,std::memory_order_acq_rel);
    if (!next) ship_engine.SetActive(false);
  }
  ship_strength = next;
}
void SetShipWeaponTriggerStrength(float value) noexcept {
  ship_trigger_strength = std::isfinite(value) && value > 0 ? (value < 3 ? value : 3) : 0;
}
void TickOnFootFeedback(bool ready, std::uint64_t now, HapticFrame travel, bool ship_ready) noexcept {
  if (!configured.load(std::memory_order_acquire)) {
    mod_peaks.sco = HapticMaximum(mod_peaks.sco, travel);
    XInputOutputSetOnFootFrame(travel,HapticNonzero(travel));
    return;
  }
  if (dropped.load(std::memory_order_acquire)) faulted.store(true,std::memory_order_release);
  const bool available = accepting.load(std::memory_order_acquire) && !faulted.load(std::memory_order_acquire);
  const bool track_onfoot = onfoot_configured.load(std::memory_order_relaxed);
  ready = ready && available && track_onfoot;
  ship_ready = ship_ready && available && ship_configured.load(std::memory_order_relaxed) && ship_strength > 0;
  const auto current_activity = SetActivity(activity,ready);
  const auto current_ship_activity = SetActivity(ship_activity,ship_ready);
  engine.SetActive(ready);
  ship_engine.SetActive(ship_ready);
  OnFootInput input{}; std::uint64_t admitted = 0;
  for (std::size_t consumed = 0; consumed < kCapacity && !dropped.load(std::memory_order_acquire) && Pop(input,admitted); ++consumed) {
    if (track_onfoot && input.kind != OnFootInputKind::StopPlayedEvent)
      engine.Accept(input,ready && admitted == current_activity);
    if (input.ship.tracked) {
      ship_engine.Accept(input,ship_ready && input.ship.activity == current_ship_activity,input.ship.overlapping);
      ShipCalls(input.object).queued.fetch_sub(1,std::memory_order_release);
    }
    processed.fetch_add(1,std::memory_order_relaxed);
  }
  if (dropped.load(std::memory_order_acquire)) {
    faulted.store(true,std::memory_order_release); accepting.store(false,std::memory_order_release);
    ready = ship_ready = false;
  }
  const auto policy = engine.Stats();
  accepted.store(policy.accepted); rejected.store(policy.rejected); duplicate_cues.store(policy.duplicate_cues);
  expired_holds.store(policy.expired_holds); actor_overflows.store(policy.actor_overflows);
  if (policy.actor_overflows || ship_engine.Overflowed()) {
    faulted.store(true); accepting.store(false); ready = ship_ready = false;
  }
  if (!ready) { SetActivity(activity,false); engine.SetActive(false); }
  if (!ship_ready) { SetActivity(ship_activity,false); ship_engine.SetActive(false); }
  if (ship_ready && post_ship_motion) {
    const auto dispatch = ship_engine.DispatchMotion(now,&PostShipMotion,&ShipCallPending);
    if (dispatch.posted) ship_motion_posts.fetch_add(dispatch.posted,std::memory_order_relaxed);
    if (dispatch.rejected) ship_motion_rejected.fetch_add(dispatch.rejected,std::memory_order_relaxed);
  }
  if (faulted.load(std::memory_order_acquire) || dropped.load(std::memory_order_acquire)) {
    ready = ship_ready = false;
    SetActivity(activity,false); SetActivity(ship_activity,false);
    engine.SetActive(false); ship_engine.SetActive(false);
  }
  // A ship voice with unresolved calls can have a release in flight. Other
  // emitters remain audible after the completed results have been drained.
  auto ship_frame = ship_ready ?
      HapticScale(ship_engine.Sample(now,&ShipCallPending),ship_strength) : HapticFrame{};
  ship_frame.left *= ship_trigger_strength; ship_frame.right *= ship_trigger_strength;
  const auto onfoot_frame = HapticScale(engine.Sample(now),strength);
  const auto frame = HapticMaximum(onfoot_frame,ship_frame);
  mod_peaks.onfoot = HapticMaximum(mod_peaks.onfoot, onfoot_frame);
  mod_peaks.ship = HapticMaximum(mod_peaks.ship, ship_frame);
  mod_peaks.sco = HapticMaximum(mod_peaks.sco, travel);
  if (HapticNonzero(frame)) output_frames.fetch_add(1,std::memory_order_relaxed);
  XInputOutputSetOnFootFrame(HapticMaximum(frame, travel), ready || ship_ready || HapticNonzero(travel));
  active.store(ready,std::memory_order_release);
  ship_active.store(ship_ready,std::memory_order_release);
}
ModRumblePeaks TakeModRumblePeaks() noexcept {
  const auto result = mod_peaks;
  mod_peaks = {};
  return result;
}
bool StopOnFootFeedback() noexcept {
  accepting.store(false,std::memory_order_release); active.store(false,std::memory_order_release);
  ship_active.store(false,std::memory_order_release);
  SetActivity(activity,false);
  SetActivity(ship_activity,false);
  // Called on the policy worker after sources are detached. Storage remains
  // resident so an already-entered callback can safely observe accepting=false.
  engine.SetActive(false); ship_engine.SetActive(false); XInputOutputSetOnFootFrame({},false);
  return !cancel_ship_motion || cancel_ship_motion(0);
}
OnFootRuntimeStats GetOnFootRuntimeStats() noexcept {
  return {onfoot_configured.load(),active.load(),faulted.load(),queued.load(),processed.load(),dropped.load(),
          accepted.load(),rejected.load(),duplicate_cues.load(),expired_holds.load(),actor_overflows.load(),output_frames.load(),
          ship_configured.load(),ship_active.load(),ship_motion_posts.load(),ship_motion_rejected.load()};
}
}  // namespace edtrace
