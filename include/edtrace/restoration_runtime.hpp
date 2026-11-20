#pragma once
#include <windows.h>
#include <array>
#include <cstdint>

namespace edtrace {
struct RestorationStatus {
  DWORD size = sizeof(RestorationStatus), version = 7;
  DWORD start_error = ERROR_NOT_READY, profile_match = 0, output_hook = 0;
  DWORD headroom = 0, gain_enabled = 0, routing_ready = 0, stopped = 0;
  std::uint64_t motor_calls = 0, wwise_calls = 0, suppressed_engine_zeros = 0;
  DWORD gameinput_error = ERROR_NOT_READY, gameinput_initialized = 0, gameinput_selected = 0, gameinput_matches = 0;
  std::uint64_t gameinput_submissions = 0, gameinput_stops = 0;
  std::uint64_t gameinput_runtime_calls = 0, gameinput_runtime_nonzero_calls = 0;
  DWORD gameinput_runtime_last_result = ERROR_NOT_READY;
  DWORD trigger_hook_error = ERROR_NOT_READY, trigger_hook_ready = 0, trigger_contexts = 0;
  std::uint64_t trigger_negotiated = 0, trigger_frames = 0, trigger_late_frames = 0;
  std::uint64_t trigger_consume_two = 0, trigger_consume_four = 0;
  std::uint64_t trigger_nonzero_frames = 0;
  std::uint32_t trigger_peak_left_milli = 0, trigger_peak_right_milli = 0;
  std::uint32_t trigger_consume_config = 0;
  std::array<std::uint32_t, 4> trigger_sink_peak_milli{}, trigger_buffer_peak_milli{};
  std::array<std::uint64_t, 4> trigger_sink_nonzero{}, trigger_buffer_nonzero{};
  DWORD gameinput_supported_motors = 0;
  std::uint64_t gameinput_trigger_submissions = 0;
  DWORD trigger_trace_error = ERROR_NOT_READY;
  DWORD gameinput_compat_error = ERROR_NOT_READY, gameinput_compat_ready = 0;
  DWORD gameinput_compat_waiting_zero = 0, gameinput_compat_pending_stop = 0;
  DWORD gameinput_hid_error = ERROR_NOT_READY, gameinput_hid_write_error = ERROR_NOT_READY;
  std::uint64_t gameinput_hid_writes = 0, gameinput_hid_failures = 0;
  std::uint64_t gameinput_compat_triggers = 0, gameinput_compat_refreshes = 0;
  std::uint64_t gameinput_compat_native_fences = 0, gameinput_compat_suppressed_native = 0;
  DWORD gameinput_compat_vendor = 0, gameinput_compat_product = 0;
  DWORD gameinput_compat_matches = 0, gameinput_compat_gamepads = 0, gameinput_compat_steam_alias = 0;
  DWORD onfoot_trace_error = 0, onfoot_trace_log_error = 0, onfoot_trace_enabled = 0, onfoot_trace_open = 0, onfoot_trace_hooked = 0;
  DWORD onfoot_trace_hook_mask = 0;
  DWORD onfoot_trace_time_limit_reached = 0, trigger_trace_time_limit_reached = 0;
  std::uint64_t onfoot_trace_posts = 0, onfoot_trace_written = 0, onfoot_trace_drops = 0;
  std::uint64_t onfoot_trace_rtpc = 0, onfoot_trace_switch = 0, onfoot_trace_register = 0, onfoot_trace_unregister = 0;
  DWORD onfoot_configured = 0, onfoot_active = 0, onfoot_faulted = 0;
  std::uint64_t onfoot_queued = 0, onfoot_processed = 0, onfoot_dropped = 0;
  std::uint64_t onfoot_effects = 0, onfoot_rejected = 0, onfoot_duplicates = 0;
  std::uint64_t onfoot_expired_holds = 0, onfoot_actor_overflows = 0, onfoot_output_frames = 0;
};
// Called from a forwarded export, never DllMain. Queues all expensive work.
void RequestRestorationStart(HMODULE self) noexcept;
DWORD RestorationStop(DWORD timeout_ms) noexcept;
RestorationStatus RestorationGetStatus() noexcept;
}
