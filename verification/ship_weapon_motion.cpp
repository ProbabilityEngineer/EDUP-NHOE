#include "edtrace/ship_weapon_haptics.hpp"

#include <cstdio>
#include <cstdlib>

namespace {
using edtrace::OnFootInputKind;
using edtrace::ShipWeaponHaptics;
constexpr std::uint32_t rail_firing = 252839122;
constexpr std::uint32_t rail_native = 252839123;
constexpr std::uint32_t cannon_firing = 2249258127;
constexpr std::uint32_t player_attachment = 1497268741;
constexpr std::uint64_t hardpoint = 42;

unsigned posts = 0;
bool defer_next = false;
std::uint32_t last_event = 0;
std::uint64_t last_object = 0;

edtrace::ShipMotionResult Post(std::uint32_t event, std::uint64_t object) noexcept {
  if (defer_next) { defer_next = false; return edtrace::ShipMotionResult::Deferred; }
  ++posts;
  last_event = event;
  last_object = object;
  return event != 4102759559 ? edtrace::ShipMotionResult::Posted : edtrace::ShipMotionResult::Rejected;
}

void Check(bool good, const char* reason) {
  if (good) return;
  std::fprintf(stderr, "FAIL %s\n", reason);
  std::exit(1);
}

void Attach(ShipWeaponHaptics& engine) {
  engine.Accept({OnFootInputKind::Register,hardpoint,1,0,0,0,1},true,false);
  engine.Accept({OnFootInputKind::Parameter,hardpoint,2,player_attachment,0,0,1,1},true,false);
}

void Fire(ShipWeaponHaptics& engine, std::uint32_t event, std::uint64_t now,
          std::uint32_t playing = 23) {
  engine.Accept({OnFootInputKind::Post,hardpoint,now,event,0,playing},true,false);
}
}

int main() {
  ShipWeaponHaptics engine;
  engine.SetActive(true);
  Check(ShipWeaponHaptics::Observes(OnFootInputKind::Post,rail_firing) &&
        ShipWeaponHaptics::Observes(OnFootInputKind::Post,cannon_firing),
        "the recorded firing IDs reach the ship weapon consumer");
  Fire(engine,rail_firing,20);
  engine.DispatchMotion(50,&Post,nullptr);
  Check(posts == 0, "an unregistered hardpoint cannot post Motion");
  engine.Accept({OnFootInputKind::Register,hardpoint,50,0,0,0,1},true,false);
  Fire(engine,rail_firing,52);
  engine.DispatchMotion(75,&Post,nullptr);
  Check(posts == 0, "a registered hardpoint needs explicit player attachment");
  Attach(engine);
  Fire(engine,rail_firing,60,0);
  Fire(engine,1168673983,65);
  engine.DispatchMotion(90,&Post,nullptr);
  Check(posts == 0, "failed and charge-only posts stay quiet");

  Fire(engine,rail_firing,100);
  Fire(engine,rail_firing,107);
  const auto rail = engine.DispatchMotion(125,&Post,nullptr);
  Check(posts == 1 && rail.posted == 1 && rail.rejected == 0 &&
        last_event == 1472761002 && last_object == hardpoint &&
        !edtrace::HapticNonzero(engine.Sample(125)),
        "one local railgun discharge posts its authored cue on the same emitter");
  engine.DispatchMotion(145,&Post,nullptr);
  Check(posts == 1, "a dispatched cue is consumed exactly once");

  Fire(engine,cannon_firing,250);
  const auto cannon = engine.DispatchMotion(274,&Post,nullptr);
  Check(posts == 2 && cannon.posted == 0 && cannon.rejected == 1 &&
        last_event == 4102759559, "cannon targets its own authored cue and reports a rejected post");
  engine.DispatchMotion(300,&Post,nullptr);
  Check(posts == 2, "a rejected post is consumed without retry");

  Fire(engine,rail_firing,400);
  Fire(engine,rail_native,410);
  engine.DispatchMotion(435,&Post,nullptr);
  Check(posts == 2, "the native firing variant supersedes a pending relink");

  Fire(engine,rail_firing,500);
  engine.Accept({OnFootInputKind::Unregister,hardpoint,510,0,0,0,1},true,false);
  engine.DispatchMotion(540,&Post,nullptr);
  Check(posts == 2, "emitter removal cancels pending Motion");
  Attach(engine);
  Fire(engine,rail_firing,600);
  constexpr auto pending = +[](std::uint64_t) noexcept { return true; };
  engine.DispatchMotion(625,&Post,pending);
  engine.DispatchMotion(710,&Post,nullptr);
  Check(posts == 2, "a pending emitter can expire without late output");

  engine.SetActive(false);
  engine.SetActive(true);
  Fire(engine,rail_firing,800);
  Fire(engine,rail_native,821);
  engine.DispatchMotion(850,&Post,nullptr);
  Check(posts == 3, "a native cue outside the pairing window preserves the earlier correction on a sparse tick");
  engine.SetActive(false);
  engine.SetActive(true);
  Fire(engine,rail_firing,900);
  engine.DispatchMotion(920,&Post,nullptr);
  Fire(engine,rail_native,921);
  engine.DispatchMotion(950,&Post,nullptr);
  Check(posts == 4, "dense worker ticks apply the same pairing rule");
  engine.SetActive(false);
  engine.SetActive(true);
  Fire(engine,rail_native,1000);
  Fire(engine,rail_firing,1010);
  engine.DispatchMotion(1040,&Post,nullptr);
  Check(posts == 4, "a preceding native alias suppresses its paired correction");
  engine.SetActive(false);
  engine.SetActive(true);
  Fire(engine,rail_firing,1100);
  Fire(engine,cannon_firing,1105);
  const auto independent = engine.DispatchMotion(1130,&Post,nullptr);
  Check(posts == 6 && independent.posted == 1 && independent.rejected == 1,
        "different linked weapons on one emitter keep independent pending cues");
  engine.SetActive(false);
  engine.SetActive(true);
  Fire(engine,rail_firing,1200);
  Fire(engine,rail_firing,1221);
  engine.DispatchMotion(1250,&Post,nullptr);
  Check(posts == 8, "separate discharges survive until the same worker tick");
  engine.SetActive(false);
  engine.SetActive(true);
  Fire(engine,rail_firing,1300);
  Fire(engine,rail_native,1304);
  Fire(engine,rail_firing,1321);
  engine.DispatchMotion(1350,&Post,nullptr);
  Check(posts == 9, "one native alternate pairs with only one discharge");
  engine.SetActive(false);
  engine.SetActive(true);
  Fire(engine,rail_firing,1360);
  Fire(engine,rail_native,1364);
  Fire(engine,rail_native,1370);
  Fire(engine,rail_firing,1385);
  engine.DispatchMotion(1410,&Post,nullptr);
  Check(posts == 9,"a consumed alternate cannot claim a later discharge");
  engine.SetActive(false);
  engine.SetActive(true);
  for (unsigned i = 0; i < 12; ++i) {
    const auto now = 1400 + i * 20;
    Fire(engine,rail_firing,now);
    Fire(engine,cannon_firing,now);
    engine.DispatchMotion(now+20,&Post,nullptr);
  }
  Check(!engine.Overflowed(), "regular independent discharges cannot fault the actor registry");
  engine.SetActive(false);
  engine.SetActive(true);
  const auto before_burst = posts;
  for (unsigned i = 0; i < 5; ++i) {
    Fire(engine,rail_firing,2000+i*20);
    Fire(engine,cannon_firing,2000+i*20);
  }
  engine.DispatchMotion(2085,&Post,nullptr);
  Check(!engine.Overflowed() && posts == before_burst, "saturated history quiets its emitter without a global fault");
  engine.Accept({OnFootInputKind::Register,43,2085,0,0,0,1},true,false);
  engine.Accept({OnFootInputKind::Parameter,43,2085,player_attachment,0,0,1,1},true,false);
  engine.Accept({OnFootInputKind::Post,43,2085,rail_firing,0,88},true,false);
  engine.DispatchMotion(2110,&Post,nullptr);
  Check(posts == before_burst+1 && last_object == 43, "another emitter retains authored Motion after saturation");
  const auto before_deferral = posts;
  engine.SetActive(false);
  engine.SetActive(true);
  Fire(engine,rail_firing,2400);
  defer_next = true;
  const auto deferred = engine.DispatchMotion(2425,&Post,nullptr);
  Check(deferred.posted == 0 && deferred.rejected == 0 && posts == before_deferral,
        "pre-submit contention defers without a native attempt");
  engine.DispatchMotion(2440,&Post,nullptr);
  Check(posts == before_deferral+1, "a deferred correction retries within its original deadline");
  return 0;
}
