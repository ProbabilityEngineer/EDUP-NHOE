#include "edtrace/authored_triggers.hpp"
#include "edtrace/authored_trigger_profile.hpp"
#include "MinHook.h"

#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>

namespace edtrace { namespace {
using Init = std::uint32_t (WINAPI*)(void*, void*, void*, void*, void*);
using Frame = void (WINAPI*)(void*);
using Term = std::uint32_t (WINAPI*)(void*, void*);
using Consume = void (WINAPI*)(void*, void*, std::uint64_t);
static_assert(sizeof(void*) == 8 && std::atomic<Init>::is_always_lock_free);
static_assert(std::atomic<Frame>::is_always_lock_free && std::atomic<Term>::is_always_lock_free);
static_assert(std::atomic<std::uintptr_t>::is_always_lock_free &&
              std::atomic<std::uint64_t>::is_always_lock_free);
constexpr std::size_t capacity = 32;
std::array<std::atomic<std::uintptr_t>, capacity> contexts{};
std::atomic<Init> original_init{nullptr};
std::atomic<Frame> original_frame{nullptr};
std::atomic<Term> original_term{nullptr};
std::atomic<Consume> original_consume{nullptr};
std::atomic<bool> accepting{false}, ready{false}, fixture{false};
std::atomic<DWORD> started{0}, error{ERROR_NOT_READY}, installed_mask{0};
std::atomic<std::uint64_t> init_calls{0}, negotiated{0}, initialization_failures{0};
std::atomic<std::uint64_t> context_overflows{0}, frame_calls{0}, valid_frames{0}, unknown_frames{0};
std::atomic<std::uint64_t> two_channel_frames{0}, nonfinite_frames{0}, clamped_channels{0}, term_calls{0};
std::atomic<std::uint64_t> consume_two{0}, consume_four{0}, nonzero_frames{0};
std::atomic<std::uint32_t> peak_left{0}, peak_right{0};
// Channel-order diagnostics: every channel of the sink's four floats and of
// the four-channel buffers consumed into it, plus the buffers' full
// AkChannelConfig (count:8, type:4, mask:20).
std::array<std::atomic<std::uint32_t>, 4> sink_peak{}, buffer_peak{};
std::array<std::atomic<std::uint64_t>, 4> sink_nonzero{}, buffer_nonzero{};
std::atomic<std::uint32_t> consume_config{0};
void Peak(std::atomic<std::uint32_t>& peak, float value) noexcept {
  const auto milli = static_cast<std::uint32_t>(value * 1000.0f);
  auto seen = peak.load(std::memory_order_relaxed);
  while (milli > seen && !peak.compare_exchange_weak(seen, milli, std::memory_order_relaxed)) {}
}
constexpr std::array<unsigned char, 32> consume_signature{{
  0x48,0x89,0x5c,0x24,0x10,0x48,0x89,0x6c,0x24,0x20,0x56,0x57,0x8b,0x79,0x0c,0x33,
  0xed,0x89,0x69,0x08,0x48,0x8b,0xf2,0x8b,0x42,0x08,0x4c,0x8b,0xd9,0x0f,0xb6,0xc0
}};
std::uintptr_t expected_vtable = 0, expected_init_return = 0;
thread_local AuthoredTriggerFrame current{};

struct Sink {
  std::uintptr_t vtable;
  std::uint32_t no_data_frames, channels;
  std::uintptr_t values;
  std::uint32_t index, unused;
};
static_assert(offsetof(Sink, channels) == 0xc && offsetof(Sink, values) == 0x10 &&
              offsetof(Sink, index) == 0x18 && sizeof(Sink) == 0x20);

bool Read(std::uintptr_t address, void* destination, std::size_t size) noexcept {
  if (!address || address > std::numeric_limits<std::uintptr_t>::max() - size) return false;
  SIZE_T count = 0;
  return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(address), destination,
                           size, &count) && count == size;
}
bool SinkHeader(void* self, Sink& sink) noexcept {
  return Read(reinterpret_cast<std::uintptr_t>(self), &sink, sizeof(sink)) &&
         sink.vtable == expected_vtable && sink.index == 1;
}
bool Known(std::uintptr_t self) noexcept {
  for (auto& slot : contexts) if (slot.load(std::memory_order_acquire) == self) return true;
  return false;
}
bool Remember(std::uintptr_t self) noexcept {
  if (Known(self)) return true;
  for (auto& slot : contexts) {
    std::uintptr_t empty = 0;
    if (slot.compare_exchange_strong(empty, self, std::memory_order_release,
                                    std::memory_order_relaxed)) return true;
    if (empty == self) return true;
  }
  context_overflows.fetch_add(1, std::memory_order_relaxed);
  return false;
}
void Forget(std::uintptr_t self) noexcept {
  for (auto& slot : contexts) {
    auto value = self;
    slot.compare_exchange_strong(value, 0, std::memory_order_acq_rel,
                                std::memory_order_relaxed);
  }
}
float Bound(float value) noexcept {
  if (value < 0.0f) { clamped_channels.fetch_add(1, std::memory_order_relaxed); return 0.0f; }
  if (value > 1.0f) { clamped_channels.fetch_add(1, std::memory_order_relaxed); return 1.0f; }
  return value;
}

std::uint32_t WINAPI Initialize(void* self, void* allocator, void* context,
                                void* parameters, void* format) noexcept {
  const DWORD incoming = GetLastError();
  const auto caller = reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
  const auto original = original_init.load(std::memory_order_acquire);
  bool changed = false;
  if (accepting.load(std::memory_order_acquire)) {
    init_calls.fetch_add(1, std::memory_order_relaxed);
    Sink sink{};
    std::uint32_t config = 0;
    const auto address = reinterpret_cast<std::uintptr_t>(format);
    if (caller == expected_init_return && SinkHeader(self, sink) && allocator &&
        sink.values == 0 && address && address <= std::numeric_limits<std::uintptr_t>::max() - 8 &&
        Read(address + 4, &config, sizeof(config)) && config == 2) {
      // The native caller owns this writable AkAudioFormat. Its common Init
      // below now allocates four floats and returns the negotiated format to
      // the output graph. Never grow an already allocated two-float array.
      *reinterpret_cast<std::uint32_t*>(address + 4) = 4;
      changed = true;
    }
  }
  SetLastError(incoming);
  const auto result = original(self, allocator, context, parameters, format);
  const DWORD outgoing = GetLastError();
  if (changed && accepting.load(std::memory_order_acquire)) {
    Sink sink{};
    if (result == 1 && SinkHeader(self, sink) && sink.channels == 4 && sink.values &&
        Remember(reinterpret_cast<std::uintptr_t>(self))) {
      negotiated.fetch_add(1, std::memory_order_relaxed);
    } else {
      initialization_failures.fetch_add(1, std::memory_order_relaxed);
    }
  }
  SetLastError(outgoing);
  return result;
}

void WINAPI Update(void* self) noexcept {
  const DWORD incoming = GetLastError();
  const auto saved = current;
  current = {};
  if (accepting.load(std::memory_order_acquire)) {
    frame_calls.fetch_add(1, std::memory_order_relaxed);
    Sink sink{};
    if (SinkHeader(self, sink)) {
      if (sink.channels == 2) two_channel_frames.fetch_add(1, std::memory_order_relaxed);
      if (ready.load(std::memory_order_acquire) && sink.channels == 4 && sink.values &&
          Known(reinterpret_cast<std::uintptr_t>(self)) &&
          sink.values <= std::numeric_limits<std::uintptr_t>::max() - 16) {
        std::array<float, 4> all{};
        if (Read(sink.values, all.data(), sizeof(all))) {
          for (std::size_t c = 0; c < all.size(); ++c) {
            if (!std::isfinite(all[c])) continue;
            const float magnitude = std::fabs(all[c]);
            if (magnitude > 0) sink_nonzero[c].fetch_add(1, std::memory_order_relaxed);
            Peak(sink_peak[c], magnitude > 1 ? 1 : magnitude);
          }
        }
        std::array<float, 2> values{};
        if (Read(sink.values + 8, values.data(), sizeof(values))) {
          if (std::isfinite(values[0]) && std::isfinite(values[1])) {
            current = {true, Bound(values[0]), Bound(values[1])};
            valid_frames.fetch_add(1, std::memory_order_relaxed);
            if (current.left > 0 || current.right > 0) nonzero_frames.fetch_add(1, std::memory_order_relaxed);
            Peak(peak_left, current.left); Peak(peak_right, current.right);
          } else nonfinite_frames.fetch_add(1, std::memory_order_relaxed);
        }
      }
    }
    if (!current.valid) unknown_frames.fetch_add(1, std::memory_order_relaxed);
  }
  const auto original = original_frame.load(std::memory_order_acquire);
  SetLastError(incoming);
  original(self);
  const DWORD outgoing = GetLastError();
  current = saved;
  SetLastError(outgoing);
}

// Planar float buffer: data pointer, config, state, max frames, valid frames.
void ObserveBuffer(std::uintptr_t address) noexcept {
  struct Header { std::uintptr_t data; std::uint32_t config, state; std::uint16_t max_frames, valid_frames; } header{};
  if (!Read(address, &header, 0x14) || !header.data || header.data % alignof(float) ||
      !header.max_frames || header.valid_frames > header.max_frames) return;
  std::array<float, 1024> samples{};
  const std::size_t count = header.valid_frames < samples.size() ? header.valid_frames : samples.size();
  if (!count) return;
  for (std::size_t c = 0; c < 4; ++c) {
    const auto offset = c * header.max_frames * sizeof(float);
    if (header.data > std::numeric_limits<std::uintptr_t>::max() - offset ||
        !Read(header.data + offset, samples.data(), count * sizeof(float))) return;
    float peak = 0;
    for (std::size_t i = 0; i < count; ++i)
      if (std::isfinite(samples[i]) && std::fabs(samples[i]) > peak) peak = std::fabs(samples[i]);
    if (peak > 0) buffer_nonzero[c].fetch_add(1, std::memory_order_relaxed);
    Peak(buffer_peak[c], peak > 1 ? 1 : peak);
  }
}

void WINAPI ObserveConsume(void* self, void* buffer, std::uint64_t gain) noexcept {
  const DWORD incoming = GetLastError();
  if (accepting.load(std::memory_order_acquire)) {
    Sink sink{}; std::uint32_t config = 0;
    const auto address = reinterpret_cast<std::uintptr_t>(buffer);
    if (SinkHeader(self, sink) && address && address <= std::numeric_limits<std::uintptr_t>::max() - 12 &&
        Read(address + 8, &config, sizeof(config))) {
      if ((config & 255) == 2) consume_two.fetch_add(1, std::memory_order_relaxed);
      if ((config & 255) == 4) {
        consume_four.fetch_add(1, std::memory_order_relaxed);
        consume_config.store(config, std::memory_order_relaxed);
        ObserveBuffer(address);
      }
    }
  }
  SetLastError(incoming);
  original_consume.load(std::memory_order_acquire)(self, buffer, gain);
}

std::uint32_t WINAPI Terminate(void* self, void* allocator) noexcept {
  const DWORD incoming = GetLastError();
  const auto saved = current;
  current = {};
  if (accepting.load(std::memory_order_acquire)) {
    term_calls.fetch_add(1, std::memory_order_relaxed);
    Forget(reinterpret_cast<std::uintptr_t>(self));
  }
  const auto original = original_term.load(std::memory_order_acquire);
  SetLastError(incoming);
  const auto result = original(self, allocator);
  const DWORD outgoing = GetLastError();
  current = saved;
  SetLastError(outgoing);
  return result;
}

template <typename T> void* Bits(T function) noexcept {
  void* result = nullptr;
  static_assert(sizeof(result) == sizeof(function));
  std::memcpy(&result, &function, sizeof(result));
  return result;
}
template <typename T> T Function(void* address) noexcept {
  T result = nullptr;
  static_assert(sizeof(result) == sizeof(address));
  std::memcpy(&result, &address, sizeof(result));
  return result;
}
template <std::size_t N> bool Match(std::uintptr_t address, const std::array<unsigned char, N>& bytes) noexcept {
  std::array<unsigned char, N> actual{};
  return Read(address, actual.data(), actual.size()) && actual == bytes;
}
DWORD Fail(DWORD value) noexcept {
  accepting.store(false, std::memory_order_release);
  ready.store(false, std::memory_order_release);
  error.store(value, std::memory_order_release);
  return value;
}
DWORD Start(const AuthoredTriggerFixtureTargets& targets, bool controlled) noexcept {
  if (!targets.common_init || !targets.frame_update || !targets.term ||
      !targets.init_return || !targets.vtable) return Fail(ERROR_INVALID_PARAMETER);
  expected_vtable = targets.vtable;
  expected_init_return = targets.init_return;
  fixture.store(controlled, std::memory_order_release);
  const auto initialization = MH_Initialize();
  if (initialization != MH_OK && initialization != MH_ERROR_ALREADY_INITIALIZED)
    return Fail(ERROR_NOT_SUPPORTED);
  void* trampoline = nullptr;
  if (MH_CreateHook(reinterpret_cast<void*>(targets.common_init), Bits(&Initialize), &trampoline) != MH_OK)
    return Fail(ERROR_NOT_SUPPORTED);
  original_init.store(Function<Init>(trampoline), std::memory_order_release);
  installed_mask.fetch_or(1, std::memory_order_relaxed);
  if (MH_CreateHook(reinterpret_cast<void*>(targets.frame_update), Bits(&Update), &trampoline) != MH_OK)
    return Fail(ERROR_NOT_SUPPORTED);
  original_frame.store(Function<Frame>(trampoline), std::memory_order_release);
  installed_mask.fetch_or(2, std::memory_order_relaxed);
  if (MH_CreateHook(reinterpret_cast<void*>(targets.term), Bits(&Terminate), &trampoline) != MH_OK)
    return Fail(ERROR_NOT_SUPPORTED);
  original_term.store(Function<Term>(trampoline), std::memory_order_release);
  installed_mask.fetch_or(4, std::memory_order_relaxed);
  if (targets.consume) {
    if (MH_CreateHook(reinterpret_cast<void*>(targets.consume), Bits(&ObserveConsume), &trampoline) != MH_OK)
      return Fail(ERROR_NOT_SUPPORTED);
    original_consume.store(Function<Consume>(trampoline), std::memory_order_release);
    installed_mask.fetch_or(8, std::memory_order_relaxed);
    if (MH_QueueEnableHook(reinterpret_cast<void*>(targets.consume)) != MH_OK) return Fail(ERROR_NOT_SUPPORTED);
  }
  for (const auto address : {targets.common_init, targets.frame_update, targets.term})
    if (MH_QueueEnableHook(reinterpret_cast<void*>(address)) != MH_OK) return Fail(ERROR_NOT_SUPPORTED);
  if (MH_ApplyQueued() != MH_OK) return Fail(ERROR_NOT_SUPPORTED);
  accepting.store(true, std::memory_order_release);
  error.store(ERROR_SUCCESS, std::memory_order_release);
  return ERROR_SUCCESS;
}
bool Begin() noexcept {
  DWORD empty = 0;
  return started.compare_exchange_strong(empty, 1, std::memory_order_acq_rel);
}
}  // namespace

DWORD AuthoredTriggersStart(std::uintptr_t image, std::size_t size, bool verified) noexcept {
  if (!Begin()) return ERROR_ALREADY_INITIALIZED;
  constexpr auto last = authored_trigger_profile::vtable_rva + 7 * sizeof(std::uintptr_t);
  if (!verified || !image || size < last || image > std::numeric_limits<std::uintptr_t>::max() - size)
    return Fail(ERROR_REVISION_MISMATCH);
  if (!Match(image + authored_trigger_profile::common_init_rva, authored_trigger_profile::common_init) ||
      !Match(image + authored_trigger_profile::frame_update_rva, authored_trigger_profile::frame_update) ||
      !Match(image + authored_trigger_profile::term_rva, authored_trigger_profile::term) ||
      !Match(image + authored_trigger_profile::init_format_call_rva, authored_trigger_profile::init_format_call) ||
      !Match(image + 0x4d05ad0, consume_signature))
    return Fail(ERROR_REVISION_MISMATCH);
  std::array<std::uintptr_t, 7> vtable{};
  if (!Read(image + authored_trigger_profile::vtable_rva, vtable.data(), sizeof(vtable)))
    return Fail(ERROR_REVISION_MISMATCH);
  for (std::size_t i = 0; i < vtable.size(); ++i)
    if (vtable[i] != image + authored_trigger_profile::vtable_method_rvas[i])
      return Fail(ERROR_REVISION_MISMATCH);
  AuthoredTriggerFixtureTargets targets{};
  targets.common_init = image + authored_trigger_profile::common_init_rva;
  targets.frame_update = image + authored_trigger_profile::frame_update_rva;
  targets.term = image + authored_trigger_profile::term_rva;
  targets.init_return = image + authored_trigger_profile::init_return_rva;
  targets.vtable = image + authored_trigger_profile::vtable_rva;
  targets.consume = image + 0x4d05ad0;
  return Start(targets, false);
}
DWORD AuthoredTriggersStartControlledFixture(const AuthoredTriggerFixtureTargets& targets) noexcept {
  if (!Begin()) return ERROR_ALREADY_INITIALIZED;
  const std::array<std::uintptr_t, 3> addresses{{targets.common_init, targets.frame_update, targets.term}};
  for (std::size_t i = 0; i < addresses.size(); ++i)
    if (!Match(addresses[i], targets.signatures[i])) return Fail(ERROR_REVISION_MISMATCH);
  if (targets.consume && !Match(targets.consume, targets.consume_signature)) return Fail(ERROR_REVISION_MISMATCH);
  return Start(targets, true);
}
void AuthoredTriggersSetReady(bool value) noexcept {
  ready.store(value && accepting.load(std::memory_order_acquire), std::memory_order_release);
}
void AuthoredTriggersStop() noexcept {
  ready.store(false, std::memory_order_release);
  accepting.store(false, std::memory_order_release);
  for (auto& slot : contexts) slot.store(0, std::memory_order_release);
}
AuthoredTriggerFrame AuthoredTriggersCurrentFrame() noexcept {
  if (!ready.load(std::memory_order_acquire) || !accepting.load(std::memory_order_acquire)) return {};
  return current;
}
AuthoredTriggerStats AuthoredTriggersGetStats() noexcept {
  AuthoredTriggerStats result{};
  result.error = error.load(); result.installed = accepting.load(); result.ready = ready.load();
  result.controlled_fixture = fixture.load(); result.installed_hook_mask = installed_mask.load();
  result.init_calls = init_calls.load(); result.negotiated = negotiated.load();
  result.initialization_failures = initialization_failures.load();
  for (auto& slot : contexts) if (slot.load(std::memory_order_acquire)) ++result.registered_contexts;
  result.context_overflows = context_overflows.load(); result.frame_calls = frame_calls.load();
  result.valid_frames = valid_frames.load(); result.unknown_frames = unknown_frames.load();
  result.existing_two_channel_frames = two_channel_frames.load(); result.nonfinite_frames = nonfinite_frames.load();
  result.clamped_channels = clamped_channels.load(); result.term_calls = term_calls.load();
  result.consume_two_channel_buffers = consume_two.load(); result.consume_four_channel_buffers = consume_four.load();
  result.nonzero_frames = nonzero_frames.load();
  result.peak_left_milli = peak_left.load(); result.peak_right_milli = peak_right.load();
  result.consume_config = consume_config.load();
  for (std::size_t c = 0; c < 4; ++c) {
    result.sink_peak_milli[c] = sink_peak[c].load(); result.sink_nonzero[c] = sink_nonzero[c].load();
    result.buffer_peak_milli[c] = buffer_peak[c].load(); result.buffer_nonzero[c] = buffer_nonzero[c].load();
  }
  return result;
}
}  // namespace edtrace
