#include "edtrace/ship_weapon_haptics.hpp"

#include <algorithm>
#include <limits>

namespace edtrace {
namespace {
constexpr std::uint32_t kAttachedToPlayer = 1497268741;
constexpr std::uint64_t kHeldTimeoutMs = 20000;
constexpr std::uint64_t kDuplicateMs = 20;
constexpr std::uint64_t kMotionExpiryMs = 100;

const ShipMotionLink* FindMotionLink(std::uint32_t id) noexcept {
  for (const auto& link : kShipMotionLinks)
    if (id == link.firing || id == link.native_firing) return &link;
  return nullptr;
}

enum class CueRole { Start, Stop, Shot };
struct Cue {
  std::uint32_t id = 0;
  const ShipWeaponProfile* profile = nullptr;
  CueRole role = CueRole::Start;
};
struct CueIndex {
  std::array<Cue, std::size(kShipWeaponProfiles) * 6> entries{};
  std::size_t count = 0;
  bool unique = true;

  constexpr void Add(std::uint32_t id, const ShipWeaponProfile& profile, CueRole role) noexcept {
    if (!id) return;
    auto index = count++;
    while (index && entries[index - 1].id > id) {
      entries[index] = entries[index - 1];
      --index;
    }
    if (index && entries[index - 1].id == id) unique = false;
    entries[index] = {id, &profile, role};
  }
};
constexpr CueIndex BuildCueIndex() noexcept {
  CueIndex index{};
  for (const auto& profile : kShipWeaponProfiles)
    for (const auto& cues : profile.cues) {
      index.Add(cues.start, profile, CueRole::Start);
      index.Add(cues.stop, profile, CueRole::Stop);
      index.Add(cues.shot, profile, CueRole::Shot);
    }
  return index;
}
constexpr auto kCueIndex = BuildCueIndex();
static_assert(kCueIndex.unique, "Ship weapon cue IDs must have one meaning");

const Cue* FindCue(std::uint32_t id) noexcept {
  const auto end = kCueIndex.entries.begin() + kCueIndex.count;
  const auto found = std::lower_bound(kCueIndex.entries.begin(), end, id,
                                     [](const Cue& cue, std::uint32_t needle) { return cue.id < needle; });
  return found != end && found->id == id ? found : nullptr;
}

struct Shape {
  std::uint32_t duration_ms, attack_ms;
  HapticFrame onset, sustain;
};
// Low motor conveys weight; high motor supplies the sharper firing texture.
// Each shot also kicks both trigger motors (which trigger fires a given
// weapon is unknown from audio events). These are EDUP patterns, not recovered
// Frontier curves. The macOS companion and GameInput with trigger motors play
// them on the triggers; XInput ignores them. OpenXR controllers without
// trigger actuators blend each trigger into its hand's vibration, so VR
// feedback is stronger where the trigger level exceeds the hand level.
// [ShipCombat] TriggerStrength in ed-onfoot-haptics.ini scales or disables it.
constexpr Shape kShapes[] = {
    {0, 60, {.15f,.25f,.25f,.25f}, {.09f,.15f,.12f,.12f}},  // Beam
    {76, 0, {.36f,.28f,.45f,.45f}, {}},                     // Multicannon
    {65, 0, {.10f,.32f,.30f,.30f}, {}},                     // Pulse laser
    {85, 0, {.15f,.35f,.35f,.35f}, {}},                     // Burst laser
    {135, 0, {.43f,.33f,.60f,.60f}, {}},                    // Plasma accelerator
    {70, 0, {.16f,.32f,.35f,.35f}, {}},                     // Plasma repeater
    {120, 24, {.33f,.25f,.40f,.40f}, {}},                   // Missile
    {195, 38, {.49f,.24f,.55f,.55f}, {}},                   // Torpedo
    {92, 0, {.28f,.23f,.45f,.45f}, {}},                     // Flak
    {105, 0, {.45f,.36f,.55f,.55f}, {}},                    // Fragment cannon
    {115, 18, {.23f,.15f,.30f,.30f}, {}},                   // Mine
    {0, 65, {.08f,.14f,.12f,.12f}, {.04f,.08f,.06f,.06f}},  // Mining laser
    {100, 14, {.27f,.23f,.40f,.40f}, {}},                   // Seismic charge launcher
};
constexpr std::uint32_t kSizePercent[] = {65, 80, 100, 120, 140};
const Shape& Pattern(const ShipWeaponProfile& profile) noexcept {
  return kShapes[static_cast<std::size_t>(profile.family)];
}
std::uint32_t SizePercent(const ShipWeaponProfile& profile) noexcept {
  return kSizePercent[static_cast<std::size_t>(profile.size)];
}
std::uint64_t PulseDuration(const ShipWeaponProfile& profile) noexcept {
  return Pattern(profile).duration_ms * SizePercent(profile) / 100;
}
std::uint64_t Deadline(std::uint64_t now, std::uint64_t duration) noexcept {
  const auto maximum = std::numeric_limits<std::uint64_t>::max();
  return now > maximum - duration ? maximum : now + duration;
}
HapticFrame ToMotors(HapticFrame from, HapticFrame to, float balance, float gain) noexcept {
  const auto blend = [&](float a, float b) { return HapticUnit((a + balance * (b - a)) * gain); };
  return {blend(from.low, to.low), blend(from.high, to.high), blend(from.left, to.left), blend(from.right, to.right)};
}
}  // namespace

bool ShipWeaponHaptics::Observes(OnFootInputKind kind, std::uint32_t id) noexcept {
  switch (kind) {
    case OnFootInputKind::Register:
    case OnFootInputKind::Unregister:
      return true;
    case OnFootInputKind::Parameter:
      return id == kAttachedToPlayer;
    case OnFootInputKind::Post:
      return FindCue(id) || FindMotionLink(id);
    case OnFootInputKind::StopPlayedEvent: {
      const auto* cue = FindCue(id);
      return cue && cue->role == CueRole::Start && cue->profile->family == ShipWeaponFamily::Beam;
    }
    default:
      return false;
  }
}

void ShipWeaponHaptics::SetActive(bool active) noexcept {
  if (active_ && !active) {
    if (cancel_motion_) cancel_motion_(0);
    for (auto& actor : actors_) {
      actor.voice = {};
      actor.motions = {};
      actor.owned_motion = false;
      actor.motion_quiet_until = 0;
    }
  }
  active_ = active;
}

ShipWeaponHaptics::Actor* ShipWeaponHaptics::Find(std::uint64_t object) noexcept {
  for (auto& actor : actors_)
    if (actor.object == object) return &actor;
  return nullptr;
}

void ShipWeaponHaptics::CancelMotion(Actor& actor) noexcept {
  if (actor.owned_motion && cancel_motion_) cancel_motion_(actor.object);
  actor.owned_motion = false;
  actor.motions = {};
  actor.motion_quiet_until = 0;
}
void ShipWeaponHaptics::Register(std::uint64_t object, Actor* existing) noexcept {
  if (existing) CancelMotion(*existing);
  auto* actor = existing ? existing : Find(0);
  if (actor) *actor = {object};
  else overflowed_ = true;
}

void ShipWeaponHaptics::Accept(const OnFootInput& input, bool permitted, bool overlapping) noexcept {
  if (!input.object || input.object == std::numeric_limits<std::uint64_t>::max()) return;
  auto* actor = Find(input.object);
  switch (input.kind) {
    case OnFootInputKind::Register:
      if (overlapping) {
        if (actor) {
          CancelMotion(*actor);
          *actor = {};
        }
      } else if (input.result == 1) Register(input.object, actor);
      return;
    case OnFootInputKind::Unregister:
      if (actor && (overlapping || input.result == 1)) {
        CancelMotion(*actor);
        *actor = {};
      }
      return;
    case OnFootInputKind::Parameter:
      if (actor && input.id == kAttachedToPlayer && (overlapping || input.result == 1)) {
        actor->attached = !overlapping && input.value == 1;
        if (!actor->attached) {
          CancelMotion(*actor);
          actor->voice = {};
          actor->motions = {};
        }
      }
      return;
    case OnFootInputKind::Post:
      break;
    case OnFootInputKind::StopPlayedEvent:
      if (actor && input.result == 1 && input.playing && actor->voice.event == input.id &&
          actor->voice.playing == input.playing) actor->voice = {};
      return;
    default:
      return;
  }
  if (!actor) return;
  auto& voice = actor->voice;
  if (overlapping) {
    CancelMotion(*actor);
    voice = {};
    actor->motions = {};
    return;
  }
  if (!input.playing) return;
  if (const auto* link = FindMotionLink(input.id)) {
    if (!active_ || !permitted || !actor->attached) return;
    const auto now = input.milliseconds;
    if (now < actor->motion_quiet_until) return;
    const bool native = input.id == link->native_firing;
    Actor::Motion* paired = nullptr;
    auto closest = kDuplicateMs;
    for (auto& motion : actor->motions) {
      const auto retention = motion.pending ? kMotionExpiryMs : kDuplicateMs;
      if (motion.event && now >= motion.cue && now - motion.cue >= retention) motion = {};
      if (motion.event != link->motion) continue;
      const auto distance = now >= motion.cue ? now - motion.cue : motion.cue - now;
      if (distance >= kDuplicateMs) continue;
      if (!native && !motion.native) return;  // Duplicate audio-only alias.
      if (motion.native != native && !motion.paired && distance < closest) {
        paired = &motion;
        closest = distance;
      }
    }
    if (paired) {
      paired->paired = true;
      paired->pending = false;
      if (native) return;  // This alternate belongs to exactly one audio shot.
    }
    for (auto& motion : actor->motions) {
      if (!motion.event) {
        motion = {link->motion, now, !native && !paired, native, paired != nullptr};
        return;
      }
    }
    // Saturation quiets this emitter briefly. Dropped native-pair history must
    // never let an alias inject duplicate output or fault unrelated feedback.
    actor->motions = {};
    actor->motion_quiet_until = Deadline(now,kDuplicateMs);
    return;
  }
  const auto* cue = FindCue(input.id);
  if (!cue) return;
  const auto& profile = *cue->profile;
  if (cue->role == CueRole::Stop) {
    if (voice.profile == &profile) voice = {};
    return;
  }
  if (!active_ || !permitted || !actor->attached) return;
  const auto now = input.milliseconds;
  const auto span = PulseDuration(profile);
  const bool current = voice.profile == &profile && now < voice.deadline;
  if (!span && current && now >= voice.cue) {
    // A close/distant alias can change the playing instance while the same
    // beam holds. Update cancellation identity without replaying the onset.
    voice.event = input.id;
    voice.playing = input.playing;
    return;
  }
  if (current && (now < voice.cue || (voice.fired && now - voice.cue < kDuplicateMs))) return;
  if (profile.family == ShipWeaponFamily::Multicannon) {
    if (cue->role == CueRole::Start) {
      if (voice.profile != &profile) voice = {&profile, now, 0, false};
    } else if (voice.profile == &profile) {
      voice = {&profile, now, Deadline(now, span), true};
    }
    return;
  }
  voice = {&profile, now, Deadline(now, span ? span : kHeldTimeoutMs), true, input.id, input.playing};
}

HapticFrame ShipWeaponHaptics::SampleVoice(Voice& voice, std::uint64_t now) noexcept {
  if (!voice.fired || now < voice.cue) return {};
  if (now >= voice.deadline) {
    if (voice.profile->family == ShipWeaponFamily::Multicannon) voice.fired = false;
    else voice = {};
    return {};
  }
  const auto& profile = *voice.profile;
  const auto& shape = Pattern(profile);
  const auto elapsed = now - voice.cue;
  const auto gain = SizePercent(profile) / 100.0f;
  const auto duration = PulseDuration(profile);
  if (!duration) {
    const auto balance = static_cast<float>(std::min<std::uint64_t>(elapsed, shape.attack_ms)) / shape.attack_ms;
    return ToMotors(shape.onset, shape.sustain, balance, gain);
  }
  if (elapsed >= duration) return {};
  const auto attack = shape.attack_ms * SizePercent(profile) / 100;
  if (elapsed < attack)
    return ToMotors({}, shape.onset, static_cast<float>(elapsed) / attack, gain);
  return ToMotors(shape.onset, {}, static_cast<float>(elapsed - attack) / (duration - attack), gain);
}

HapticFrame ShipWeaponHaptics::Sample(std::uint64_t now, PendingCall pending) noexcept {
  if (!active_) return {};
  HapticFrame frame{};
  for (auto& actor : actors_)
    if (actor.voice.profile && (!pending || !pending(actor.object)))
      frame = HapticMaximum(frame, SampleVoice(actor.voice, now));
  return frame;
}

ShipMotionDispatch ShipWeaponHaptics::DispatchMotion(std::uint64_t now, ShipMotionPost post,
                                                      PendingCall pending) noexcept {
  ShipMotionDispatch result{};
  if (!active_) return result;
  for (auto& actor : actors_) {
    for (auto& motion : actor.motions) {
      if (!motion.pending) continue;
      if (!post) { motion = {}; continue; }
      if (now < motion.cue) continue;
      const auto elapsed = now - motion.cue;
      if (elapsed >= kMotionExpiryMs) { motion = {}; continue; }
      // Pairing uses cue timestamps, independent of worker cadence.
      if (elapsed < kDuplicateMs || (pending && pending(actor.object))) continue;
      const auto submitted = post(motion.event,actor.object);
      if (submitted == ShipMotionResult::Deferred) continue;
      motion.pending = false;
      if (submitted == ShipMotionResult::Posted) {
        actor.owned_motion = true;
        ++result.posted;
      } else ++result.rejected;
    }
  }
  return result;
}
}  // namespace edtrace
