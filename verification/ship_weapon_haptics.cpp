#include "edtrace/ship_weapon_haptics.hpp"
#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace edtrace;
namespace {
void Check(bool condition, const char* name) {
  if (!condition) { std::fprintf(stderr, "FAIL %s\n", name); std::exit(1); }
}
bool Same(float first, float second) { return std::fabs(first - second) < .000001f; }
constexpr std::uint32_t kAttachedToPlayer = 1497268741;
constexpr std::uint32_t kBurstSmall = 1202148455;  // Burst laser, small: 85 ms pulse, no attack.

ShipWeaponHaptics Armed(std::uint64_t object) {
  ShipWeaponHaptics engine;
  engine.SetActive(true);
  engine.Accept({OnFootInputKind::Register, object, 0, 0, 0, 0, 1, 0}, true, false);
  engine.Accept({OnFootInputKind::Parameter, object, 0, kAttachedToPlayer, 0, 0, 1, 1}, true, false);
  return engine;
}
}  // namespace

int main() {
  {
    auto engine = Armed(7);
    engine.Accept({OnFootInputKind::Post, 7, 1000, kBurstSmall, 0, 1, 0, 0}, true, false);
    const auto onset = engine.Sample(1000);
    const float gain = .80f;  // Small mount.
    Check(Same(onset.low, .15f * gain) && Same(onset.high, .35f * gain), "main motors keep their tuned onset");
    Check(Same(onset.left, .35f * gain) && Same(onset.right, .35f * gain), "a shot kicks both trigger motors");
    const auto later = engine.Sample(1034);
    Check(later.left > 0 && later.left < onset.left && Same(later.left, later.right), "the trigger kick decays with the shot");
    const auto after = engine.Sample(1000 + 85 * 80 / 100);
    Check(after.left == 0 && after.right == 0 && after.low == 0, "the trigger kick ends with the pulse");
  }
  {
    auto engine = Armed(7);
    engine.Accept({OnFootInputKind::Parameter, 7, 0, kAttachedToPlayer, 0, 0, 1, 0}, true, false);
    engine.Accept({OnFootInputKind::Post, 7, 1000, kBurstSmall, 0, 1, 0, 0}, true, false);
    const auto frame = engine.Sample(1000);
    Check(frame.left == 0 && frame.right == 0, "weapons not attached to the player never kick the triggers");
  }
  std::puts("PASS ship weapon trigger kick");
  return 0;
}
