#pragma once
#include <windows.h>
#include <array>
#include <cstddef>
#include <cstdint>

namespace edtrace {
struct AuthoredTriggerFrame {
  bool valid = false;
  float left = 0.0f, right = 0.0f;
};
struct AuthoredTriggerStats {
  DWORD error = ERROR_NOT_READY;
  bool installed = false, ready = false, controlled_fixture = false;
  DWORD installed_hook_mask = 0;
  std::uint64_t init_calls = 0, negotiated = 0, initialization_failures = 0;
  std::uint64_t registered_contexts = 0, context_overflows = 0;
  std::uint64_t frame_calls = 0, valid_frames = 0, unknown_frames = 0;
  std::uint64_t existing_two_channel_frames = 0, nonfinite_frames = 0;
  std::uint64_t clamped_channels = 0, term_calls = 0;
  std::uint64_t consume_two_channel_buffers = 0, consume_four_channel_buffers = 0;
  // Valid frames with a non-zero trigger channel, and the peak of each channel
  // in thousandths: tells "the game authored nothing" from "output dropped it".
  std::uint64_t nonzero_frames = 0;
  std::uint32_t peak_left_milli = 0, peak_right_milli = 0;
};

// Call outside DllMain. The caller must verify the complete game-file hash.
// This module independently checks all target bytes, callsite, and vtable.
// It changes only freshly initialized native Motion output formats, never an
// already allocated two-channel accumulator, and invokes no output transport.
DWORD AuthoredTriggersStart(std::uintptr_t image, std::size_t size, bool verified) noexcept;
void AuthoredTriggersSetReady(bool ready) noexcept;
// Permanently disables format mutation/capture; installed hooks only forward.
void AuthoredTriggersStop() noexcept;
// Valid only synchronously during a verified four-channel FrameUpdate call.
// No prior frame is reused. Term/readiness loss returns an invalid zero frame.
AuthoredTriggerFrame AuthoredTriggersCurrentFrame() noexcept;
AuthoredTriggerStats AuthoredTriggersGetStats() noexcept;

// C++ owned-fixture seam only; do not export from any production .def file.
// Explicit 32-byte fixture signatures never establish a verified game profile.
struct AuthoredTriggerFixtureTargets {
  std::uintptr_t common_init = 0, frame_update = 0, term = 0;
  std::uintptr_t init_return = 0, vtable = 0;
  std::uintptr_t consume = 0;
  std::array<unsigned char, 32> consume_signature{};
  std::array<std::array<unsigned char, 32>, 3> signatures{};
};
DWORD AuthoredTriggersStartControlledFixture(const AuthoredTriggerFixtureTargets& targets) noexcept;
}  // namespace edtrace
