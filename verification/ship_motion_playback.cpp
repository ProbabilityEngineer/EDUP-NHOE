#include "edtrace/ship_motion_playback.hpp"
#include <cstdio>
#include <cstdlib>

namespace {
unsigned next_playing = 17, stops = 0;
bool accept_stop = false, reject_post = false, complete_early = false;
void* completion = nullptr;
std::uint32_t stopped_event = 0, stopped_playing = 0;
std::uint64_t stopped_object = 0;
std::uint32_t Post(std::uint32_t, std::uint64_t, void* cookie) noexcept {
  completion = cookie;
  if (complete_early) edtrace::ShipMotionPlayback::Complete(cookie);
  return reject_post ? 0 : next_playing++;
}
bool Stop(std::uint32_t event, std::uint64_t object, std::uint32_t playing) noexcept {
  ++stops;
  stopped_event = event; stopped_object = object; stopped_playing = playing;
  return accept_stop;
}
void Check(bool ok, const char* message) {
  if (!ok) { std::fprintf(stderr,"FAIL %s\n",message); std::exit(1); }
}
}

int main() {
  edtrace::ShipMotionPlayback playback;
  Check(!playback.Submit(100,42,&Post,nullptr), "submission requires a usable stop route");
  Check(playback.Submit(100,42,&Post,&Stop), "an owned native post is accepted");
  playback.Cancel(43,&Stop);
  Check(stops == 0, "another emitter cannot cancel the owned voice");
  Check(!playback.Cancel(42,&Stop), "an unaccepted stop keeps shutdown pending");
  Check(stops == 1 && stopped_event == 100 && stopped_object == 42 && stopped_playing == 17,
        "cancellation targets exactly the accepted native playing instance");
  accept_stop = true;
  Check(playback.Service(&Stop), "service reports accepted cancellation");
  Check(stops == 2, "a failed stop is retried");
  playback.Service(&Stop);
  Check(stops == 2, "an accepted stop waits for completion without resubmission");
  Check(playback.Cancel(0,&Stop) && playback.Cancel(42,&Stop) && stops == 2,
        "repeated cancellation preserves accepted stops while completion is delayed");
  edtrace::ShipMotionPlayback::Complete(completion);
  playback.Service(&Stop);
  playback.Cancel(0,&Stop);
  Check(stops == 2, "a completed voice is released");

  reject_post = true;
  Check(!playback.Submit(200,44,&Post,&Stop), "a rejected native post is reported");
  playback.Cancel(0,&Stop);
  Check(stops == 2, "failed posts never create a cancellable playing instance");
  reject_post = false;
  complete_early = true;
  for (int i = 0; i < 600; ++i)
    Check(playback.Submit(300,45,&Post,&Stop), "completion before Post returns permits continued slot reuse");
  playback.Cancel(0,&Stop);
  Check(stops == 2, "completed posts stay out of cancellation");
}
