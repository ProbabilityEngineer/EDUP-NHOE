#include "edtrace/onfoot_event_trace.hpp"
#include "edtrace/native_motion_routes.hpp"
#include "MinHook.h"

#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <string>

namespace edtrace { namespace {
using U32 = std::uint32_t;
using U64 = std::uint64_t;
using Address = std::uintptr_t;
enum class AkCallbackType : std::int32_t {};
struct AkCallbackInfo;
using Callback = void (WINAPI*)(AkCallbackType, AkCallbackInfo*);
using PostFn = U32 (WINAPI*)(U32, U64, U32, Callback, void*, const void*, U32);
using RtpcFn = std::int32_t (WINAPI*)(U32, float, U64, std::int32_t, std::int32_t, bool);
using SwitchFn = std::int32_t (WINAPI*)(U32, U32, U64);
using ObjectFn = std::int32_t (WINAPI*)(U64);
using EventActionFn = std::int32_t (WINAPI*)(U32, U32, U64, std::int32_t, U32, U32);
static_assert(sizeof(void*) == 8 && std::atomic<U64>::is_always_lock_free &&
              std::atomic<std::size_t>::is_always_lock_free && std::atomic<PostFn>::is_always_lock_free &&
              std::atomic<RtpcFn>::is_always_lock_free && std::atomic<SwitchFn>::is_always_lock_free &&
              std::atomic<ObjectFn>::is_always_lock_free,
              "Recording requires Windows x64 lock-free atomics");
constexpr Address kQueueRva = 0x4a23790;
constexpr Address kEventActionRva = 0x4a21030;
constexpr std::array<unsigned char, 32> kEventActionSignature{{
    0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x6c,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x57,
    0x41,0x56,0x41,0x57,0x48,0x83,0xec,0x20,0x48,0x8b,0x35,0x99,0xd6,0x62,0x01,0x8b,
}};
constexpr std::array<Address, 5> kRvas{{kQueueRva,0x4a28ae0,0x4a29150,0x4a25f80,0x4a2a3f0}};
constexpr std::array<std::array<unsigned char, 32>, 5> kSignatures{{
  {{0x48,0x89,0x5c,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x48,0x89,0x7c,0x24,0x20,0x41,0x54,0x41,0x56,0x41,0x57,0x48,0x83,0xec,0x30,0x48,0x8b,0x1d,0x38,0xaf,0x62,0x01}},
  {{0x48,0x83,0xec,0x48,0x0f,0xb6,0x44,0x24,0x78,0x88,0x44,0x24,0x30,0x8b,0x44,0x24,0x70,0x89,0x44,0x24,0x28,0x44,0x89,0x4c,0x24,0x20,0x45,0x33,0xc9,0xe8,0x1e,0x1f}},
  {{0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x74,0x24,0x10,0x57,0x48,0x83,0xec,0x20,0x49,0x8b,0xf8,0x8b,0xf2,0x8b,0xd9,0xe8,0xc5,0x80,0x84,0xfb,0x48,0x8b,0x0d,0xfe,0x55}},
  {{0x40,0x53,0x48,0x83,0xec,0x20,0x48,0x8b,0xd9,0x48,0x83,0xf9,0xe0,0x72,0x0b,0xb8,0x1f,0x00,0x00,0x00,0x48,0x83,0xc4,0x20,0x5b,0xc3,0xe8,0x91,0xb2,0x84,0xfb,0x48}},
  {{0x40,0x53,0x48,0x83,0xec,0x20,0x48,0x8b,0xd9,0x48,0x83,0xf9,0xe0,0x72,0x0b,0xb8,0x02,0x00,0x00,0x00,0x48,0x83,0xc4,0x20,0x5b,0xc3,0xe8,0xc1,0x6d,0x84,0xfb,0x48}}
}};
constexpr U64 kClosed = U64{1} << 63;
constexpr std::size_t kCapacity = 2048;
constexpr U64 kMaxBytes = 32ULL * 1024 * 1024, kFooterReserve = 1024;
enum class Kind : U32 { Post, Rtpc, Switch, Register, Unregister };
struct Record {
  Kind kind = Kind::Post;
  U64 sequence = 0, game_object = 0;
  LONGLONG start = 0, end = 0;
  Address caller = 0;
  U32 event = 0, flags = 0, requested_playing = 0, returned_playing = 0;
  DWORD thread = 0, incoming_error = 0, outgoing_error = 0;
  U32 parameter = 0, state = 0, value_bits = 0;
  float value = 0;
  std::int32_t duration = 0, curve = 0, result = 0;
  bool bypass = false;
};
struct Slot { std::atomic<std::size_t> sequence{0}; Record record{}; };
std::array<Slot, kCapacity> queue{};
std::atomic<std::size_t> write_position{0};
std::size_t read_position = 0;
std::atomic<U64> gate{kClosed}, sequence{0}, post_calls{0}, written{0};
std::atomic<U64> rtpc_calls{0}, switch_calls{0}, register_calls{0}, unregister_calls{0};
std::atomic<U64> queue_drops{0}, admission_drops{0}, storage_drops{0}, bytes_written{0};
std::atomic<DWORD> installation_error{0}, logging_error{0}, hook_mask{0};
std::atomic<bool> enabled{false}, started{false}, stopped{false}, controlled_fixture{false}, limit_reached{false};
std::atomic<PostFn> original_post{nullptr};
std::atomic<RtpcFn> original_rtpc{nullptr};
std::atomic<SwitchFn> original_switch{nullptr};
std::atomic<ObjectFn> original_register{nullptr}, original_unregister{nullptr};
std::atomic<EventActionFn> original_event_action{nullptr};
std::atomic<OnFootInputConsumer> numeric_consumer{nullptr};
std::atomic<ShipWeaponBegin> ship_weapon_begin{nullptr};
Address image_base = 0;
std::size_t image_size = 0;
U64 file_limit = kMaxBytes, anchor_filetime = 0;
LARGE_INTEGER qpc_frequency{}, anchor_before{}, anchor_after{};
SRWLOCK lifecycle = SRWLOCK_INIT;
// The writer can reach a recording limit before the host calls Stop. Serialize
// its recording-only unhook with Stop without taking the lock held during join.
SRWLOCK hook_shutdown = SRWLOCK_INIT;
HANDLE stop_event = nullptr, worker = nullptr, file = INVALID_HANDLE_VALUE;
std::array<void*, 5> targets{};
void* event_action_target = nullptr;
constexpr DWORD kEventActionHook = 1u << 5;
std::array<std::array<unsigned char,32>,5> profile_signatures{};
ObservationWindow observation_window;
U64 MonotonicMs() noexcept { return GetTickCount64(); }

bool ReadBytes(Address address, void* data, std::size_t size) noexcept {
  if (address < 65536 || address > std::numeric_limits<Address>::max() - size) return false;
  SIZE_T count = 0;
  return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address), data, size, &count) && count == size;
}
bool Admit() noexcept {
  auto state = gate.load(std::memory_order_acquire);
  if (!(state & kClosed) && observation_window.Expired()) {
    gate.fetch_or(kClosed, std::memory_order_acq_rel); return false;
  }
  for (unsigned attempt = 0; attempt < 16; ++attempt) {
    if (state & kClosed) return false;
    if (gate.compare_exchange_weak(state, state + 1, std::memory_order_acq_rel)) return true;
  }
  admission_drops.fetch_add(1, std::memory_order_relaxed); return false;
}
void Enqueue(Record& record) noexcept {
  record.sequence = sequence.fetch_add(1, std::memory_order_relaxed) + 1;
  auto position = write_position.load(std::memory_order_relaxed);
  for (unsigned attempt = 0; attempt < 16; ++attempt) {
    auto& slot = queue[position % kCapacity];
    const auto difference = static_cast<std::intptr_t>(slot.sequence.load(std::memory_order_acquire)) - static_cast<std::intptr_t>(position);
    if (difference == 0) {
      if (write_position.compare_exchange_weak(position, position + 1, std::memory_order_relaxed)) {
        slot.record = record; slot.sequence.store(position + 1, std::memory_order_release); return;
      }
    } else if (difference < 0) break;
    else position = write_position.load(std::memory_order_relaxed);
  }
  queue_drops.fetch_add(1, std::memory_order_relaxed);
}
bool Dequeue(Record& record) noexcept {
  auto& slot = queue[read_position % kCapacity];
  if (slot.sequence.load(std::memory_order_acquire) != read_position + 1) return false;
  record = slot.record; slot.sequence.store(read_position + kCapacity, std::memory_order_release); ++read_position; return true;
}
bool Begin(Record& record, Kind kind, DWORD incoming, Address caller) noexcept {
  if (!Admit()) return false;
  LARGE_INTEGER now{}; QueryPerformanceCounter(&now); record.start = now.QuadPart;
  record.kind = kind; record.thread = GetCurrentThreadId(); record.incoming_error = incoming; record.caller = caller; return true;
}
void Finish(Record& record, DWORD outgoing) noexcept {
  LARGE_INTEGER now{}; QueryPerformanceCounter(&now); record.end = now.QuadPart; record.outgoing_error = outgoing;
  switch (record.kind) {
    case Kind::Post: post_calls.fetch_add(1, std::memory_order_relaxed); break;
    case Kind::Rtpc: rtpc_calls.fetch_add(1, std::memory_order_relaxed); break;
    case Kind::Switch: switch_calls.fetch_add(1, std::memory_order_relaxed); break;
    case Kind::Register: register_calls.fetch_add(1, std::memory_order_relaxed); break;
    case Kind::Unregister: unregister_calls.fetch_add(1, std::memory_order_relaxed); break;
  }
  Enqueue(record); gate.fetch_sub(1, std::memory_order_release);
}
struct InputDelivery {
  OnFootInputConsumer consumer = nullptr;
  ShipWeaponAdmission ship{};

  void Publish(OnFootInputKind kind, U64 object, U32 id, U32 state, U32 playing,
               std::int32_t result, float value = 0) const noexcept {
    if (consumer) consumer({kind,object,GetTickCount64(),id,state,playing,result,value,ship});
  }
};
InputDelivery BeginInput(OnFootInputKind kind, U64 object, U32 id = 0) noexcept {
  const auto consumer = numeric_consumer.load(std::memory_order_acquire);
  const auto begin = ship_weapon_begin.load(std::memory_order_relaxed);
  return {consumer, consumer && begin ? begin(kind, object, id) : ShipWeaponAdmission{}};
}
std::int32_t WINAPI ObserveEventAction(U32 event, U32 action, U64 object, std::int32_t transition,
                                      U32 curve, U32 playing) noexcept {
  const DWORD incoming = GetLastError();
  const auto kind = OnFootInputKind::StopPlayedEvent;
  // The game's hardpoint dispatcher can stop the original playing instance
  // directly through AK::SoundEngine::ExecuteActionOnEvent (Stop = 0).
  const auto delivery = action == 0 && playing ? BeginInput(kind,object,event) : InputDelivery{};
  SetLastError(incoming);
  const auto result = original_event_action.load(std::memory_order_acquire)(event,action,object,transition,curve,playing);
  const DWORD outgoing = GetLastError();
  if (delivery.ship.tracked) delivery.Publish(kind,object,event,0,playing,result);
  SetLastError(outgoing);
  return result;
}
U32 WINAPI ObservePost(U32 event, U64 object, U32 flags, Callback callback, void* cookie,
                       const void* external_sources, U32 requested_playing) noexcept {
  const DWORD incoming = GetLastError();
  const Address caller = reinterpret_cast<Address>(__builtin_return_address(0));
  Record record{};
  const bool captured = Begin(record, Kind::Post, incoming, caller);
  const auto delivery = BeginInput(OnFootInputKind::Post, object, event);
  SetLastError(incoming);
  const U32 result = original_post.load(std::memory_order_acquire)(event, object, flags, callback, cookie, external_sources, requested_playing);
  const DWORD outgoing = GetLastError();
  if (captured) {
    record.event = event; record.game_object = object; record.flags = flags;
    record.requested_playing = requested_playing; record.returned_playing = result; Finish(record,outgoing);
  }
  delivery.Publish(OnFootInputKind::Post,object,event,0,result,0);
  SetLastError(outgoing); return result;
}
std::int32_t WINAPI ObserveRtpc(U32 parameter, float value, U64 object, std::int32_t duration,
                                std::int32_t curve, bool bypass) noexcept {
  const DWORD incoming = GetLastError();
  Record record{};
  const bool captured = ObservedTraceParameterName(parameter) &&
                       Begin(record,Kind::Rtpc,incoming,reinterpret_cast<Address>(__builtin_return_address(0)));
  const bool relevant = parameter == 1497268741 || parameter == 103566125 || parameter == 3377597109 ||
                        parameter == 1459991788 || parameter == 3661999171 || parameter == 2410709172;
  const auto delivery = relevant ? BeginInput(OnFootInputKind::Parameter, object, parameter) : InputDelivery{};
  SetLastError(incoming);
  const auto result = original_rtpc.load(std::memory_order_acquire)(parameter,value,object,duration,curve,bypass);
  const DWORD outgoing = GetLastError();
  if (captured) {
    record.parameter = parameter; record.value = value; std::memcpy(&record.value_bits,&value,sizeof(value));
    record.game_object = object; record.duration = duration; record.curve = curve; record.bypass = bypass;
    record.result = result; Finish(record,outgoing);
  }
  delivery.Publish(OnFootInputKind::Parameter,object,parameter,0,0,result,value);
  SetLastError(outgoing); return result;
}
std::int32_t WINAPI ObserveSwitch(U32 group, U32 state, U64 object) noexcept {
  const DWORD incoming = GetLastError(); Record record{};
  const bool relevant = group == 1497268741 || group == 103566125 || group == 1979902012;
  const bool captured = relevant && Begin(record,Kind::Switch,incoming,
                                          reinterpret_cast<Address>(__builtin_return_address(0)));
  const auto delivery = relevant ? InputDelivery{numeric_consumer.load(std::memory_order_acquire)} : InputDelivery{};
  SetLastError(incoming); const auto result = original_switch.load(std::memory_order_acquire)(group,state,object);
  const DWORD outgoing = GetLastError();
  if (captured) { record.parameter = group; record.state = state; record.game_object = object; record.result = result; Finish(record,outgoing); }
  delivery.Publish(OnFootInputKind::Switch,object,group,state,0,result);
  SetLastError(outgoing); return result;
}
std::int32_t WINAPI ObserveRegister(U64 object) noexcept {
  const DWORD incoming = GetLastError(); Record record{};
  const bool captured = Begin(record,Kind::Register,incoming,reinterpret_cast<Address>(__builtin_return_address(0)));
  const auto delivery = BeginInput(OnFootInputKind::Register, object);
  SetLastError(incoming); const auto result = original_register.load(std::memory_order_acquire)(object);
  const DWORD outgoing = GetLastError();
  if (captured) { record.game_object = object; record.result = result; Finish(record,outgoing); }
  delivery.Publish(OnFootInputKind::Register,object,0,0,0,result);
  SetLastError(outgoing); return result;
}
std::int32_t WINAPI ObserveUnregister(U64 object) noexcept {
  const DWORD incoming = GetLastError(); Record record{};
  const bool captured = Begin(record,Kind::Unregister,incoming,reinterpret_cast<Address>(__builtin_return_address(0)));
  const auto delivery = BeginInput(OnFootInputKind::Unregister, object);
  SetLastError(incoming); const auto result = original_unregister.load(std::memory_order_acquire)(object);
  const DWORD outgoing = GetLastError();
  if (captured) { record.game_object = object; record.result = result; Finish(record,outgoing); }
  delivery.Publish(OnFootInputKind::Unregister,object,0,0,0,result);
  SetLastError(outgoing); return result;
}
std::string Json(const Record& record) {
  std::ostringstream out; out.imbue(std::locale::classic());
  const char* type = record.kind == Kind::Post ? "onfoot_queue_post" : record.kind == Kind::Rtpc ? "onfoot_rtpc_request" :
                     record.kind == Kind::Switch ? "onfoot_switch_request" : record.kind == Kind::Register ?
                     "onfoot_register_request" : "onfoot_unregister_request";
  out << "{\"type\":\"" << type << "\",\"sequence\":" << record.sequence
      << ",\"start_qpc\":" << record.start << ",\"end_qpc\":" << record.end << ",\"thread_id\":" << record.thread
      << ",\"incoming_last_error\":" << record.incoming_error << ",\"outgoing_last_error\":" << record.outgoing_error
      << ",\"game_object_id\":" << record.game_object;
  if (record.kind == Kind::Post) {
    out << ",\"event_id\":" << record.event << ",\"flags\":" << record.flags
        << ",\"requested_playing_id\":" << record.requested_playing << ",\"returned_playing_id\":" << record.returned_playing;
    out << ",\"native_motion_route\":";
    WriteNativeMotionRoute(out,record.event);
  } else {
    out << ",\"result\":" << record.result;
    if (record.kind == Kind::Rtpc) {
      out << ",\"parameter_id\":" << record.parameter << ",\"value_bits\":" << record.value_bits << ",\"value\":";
      if (std::isfinite(record.value)) out << std::setprecision(9) << record.value; else out << "null";
      if (const char* name = ObservedTraceParameterName(record.parameter)) out << ",\"parameter_name\":\"" << name << '"';
      out << ",\"value_status\":\"" << (std::isfinite(record.value) ? "finite" : "non_finite")
          << "\",\"transition_duration_ms\":" << record.duration << ",\"curve\":" << record.curve
          << ",\"bypass_interpolation\":" << (record.bypass ? "true" : "false");
    } else if (record.kind == Kind::Switch) out << ",\"group_id\":" << record.parameter << ",\"state_id\":" << record.state;
  }
  out << ",\"caller_address\":" << record.caller << ",\"caller_rva\":";
  if (record.caller >= image_base && record.caller - image_base < image_size) out << record.caller - image_base;
  else out << "null";
  out << ",\"local_player\":\"unknown\"}"; return out.str();
}
bool WriteLine(const std::string& text, bool footer = false) noexcept {
  const U64 used = bytes_written.load(std::memory_order_relaxed);
  const U64 ceiling = footer ? file_limit : file_limit - kFooterReserve;
  if (used > ceiling || text.size() + 1 > ceiling - used) {
    limit_reached.store(true, std::memory_order_release); gate.fetch_or(kClosed, std::memory_order_acq_rel); return false;
  }
  DWORD count = 0;
  if (!WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &count, nullptr) || count != text.size()) {
    const DWORD error = GetLastError(); logging_error.store(error ? error : ERROR_WRITE_FAULT);
    gate.fetch_or(kClosed, std::memory_order_acq_rel); return false;
  }
  bytes_written.fetch_add(count, std::memory_order_relaxed);
  const char newline = '\n';
  if (!WriteFile(file, &newline, 1, &count, nullptr) || count != 1) {
    const DWORD error = GetLastError(); logging_error.store(error ? error : ERROR_WRITE_FAULT);
    gate.fetch_or(kClosed, std::memory_order_acq_rel); return false;
  }
  bytes_written.fetch_add(1, std::memory_order_relaxed); return true;
}
void DisableHook() noexcept;
void DiscardUnstartedRecording(const std::wstring& path) noexcept {
  if (file == INVALID_HANDLE_VALUE) return;
  CloseHandle(file);
  file = INVALID_HANDLE_VALUE;
  DeleteFileW(path.c_str());
}
DWORD WINAPI Writer(void*) noexcept {
  try {
    std::ostringstream header; header.imbue(std::locale::classic());
    header << "{\"type\":\"onfoot_event_session\",\"schema\":1,\"mode\":\"observation_only\",\"capture\":\"all_queue_posts\""
           << ",\"pid\":" << GetCurrentProcessId() << ",\"qpc_frequency\":" << qpc_frequency.QuadPart
           << ",\"time_anchor_qpc\":" << anchor_before.QuadPart + (anchor_after.QuadPart - anchor_before.QuadPart) / 2
           << ",\"time_anchor_qpc_before\":" << anchor_before.QuadPart << ",\"time_anchor_qpc_after\":" << anchor_after.QuadPart
           << ",\"time_anchor_filetime\":" << anchor_filetime << ",\"image_base\":" << image_base
           << ",\"image_size\":" << image_size << ",\"queue_helper_rva\":" << kQueueRva
           << ",\"selector_requests_are_applied_state\":false,\"observed_selector_ids\":[1497268741,103566125,1979902012]"
           << ",\"native_motion_route_schema\":1,\"observed_rtpc_ids\":[1497268741,103566125,2410709172,1326349511,2581359525,1951144523,4274346379]"
           << ",\"max_file_bytes\":" << file_limit << ",\"controlled_fixture\":" << (controlled_fixture.load() ? "true" : "false")
           << ",\"max_capture_ms\":" << observation_window.DurationMs()
           << ",\"verified_game_sha256\":";
    if (controlled_fixture.load()) header << "null}";
    else header << "\"ed5a8f8cd10f9a80d3bda5badd7ba1d4fcfc15996c8c7e018852cb170e264b63\"}";
    bool writable = WriteLine(header.str());
    for (;;) {
      if (observation_window.Expired()) gate.fetch_or(kClosed, std::memory_order_acq_rel);
      Record record{};
      while (Dequeue(record)) {
        if (writable && WriteLine(Json(record))) written.fetch_add(1, std::memory_order_relaxed);
        else { writable = false; storage_drops.fetch_add(1, std::memory_order_relaxed); }
      }
      const bool stop = WaitForSingleObject(stop_event, 20) == WAIT_OBJECT_0 ||
                        observation_window.TimeLimitReached() || limit_reached.load() || logging_error.load();
      if (stop && (gate.load(std::memory_order_acquire) & ~kClosed) == 0 &&
          read_position == write_position.load(std::memory_order_acquire)) break;
    }
    if (!numeric_consumer.load(std::memory_order_acquire)) DisableHook();
    const auto stats = GetOnFootEventTraceStats();
    std::ostringstream footer; footer.imbue(std::locale::classic());
    footer << "{\"type\":\"onfoot_event_summary\",\"capture_closed\":true,\"hooked\":" << (stats.hooked ? "true" : "false")
           << ",\"installation_error\":" << stats.installation_error << ",\"logging_error\":" << stats.logging_error
           << ",\"time_limit_reached\":" << (stats.time_limit_reached ? "true" : "false")
           << ",\"limit_reached\":" << (stats.limit_reached ? "true" : "false") << ",\"post_calls\":" << stats.post_calls
           << ",\"rtpc_calls\":" << stats.rtpc_calls << ",\"switch_calls\":" << stats.switch_calls
           << ",\"register_calls\":" << stats.register_calls << ",\"unregister_calls\":" << stats.unregister_calls
           << ",\"written\":" << stats.written << ",\"queue_drops\":" << stats.queue_drops
           << ",\"admission_drops\":" << stats.admission_drops << ",\"storage_drops\":" << stats.storage_drops << '}';
    if (!logging_error.load()) WriteLine(footer.str(), true);
    if (!FlushFileBuffers(file) && !logging_error.load()) logging_error.store(GetLastError());
  } catch (...) { logging_error.store(ERROR_NOT_ENOUGH_MEMORY); gate.fetch_or(kClosed, std::memory_order_acq_rel); }
  return 0;
}
void DisableHook() noexcept {
  AcquireSRWLockExclusive(&hook_shutdown);
  for (std::size_t i = 0; i < targets.size(); ++i) {
    if (!(hook_mask.load(std::memory_order_acquire) & (DWORD{1} << i))) continue;
    const auto status = MH_DisableHook(targets[i]);
    if (status == MH_OK || status == MH_ERROR_DISABLED) hook_mask.fetch_and(~(DWORD{1} << i), std::memory_order_release);
  }
  if (hook_mask.load(std::memory_order_acquire) & kEventActionHook) {
    const auto status = MH_DisableHook(event_action_target);
    if (status == MH_OK || status == MH_ERROR_DISABLED)
      hook_mask.fetch_and(~kEventActionHook,std::memory_order_release);
  }
  ReleaseSRWLockExclusive(&hook_shutdown);
}

void EnableEventActionHook(Address image, std::size_t size, const OnFootEventTraceFixtureTarget* fixture) noexcept {
  std::array<unsigned char, 32> bytes{};
  if (!fixture && (kEventActionRva > size || bytes.size() > size-kEventActionRva)) return;
  const auto address = fixture ? fixture->event_action : image+kEventActionRva;
  if (!address || !ReadBytes(address,bytes.data(),bytes.size()) ||
      bytes != (fixture ? fixture->event_action_signature : kEventActionSignature)) return;
  auto* target = reinterpret_cast<void*>(address);
  void* trampoline = nullptr;
  if (MH_CreateHook(target,reinterpret_cast<void*>(&ObserveEventAction),&trampoline) != MH_OK) return;
  original_event_action.store(reinterpret_cast<EventActionFn>(trampoline),std::memory_order_release);
  if (MH_EnableHook(target) != MH_OK) {
    MH_RemoveHook(target);
    return;
  }
  event_action_target = target;
  hook_mask.fetch_or(kEventActionHook,std::memory_order_release);
}
}  // namespace

static DWORD StartTrace(Address image, std::size_t size, bool verified, bool requested,
                        const wchar_t* log_directory, const OnFootEventTraceFixtureTarget* fixture,
                        OnFootInputConsumer consumer = nullptr, ShipWeaponBegin ship_begin = nullptr) noexcept {
  if (!requested && !consumer) return ERROR_SUCCESS;
  AcquireSRWLockExclusive(&lifecycle);
  if (started.load(std::memory_order_acquire)) { ReleaseSRWLockExclusive(&lifecycle); return ERROR_ALREADY_INITIALIZED; }
  DWORD error = ERROR_SUCCESS;
  if (!verified || !image || (requested && (!log_directory || !*log_directory)) || image > std::numeric_limits<Address>::max() - size)
    error = ERROR_INVALID_PARAMETER;
  if (fixture && (!fixture->max_capture_ms || fixture->max_capture_ms > kMaxLiveObservationMs)) error = ERROR_INVALID_PARAMETER;
  std::array<unsigned char, 32> observed{};
  for (std::size_t i = 0; !error && i < targets.size(); ++i) {
    const Address target = fixture ? fixture->addresses[i] : image + kRvas[i];
    if ((fixture && (fixture->max_file_bytes < 4096 || fixture->max_file_bytes > kMaxBytes)) ||
        (!fixture && (kRvas[i] > size || observed.size() > size - kRvas[i])) ||
        !ReadBytes(target, observed.data(), observed.size()) || observed != (fixture ? fixture->signatures[i] : kSignatures[i]))
      error = ERROR_REVISION_MISMATCH;
    else { targets[i] = reinterpret_cast<void*>(target); profile_signatures[i] = fixture ? fixture->signatures[i] : kSignatures[i]; }
  }
  if (error) { installation_error.store(error); ReleaseSRWLockExclusive(&lifecycle); return error; }
  installation_error.store(ERROR_SUCCESS, std::memory_order_release);
  enabled.store(requested); started.store(true); stopped.store(false); controlled_fixture.store(fixture != nullptr);
  ship_weapon_begin.store(ship_begin, std::memory_order_relaxed);
  numeric_consumer.store(consumer,std::memory_order_release);
  image_base = image; image_size = size;
  file_limit = fixture ? fixture->max_file_bytes : kMaxBytes;
  observation_window.Initialize(fixture && fixture->observation_clock ? fixture->observation_clock : &MonotonicMs,
                                fixture ? fixture->max_capture_ms : kMaxLiveObservationMs);
  std::size_t created = 0;
  std::wstring path;
  try {
    for (std::size_t i = 0; i < queue.size(); ++i) queue[i].sequence.store(i, std::memory_order_relaxed);
    FILETIME time{};
    if (!QueryPerformanceFrequency(&qpc_frequency) || qpc_frequency.QuadPart <= 0) error = ERROR_INVALID_DATA;
    QueryPerformanceCounter(&anchor_before); GetSystemTimePreciseAsFileTime(&time); QueryPerformanceCounter(&anchor_after);
    anchor_filetime = (static_cast<U64>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
    if (!error && requested) {
      path = std::wstring(log_directory) + L"\\onfoot-events-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(anchor_filetime) + L".jsonl";
      file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (file == INVALID_HANDLE_VALUE) error = GetLastError();
    }
    HMODULE own_module = nullptr;
    if (!error && !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                                     reinterpret_cast<LPCWSTR>(&StartOnFootEventTrace), &own_module)) error = GetLastError();
    if (!error) { stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr); if (!stop_event) error = GetLastError(); }
    const auto initialized = error ? MH_ERROR_NOT_INITIALIZED : MH_Initialize();
    if (!error && initialized != MH_OK && initialized != MH_ERROR_ALREADY_INITIALIZED) error = ERROR_INVALID_FUNCTION;
    const std::array<void*,5> detours{{reinterpret_cast<void*>(&ObservePost),reinterpret_cast<void*>(&ObserveRtpc),
      reinterpret_cast<void*>(&ObserveSwitch),reinterpret_cast<void*>(&ObserveRegister),reinterpret_cast<void*>(&ObserveUnregister)}};
    std::array<void*,5> trampolines{};
    while (!error && created < targets.size()) {
      if (MH_CreateHook(targets[created],detours[created],&trampolines[created]) != MH_OK) error = ERROR_INVALID_FUNCTION;
      else ++created;
    }
    if (!error) {
      original_post.store(reinterpret_cast<PostFn>(trampolines[0]),std::memory_order_release);
      original_rtpc.store(reinterpret_cast<RtpcFn>(trampolines[1]),std::memory_order_release);
      original_switch.store(reinterpret_cast<SwitchFn>(trampolines[2]),std::memory_order_release);
      original_register.store(reinterpret_cast<ObjectFn>(trampolines[3]),std::memory_order_release);
      original_unregister.store(reinterpret_cast<ObjectFn>(trampolines[4]),std::memory_order_release);
    }
    if (!error) {
      for (std::size_t i = 0; i < targets.size(); ++i) {
        if (MH_EnableHook(targets[i]) != MH_OK) { error = ERROR_INVALID_FUNCTION; break; }
        hook_mask.fetch_or(DWORD{1} << i,std::memory_order_release);
      }
    }
    if (!error && ship_begin) EnableEventActionHook(image,size,fixture);
    if (!error && requested) {
      gate.store(0, std::memory_order_release);
      worker = CreateThread(nullptr, 0, Writer, nullptr, 0, nullptr); if (!worker) error = GetLastError();
    }
  } catch (...) { error = ERROR_NOT_ENOUGH_MEMORY; }
  installation_error.store(error, std::memory_order_release);
  if (error) {
    numeric_consumer.store(nullptr,std::memory_order_release);
    const DWORD exposed_hooks = hook_mask.load(std::memory_order_acquire);
    gate.fetch_or(kClosed, std::memory_order_acq_rel); DisableHook();
    // In-flight detours may still need trampolines from hooks already enabled.
    for (std::size_t i = 0; i < created; ++i) {
      if (!(exposed_hooks & (DWORD{1} << i))) MH_RemoveHook(targets[i]);
    }
    DiscardUnstartedRecording(path);
    if (stop_event) { CloseHandle(stop_event); stop_event = nullptr; }
    stopped.store(true, std::memory_order_release);
    // An early failure left no hooks to reuse. Allow the next request to start fresh.
    if (!created) started.store(false, std::memory_order_release);
  }
  ReleaseSRWLockExclusive(&lifecycle); return error;
}
DWORD StartOnFootEventTrace(Address image, std::size_t size, bool verified, bool requested, const wchar_t* directory) noexcept {
  return StartTrace(image, size, verified, requested, directory, nullptr);
}
DWORD StartOnFootEventTraceControlledFixture(const OnFootEventTraceFixtureTarget& fixture, const wchar_t* directory) noexcept {
  return StartTrace(fixture.image, fixture.image_size, true, true, directory, &fixture);
}
DWORD StartOnFootEventSourcesControlledFixture(const OnFootEventTraceFixtureTarget& fixture,
                                               OnFootInputConsumer consumer,
                                               ShipWeaponBegin ship_begin) noexcept {
  return StartTrace(fixture.image,fixture.image_size,true,false,nullptr,&fixture,consumer,ship_begin);
}
DWORD StartOnFootEventSources(Address image, std::size_t size, bool verified,
                             OnFootInputConsumer consumer, ShipWeaponBegin ship_begin) noexcept {
  return StartTrace(image,size,verified,false,nullptr,nullptr,consumer,ship_begin);
}
void ClearOnFootEventConsumer() noexcept { numeric_consumer.store(nullptr,std::memory_order_release); }
bool ShipMotionStopReady() noexcept {
  return original_event_action.load(std::memory_order_acquire) != nullptr &&
         (hook_mask.load(std::memory_order_acquire) & kEventActionHook);
}
bool StopShipMotion(U32 event, U64 object, U32 playing) noexcept {
  const auto action = original_event_action.load(std::memory_order_acquire);
  return action && playing && action(event,0,object,0,0,playing) == 1;
}
DWORD StartOnFootEventRecording(const wchar_t* directory) noexcept {
  AcquireSRWLockExclusive(&lifecycle);
  if (!started.load() || installation_error.load() || worker || file != INVALID_HANDLE_VALUE ||
      !directory || !*directory || (gate.load(std::memory_order_acquire) & ~kClosed)) {
    ReleaseSRWLockExclusive(&lifecycle); return ERROR_INVALID_STATE;
  }
  DWORD error = ERROR_SUCCESS;
  const bool retained = numeric_consumer.load(std::memory_order_acquire) != nullptr;
  for (std::size_t i = 0; i < targets.size() && !error; ++i) {
    if (hook_mask.load() & (DWORD{1} << i)) continue;
    std::array<unsigned char,32> bytes{};
    if (!ReadBytes(reinterpret_cast<Address>(targets[i]),bytes.data(),bytes.size()) || bytes != profile_signatures[i]) error = ERROR_REVISION_MISMATCH;
  }
  if (error) { ReleaseSRWLockExclusive(&lifecycle); return error; }
  if (stop_event) { CloseHandle(stop_event); stop_event = nullptr; }
  observation_window.Reset();
  for (std::size_t i = 0; i < queue.size(); ++i) queue[i].sequence.store(i,std::memory_order_relaxed);
  write_position.store(0); read_position = 0; sequence.store(0); written.store(0); bytes_written.store(0);
  post_calls.store(0); rtpc_calls.store(0); switch_calls.store(0); register_calls.store(0); unregister_calls.store(0);
  queue_drops.store(0); admission_drops.store(0); storage_drops.store(0); limit_reached.store(false); logging_error.store(0);
  FILETIME time{}; QueryPerformanceCounter(&anchor_before); GetSystemTimePreciseAsFileTime(&time); QueryPerformanceCounter(&anchor_after);
  anchor_filetime = (static_cast<U64>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
  std::wstring path;
  try {
    path = std::wstring(directory) + L"\\onfoot-events-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(anchor_filetime) + L".jsonl";
    file = CreateFileW(path.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if (file == INVALID_HANDLE_VALUE) error = GetLastError();
    if (!error) { stop_event = CreateEventW(nullptr,TRUE,FALSE,nullptr); if (!stop_event) error = GetLastError(); }
    for (std::size_t i = 0; i < targets.size() && !error; ++i) if (!(hook_mask.load() & (DWORD{1} << i))) {
      if (MH_EnableHook(targets[i]) != MH_OK) error = ERROR_INVALID_FUNCTION;
      else hook_mask.fetch_or(DWORD{1} << i);
    }
    if (!error) {
      enabled.store(true); stopped.store(false); gate.store(0,std::memory_order_release);
      worker = CreateThread(nullptr,0,Writer,nullptr,0,nullptr); if (!worker) error = GetLastError();
    }
  } catch (...) { error = ERROR_NOT_ENOUGH_MEMORY; }
  if (error) {
    gate.fetch_or(kClosed,std::memory_order_acq_rel);
    if (!retained) DisableHook();
    DiscardUnstartedRecording(path);
    if (stop_event) { CloseHandle(stop_event); stop_event = nullptr; }
    stopped.store(true); logging_error.store(error);
  }
  ReleaseSRWLockExclusive(&lifecycle); return error;
}
bool StopOnFootEventTrace() noexcept {
  AcquireSRWLockExclusive(&lifecycle);
  gate.fetch_or(kClosed, std::memory_order_acq_rel);
  const bool retained = numeric_consumer.load(std::memory_order_acquire) != nullptr;
  if (!retained) DisableHook();
  if (stop_event) SetEvent(stop_event);
  const bool done = !worker || WaitForSingleObject(worker, 10000) == WAIT_OBJECT_0;
  if (done) {
    if (worker) { CloseHandle(worker); worker = nullptr; }
    if (file != INVALID_HANDLE_VALUE) { CloseHandle(file); file = INVALID_HANDLE_VALUE; }
    if (stop_event) { CloseHandle(stop_event); stop_event = nullptr; }
    stopped.store(true, std::memory_order_release);
  }
  const bool clean = done && (retained || hook_mask.load(std::memory_order_acquire) == 0);
  ReleaseSRWLockExclusive(&lifecycle); return clean;
}
OnFootEventTraceStats GetOnFootEventTraceStats() noexcept {
  OnFootEventTraceStats result;
  result.installation_error = installation_error.load(); result.logging_error = logging_error.load(); result.hook_mask = hook_mask.load();
  result.enabled = enabled.load(); result.started = started.load(); result.stopped = stopped.load(); result.hooked = result.hook_mask != 0;
  result.controlled_fixture = controlled_fixture.load(); result.capture_open = !(gate.load() & kClosed); result.limit_reached = limit_reached.load();
  result.time_limit_reached = observation_window.TimeLimitReached();
  result.post_calls = post_calls.load(); result.written = written.load(); result.queue_drops = queue_drops.load();
  result.rtpc_calls = rtpc_calls.load(); result.switch_calls = switch_calls.load();
  result.register_calls = register_calls.load(); result.unregister_calls = unregister_calls.load();
  result.admission_drops = admission_drops.load(); result.storage_drops = storage_drops.load(); result.bytes_written = bytes_written.load(); return result;
}
}  // namespace edtrace
