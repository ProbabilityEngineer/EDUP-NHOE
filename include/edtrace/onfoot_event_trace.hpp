#pragma once
#include <windows.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include "edtrace/onfoot_haptics.hpp"
#include "edtrace/observation_window.hpp"

namespace edtrace {
struct OnFootEventTraceStats {
  DWORD installation_error = 0, logging_error = 0, hook_mask = 0;
  bool enabled = false, started = false, stopped = false, hooked = false;
  bool controlled_fixture = false, capture_open = false, limit_reached = false, time_limit_reached = false;
  std::uint64_t post_calls = 0, written = 0;
  std::uint64_t rtpc_calls = 0, switch_calls = 0, register_calls = 0, unregister_calls = 0;
  std::uint64_t queue_drops = 0, admission_drops = 0, storage_drops = 0;
  std::uint64_t bytes_written = 0;
};

// Optional recording only. The host supplies explicit sidecar-marker opt-in
// and verification of the complete game hash. This module checks the exact
// queue helper, two selector APIs and object lifecycle entry signatures.
// No callback data, names, other text or audio is read. Static native-route
// annotations and selected strength RTPC names describe requests only.
// These diagnostic fields never trigger effects or infer local ownership. Selector/lifecycle
// records are API requests: result == 1 is AK_Success, not applied-state proof.
// Call Start/Stop outside DllMain and audio callbacks. A stopped recording may
// be manually re-armed without ending the gameplay session.
DWORD StartOnFootEventTrace(std::uintptr_t image, std::size_t image_size,
                           bool verified, bool enabled, const wchar_t* log_directory) noexcept;
bool StopOnFootEventTrace() noexcept;
OnFootEventTraceStats GetOnFootEventTraceStats() noexcept;
using OnFootInputConsumer = void (*)(const OnFootInput&) noexcept;
using ShipWeaponBegin = ShipWeaponAdmission (*)(OnFootInputKind, std::uint64_t, std::uint32_t) noexcept;
// Hooks can feed a numeric consumer without enabling any recording. The
// consumer must enqueue only: it runs on the original caller's thread.
DWORD StartOnFootEventSources(std::uintptr_t image, std::size_t image_size, bool verified,
                             OnFootInputConsumer consumer, ShipWeaponBegin ship_begin = nullptr) noexcept;
// Clear before final Stop to disable all hooks. While a consumer remains,
// StopOnFootEventTrace closes only the recorder and retains forwarding hooks.
void ClearOnFootEventConsumer() noexcept;
bool ShipMotionStopReady() noexcept;
bool StopShipMotion(std::uint32_t event, std::uint64_t object, std::uint32_t playing) noexcept;
// Re-arm recording on existing validated hooks after a clean recorder stop.
DWORD StartOnFootEventRecording(const wchar_t* log_directory) noexcept;

// Owned verification seam only; never export from a production .def file.
// Fake target signatures do not establish a verified game identity.
struct OnFootEventTraceFixtureTarget {
  std::uintptr_t image = 0;
  std::size_t image_size = 0;
  // Order: queue Post, RTPC, Switch, Register, Unregister.
  std::array<std::uintptr_t, 5> addresses{};
  std::array<std::array<unsigned char, 32>, 5> signatures{};
  std::uintptr_t event_action = 0;
  std::array<unsigned char, 32> event_action_signature{};
  std::uint64_t max_file_bytes = 32ULL * 1024 * 1024;
  std::uint64_t max_capture_ms = kMaxLiveObservationMs;
  ObservationClock observation_clock = nullptr;
};
DWORD StartOnFootEventTraceControlledFixture(const OnFootEventTraceFixtureTarget& target,
                                            const wchar_t* log_directory) noexcept;
DWORD StartOnFootEventSourcesControlledFixture(const OnFootEventTraceFixtureTarget& target,
                                               OnFootInputConsumer consumer,
                                               ShipWeaponBegin ship_begin = nullptr) noexcept;
}  // namespace edtrace
