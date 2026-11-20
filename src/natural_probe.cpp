#include "edtrace/natural_probe.hpp"
#include "edtrace/natural_gain_control.hpp"
#include <array>
#include <atomic>
#include <cstring>
#include <sstream>
#include <limits>

namespace edtrace { namespace {
using U32 = std::uint32_t;
using U64 = std::uint64_t;
using LoadFn = int (WINAPI*)(const void*, U32, U32*);
using RegisterFn = int (WINAPI*)(U64);
using UnloadFn = int (WINAPI*)(U32, const void*);
enum class CallbackType : int {};
struct CallbackInfo;
using Callback = void (WINAPI*)(CallbackType, CallbackInfo*);
struct ExternalSource;
using PostFn = U32 (WINAPI*)(U32, U64, U32, Callback, void*, U32, ExternalSource*, U32);
std::uintptr_t image = 0;
constexpr U64 object_id = 0x4544525450524f42ULL;
LoadFn load_bank = nullptr;
RegisterFn register_object = nullptr, unregister_object = nullptr;
UnloadFn unload_bank = nullptr;
PostFn post_event = nullptr;
HANDLE stop_event = nullptr, done_event = nullptr, worker = nullptr;
std::atomic<DWORD> error{0};
std::wstring output_dir;
bool registered = false, bank_loaded = false;
bool session_restoration = false;
bool (*headroom_ready)() noexcept = nullptr;
std::atomic<bool> restoration_enabled{false}, restoration_owned{false};

template <typename T> bool Read(std::uintptr_t address, T& value) noexcept {
  if (address < 65536 || address > 0x7fffffffffffULL - sizeof(T)) return false;
  SIZE_T count = 0;
  return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address), &value, sizeof(value), &count) && count == sizeof(value);
}
std::uintptr_t Lookup(U32 id, std::uintptr_t pointer_offset, std::uintptr_t count_offset, bool* known = nullptr) noexcept {
  if (known) *known = false;
  std::uintptr_t manager = 0, buckets = 0; U32 count = 0;
  if (!Read(image + 0x604e6e8, manager) || !manager || !Read(manager + pointer_offset, buckets) ||
      !Read(manager + count_offset, count) || !buckets || !count || count > 1048576) return 0;
  std::uintptr_t node = 0;
  if (!Read(buckets + 8 * (id % count), node)) return 0;
  for (unsigned i = 0; node && i < 1024; ++i) {
    U32 current = 0; std::uintptr_t next = 0;
    if (!Read(node + 16, current) || !Read(node + 8, next)) return 0;
    if (current == id) { if (known) *known = true; return node; }
    if (next == node) return 0;
    node = next;
  }
  if (!node && known) *known = true;
  return 0;
}
bool Property(std::uintptr_t bundle, std::size_t stride, float& value) noexcept {
  value = 0;
  if (!bundle) return true;
  BYTE count = 0;
  if (!Read(bundle, count)) return false;
  for (unsigned i = 0; i < count; ++i) {
    BYTE id = 0; if (!Read(bundle + 1 + i, id)) return false;
    if (id == 0) return Read(bundle + ((count + 4) & ~3U) + stride * i, value) && std::isfinite(value);
  }
  return true;
}
bool State(void*, MotionGainState& state) noexcept {
  const auto node = Lookup(373542019U, 0x28, 0x30);
  std::uintptr_t vtable = 0, authored = 0, sis = 0, bundle = 0, rtpc = 0, states = 0;
  if (!node || !Read(node, vtable) || vtable != image + 0x57e9520 ||
      !Read(node + 0xa8, authored) || !Read(node + 0x68, sis) || !Read(node + 0x38, rtpc) || !Read(node + 0x28, states)) return false;
  state.identity = node; state.id = 373542019U;
  if (!Property(authored, 4, state.authored)) return false;
  if (sis && (!Read(sis + 24, bundle) || !Property(bundle, 16, state.global_delta))) return false;
  // The authored root has no RTPC. State contributions must be inactive for this probe.
  bool neutral = rtpc == 0;
  std::uintptr_t item = 0;
  if (states && !Read(states + 8, item)) return false;
  unsigned visited = 0;
  while (item && visited++ < 128) {
    std::uintptr_t next = 0, props = 0;
    if (!Read(item + 16, next) || !Read(item + 32, props) || next == item) return false;
    if (props) neutral = false;  // A populated state is outside this narrow A/B test.
    item = next;
  }
  if (item) return false;
  state.unmodulated_root = neutral;
  std::uintptr_t stable_sis = 0;
  return Read(node + 0x68, stable_sis) && stable_sis == sis && Lookup(373542019U, 0x28, 0x30) == node;
}
bool CollisionFree(void*) noexcept {
  // Event IDs must not resolve before this bank loads. Other action IDs were
  // required for the supported runtime control-bank contract.
  bool first_known = false, second_known = false;
  const auto first = Lookup(gain_bank::enable_event, 0xe8, 0xf0, &first_known);
  const auto second = Lookup(gain_bank::restore_event, 0xe8, 0xf0, &second_known);
  bool action1_known = false, action2_known = false;
  const auto action1 = Lookup(gain_bank::enable_action, 0x128, 0x130, &action1_known);
  const auto action2 = Lookup(gain_bank::restore_action, 0x128, 0x130, &action2_known);
  return first_known && second_known && action1_known && action2_known &&
         first == 0 && second == 0 && action1 == 0 && action2 == 0;
}
bool Load(void*) noexcept {
  if (register_object(object_id) != 1) return false;
  registered = true;
  U32 identifier = 0;
  const int result = load_bank(gain_bank::bytes, sizeof(gain_bank::bytes), &identifier);
  bank_loaded = result == 1 && identifier == gain_bank::bank;
  return bank_loaded;
}
U32 Post(void*, U32 event) noexcept {
  if (event == gain_bank::enable_event && headroom_ready && !headroom_ready()) return 0;
  return post_event(event, object_id, 0, nullptr, nullptr, 0, nullptr, 0);
}
bool Marker(const wchar_t* name) {
  const DWORD value = GetFileAttributesW((output_dir + L"\\" + name).c_str());
  return value != INVALID_FILE_ATTRIBUTES && !(value & FILE_ATTRIBUTE_DIRECTORY);
}
ULONGLONG MarkerTime(const wchar_t* name) {
  WIN32_FILE_ATTRIBUTE_DATA value{};
  if (!GetFileAttributesExW((output_dir + L"\\" + name).c_str(), GetFileExInfoStandard, &value)) return 0;
  if (value.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return 0;
  return (static_cast<ULONGLONG>(value.ftLastWriteTime.dwHighDateTime) << 32) | value.ftLastWriteTime.dwLowDateTime;
}
template <typename T> bool Resolve(T& target, const char* name) noexcept {
  const auto address = GetProcAddress(reinterpret_cast<HMODULE>(image), name);
  static_assert(sizeof(target) == sizeof(address)); std::memcpy(&target, &address, sizeof(target)); return target != nullptr;
}
DWORD WINAPI Run(void*) noexcept {
  HANDLE log = INVALID_HANDLE_VALUE;
  MotionGainControl control(true, {nullptr, State, CollisionFree, Load, Post}, session_restoration);
  try {
    const auto path = output_dir + L"\\native-gain-" + std::to_wstring(GetCurrentProcessId()) + L".jsonl";
    log = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (log == INVALID_HANDLE_VALUE) { error.store(GetLastError()); SetEvent(done_event); return 0; }
    auto record = [&](const char* action) {
      restoration_owned.store(control.owns_change(), std::memory_order_release);
      MotionGainState state{}; const bool known = State(nullptr, state);
      std::ostringstream out;
      out << "{\"type\":\"native_gain\",\"action\":\"" << action << "\",\"phase\":" << static_cast<int>(control.phase())
          << ",\"control_error\":" << static_cast<int>(control.error()) << ",\"playing_id\":" << control.playing_id()
          << ",\"snapshot_known\":" << (known ? "true" : "false") << ",\"authored_db\":" << state.authored
          << ",\"global_sis_delta_db\":" << state.global_delta << ",\"owns_change\":" << (control.owns_change() ? "true" : "false") << "}\n";
      const auto text = out.str(); DWORD written = 0;
      if (!WriteFile(log, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) || written != text.size()) error.store(ERROR_WRITE_FAULT);
      if (!FlushFileBuffers(log)) error.store(GetLastError());
    };
    record("ready_no_property_change");
    bool enable_seen = false, restore_seen = false;
    ULONGLONG play_marker = 0;
    ULONGLONG enabled_at = 0;
    GainPhase last = control.phase();
    std::uintptr_t enabled_root_identity = 0;
    while (WaitForSingleObject(stop_event, 50) != WAIT_OBJECT_0) {
      if (headroom_ready && !headroom_ready()) { error.store(ERROR_NOT_SUPPORTED); break; }
      MotionGainState ready{};
      const bool automatic_ready = session_restoration && State(nullptr, ready) && ready.authored == -96.0f;
      if (!enable_seen && (session_restoration ? automatic_ready : Marker(L"natural-gain-enable.marker"))) {
        enabled_root_identity = ready.identity;
        enable_seen = true; control.Enable(); record("enable_queued"); enabled_at = GetTickCount64();
      }
      control.Observe();
      if (session_restoration && control.phase() == GainPhase::Failed) error.store(ERROR_INVALID_DATA);
      if (control.phase() != last) { record("readback"); last = control.phase(); }
      if (session_restoration) {
        MotionGainState current{};
        restoration_enabled.store(control.phase() == GainPhase::Enabled && State(nullptr, current) &&
            current.identity == enabled_root_identity && current.authored == -96.0f && current.global_delta == 96.0f,
            std::memory_order_release);
      }
      const auto requested_play = session_restoration ? 0 : MarkerTime(L"natural-motion-play.marker");
      if (requested_play && requested_play != play_marker) {
        play_marker = requested_play;
        // Exact-build catalog proves this nested event contains only the single
        // launched Motion sound, without the ordinary launch event's SetStates.
        bool event_known = false;
        if (control.Prepare() && Lookup(2042794513U, 0xe8, 0xf0, &event_known) && event_known && Post(nullptr, 2042794513U))
          record("authored_motion_event_queued");
        else record("authored_motion_event_rejected");
      }
      if (!session_restoration && !restore_seen && (Marker(L"natural-gain-restore.marker") || (enabled_at && GetTickCount64() - enabled_at >= 180000))) {
        if (control.Restore()) { restore_seen = true; record("restore_queued"); }
      }
    }
    restoration_enabled.store(false, std::memory_order_release);
    const ULONGLONG deadline = GetTickCount64() + 5000;
    while (control.owns_change() && GetTickCount64() < deadline) {
      control.Observe(); control.Restore(); control.Observe();
      if (control.owns_change()) Sleep(20);
    }
    record(control.owns_change() ? "restoration_unconfirmed" : "restored_or_no_change");
    if (control.owns_change()) error.store(ERROR_TIMEOUT);
    // Leave a failed/pending bank resident until exit rather than cancelling its actions.
    if (!control.owns_change()) {
      if (bank_loaded && unload_bank(gain_bank::bank, nullptr) != 1) error.store(ERROR_GEN_FAILURE);
      if (registered && unregister_object(object_id) != 1) error.store(ERROR_GEN_FAILURE);
    }
  } catch (...) { error.store(ERROR_NOT_ENOUGH_MEMORY); }
  restoration_enabled.store(false, std::memory_order_release);
  // Restoration must still be attempted after a writer/allocation failure.
  if (control.owns_change()) {
    const ULONGLONG deadline = GetTickCount64() + 5000;
    while (control.owns_change() && GetTickCount64() < deadline) {
      control.Observe(); control.Restore(); control.Observe();
      if (control.owns_change()) Sleep(20);
    }
    if (control.owns_change()) error.store(ERROR_TIMEOUT);
  }
  if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
  restoration_owned.store(control.owns_change(), std::memory_order_release);
  SetEvent(done_event); return 0;
}
}}
namespace edtrace {
DWORD NaturalProbeStart(std::uintptr_t base, bool verified, const std::wstring& directory) noexcept {
  if (!verified) return ERROR_REVISION_MISMATCH;
  if (worker) return ERROR_ALREADY_INITIALIZED;
  image = base;
  try {
    output_dir = directory;
    if (Marker(L"natural-gain-enable.marker") || Marker(L"natural-gain-restore.marker") || Marker(L"natural-motion-play.marker"))
      return ERROR_ALREADY_EXISTS;
    if (!Resolve(load_bank, "?LoadBankMemoryCopy@SoundEngine@AK@@YA?AW4AKRESULT@@PEBXKAEAK@Z") ||
        !Resolve(register_object, "?RegisterGameObj@SoundEngine@AK@@YA?AW4AKRESULT@@_K@Z") ||
        !Resolve(unregister_object, "?UnregisterGameObj@SoundEngine@AK@@YA?AW4AKRESULT@@_K@Z") ||
        !Resolve(unload_bank, "?UnloadBank@SoundEngine@AK@@YA?AW4AKRESULT@@KPEBX@Z") ||
        !Resolve(post_event, "?PostEvent@SoundEngine@AK@@YAKK_KKP6AXW4AkCallbackType@@PEAUAkCallbackInfo@@@ZPEAXKPEAUAkExternalSourceInfo@@K@Z")) return ERROR_PROC_NOT_FOUND;
    stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr); done_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stop_event || !done_event) return GetLastError();
    worker = CreateThread(nullptr, 0, Run, nullptr, 0, nullptr);
    return worker ? ERROR_SUCCESS : GetLastError();
  } catch (...) { return ERROR_NOT_ENOUGH_MEMORY; }
}
DWORD NaturalProbeStop(DWORD timeout_ms) noexcept {
  if (!worker) return ERROR_SUCCESS;
  SetEvent(stop_event);
  const DWORD wait = WaitForSingleObject(done_event, timeout_ms);
  if (wait == WAIT_TIMEOUT) return ERROR_TIMEOUT;
  return wait == WAIT_OBJECT_0 ? error.load() : GetLastError();
}
DWORD NaturalRestorationStart(std::uintptr_t base, bool verified, const std::wstring& directory,
                              bool (*ready)() noexcept) noexcept {
  if (worker) return ERROR_ALREADY_INITIALIZED;
  if (!ready) return ERROR_INVALID_PARAMETER;
  session_restoration = true;
  headroom_ready = ready;
  const DWORD result = NaturalProbeStart(base, verified, directory);
  if (result) {
    headroom_ready = nullptr;
    session_restoration = false;
  }
  return result;
}
NaturalRestorationStatus NaturalRestorationGetStatus() noexcept {
  return {error.load(), restoration_enabled.load(std::memory_order_acquire), restoration_owned.load(std::memory_order_acquire)};
}
}
