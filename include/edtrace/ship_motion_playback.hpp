#pragma once

#include <array>
#include <atomic>
#include <cstdint>

namespace edtrace {
// Only the policy worker accesses voices. End callbacks touch their stable
// completion flag, so callback storage survives a stop or emitter replacement.
class ShipMotionPlayback {
 public:
  using Post = std::uint32_t (*)(std::uint32_t, std::uint64_t, void*) noexcept;
  using Stop = bool (*)(std::uint32_t, std::uint64_t, std::uint32_t) noexcept;

  static void Complete(void* cookie) noexcept {
    if (cookie) static_cast<std::atomic<bool>*>(cookie)->store(true,std::memory_order_release);
  }
  bool Service(Stop stop) noexcept {
    bool quiet = true;
    for (auto& voice : voices_) {
      if (voice.state == State::Free) continue;
      if (voice.ended.load(std::memory_order_acquire)) {
        voice.state = State::Free;
      } else if (voice.state == State::StopRequested) {
        if (stop && stop(voice.event,voice.object,voice.playing)) voice.state = State::StopAccepted;
        else quiet = false;
      }
    }
    return quiet;
  }
  bool Submit(std::uint32_t event, std::uint64_t object, Post post, Stop stop) noexcept {
    if (!Service(stop)) return false;
    if (!post || !stop) return false;
    for (auto& voice : voices_) {
      if (voice.state != State::Free) continue;
      voice.ended.store(false,std::memory_order_relaxed);
      voice.event = event;
      voice.object = object;
      voice.state = State::Playing;
      voice.playing = post(event,object,&voice.ended);
      if (!voice.playing) voice.state = State::Free;
      return voice.state != State::Free;
    }
    return false;
  }
  bool Cancel(std::uint64_t object, Stop stop) noexcept {
    for (auto& voice : voices_)
      if (voice.state == State::Playing && (!object || voice.object == object)) voice.state = State::StopRequested;
    return Service(stop);
  }

 private:
  enum class State { Free, Playing, StopRequested, StopAccepted };
  struct Voice {
    std::atomic<bool> ended{false};
    State state = State::Free;
    std::uint32_t event = 0, playing = 0;
    std::uint64_t object = 0;
  };
  std::array<Voice,256> voices_{};
};
}
