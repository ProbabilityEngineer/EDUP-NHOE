#include "edtrace/onfoot_runtime.hpp"
#include "edtrace/xinput_output.hpp"
#include <cstdio>
#include <cstdlib>
#include <future>
#include <thread>

namespace edtrace {
void XInputOutputSetOnFootFrame(HapticFrame, bool) noexcept {}
}
namespace {
unsigned native_posts = 0;
edtrace::ShipMotionResult Post(std::uint32_t event, std::uint64_t object) noexcept {
  if (event != 1472761002u || object != 42) std::abort();
  ++native_posts;
  return edtrace::ShipMotionResult::Posted;
}
bool Cancel(std::uint64_t) noexcept { return true; }
void Check(bool ok, const char* message) {
  if (!ok) { std::fprintf(stderr,"FAIL %s\n",message); std::exit(1); }
}
void Publish(edtrace::OnFootInput input) {
  input.ship = edtrace::BeginShipWeaponInput(input.kind,input.object,input.id);
  Check(input.ship.tracked,"owned callback admission succeeds");
  edtrace::QueueOnFootInput(input);
}
}

int main() {
  using edtrace::OnFootInputKind;
  edtrace::StartOnFootFeedback(0,true,1,false,&Post,&Cancel);
  edtrace::TickOnFootFeedback(false,100,{},true);
  Publish({OnFootInputKind::Register,42,100,0,0,0,1});
  Publish({OnFootInputKind::Parameter,42,100,1497268741u,0,0,1,1});
  Publish({OnFootInputKind::Post,42,100,252839122u,0,7});

  std::promise<void> held, release;
  auto acquired = held.get_future();
  auto proceed = release.get_future();
  std::thread other([&] {
    edtrace::OnFootInput input{OnFootInputKind::Register,43,130,0,0,0,1};
    input.ship = edtrace::BeginShipWeaponInput(input.kind,input.object,0);
    Check(input.ship.tracked,"other emitter acquires callback admission");
    held.set_value();
    proceed.wait();
    edtrace::QueueOnFootInput(input);
  });
  acquired.wait();
  edtrace::TickOnFootFeedback(false,125,{},true);
  const auto busy = edtrace::GetOnFootRuntimeStats();
  Check(native_posts == 0 && busy.ship_motion_posts == 0 && busy.ship_motion_rejected == 0,
        "unrelated native-call contention defers without losing or rejecting the correction");
  release.set_value();
  other.join();
  edtrace::TickOnFootFeedback(false,150,{},true);
  Check(native_posts == 1 && edtrace::GetOnFootRuntimeStats().ship_motion_posts == 1,
        "the correction posts after contention ends within its original deadline");
  edtrace::TickOnFootFeedback(false,160,{},true);
  Check(native_posts == 1,"the deferred correction is consumed once");
  Check(edtrace::StopOnFootFeedback(),"owned output stops");
}
