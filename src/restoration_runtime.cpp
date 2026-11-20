#include "edtrace/restoration_runtime.hpp"
#include "edtrace/control.hpp"
#include "edtrace/capture_request.hpp"
#include "edtrace/profile.hpp"
#include "edtrace/xinput_output.hpp"
#include "edtrace/sco_feedback.hpp"
#ifdef ED_CROSSOVER_COMPAT
#include "edtrace/mac_output.hpp"
#include "edtrace/output_slot_router.hpp"
// Read the loader-resolved import directly instead of taking the address
// of MinGW's local XInput import stub.
extern "C" edtrace::MotorBackend __imp_XInputSetState;
#endif
#include "edtrace/motor_headroom.hpp"
#if __has_include("edup_version.h")
#include "edup_version.h"
#endif
#ifndef EDUP_BUILD_DESCRIBE
#define EDUP_BUILD_DESCRIBE "unknown"
#endif
#include "MinHook.h"
#include "edtrace/motor_ownership.hpp"
#include "edtrace/natural_probe.hpp"
#include "edtrace/gameinput_output.hpp"
#include "edtrace/gameinput_compat.hpp"
#include "edtrace/gameinput_compat_hid.hpp"
#include "edtrace/authored_triggers.hpp"
#include "edtrace/authored_trigger_trace.hpp"
#include "edtrace/onfoot_event_trace.hpp"
#include "edtrace/onfoot_runtime.hpp"
#ifdef ED_VR_HAPTICS_MIRROR
#include "edtrace/vr_output.hpp"
#include "edtrace/vr_auto_launch.hpp"
#include "edtrace/meta_output.hpp"
#endif
#include <bcrypt.h>
#include <array>
#include <atomic>
#include <cstring>
#include <string>
#include <cwchar>

namespace edtrace { namespace {
INIT_ONCE once = INIT_ONCE_STATIC_INIT;
HANDLE stop_event = nullptr, done_event = nullptr, error_event = nullptr;
std::atomic<DWORD> start_error{ERROR_NOT_READY};
std::atomic<bool> profile_match{false}, stopped{false};
std::wstring log_directory;
std::atomic<DWORD> gameinput_error{ERROR_NOT_READY};
const char* controller_output_mode = "auto";
const char* startup_stage = "profile";
bool gameinput_attempted = false;
std::atomic<DWORD> trigger_hook_error{ERROR_NOT_READY}, trigger_trace_error{ERROR_NOT_READY};
std::atomic<DWORD> onfoot_trace_error{0};
#ifdef ED_CROSSOVER_COMPAT
std::atomic<DWORD> output_restore_error{ERROR_SUCCESS};
#endif
constexpr char gameinput_runtime_sha256[] = "156030ed7928791df36ab9033061a151be61251054c45cf3ae65cd460509b04e";
constexpr char gameinput_helper_sha256[] = "d5d34853ccc9d9f8f19644cf71ffba625d0575bbb80ab19fbc2882b632aee174";
using GetCapabilitiesFn = DWORD (WINAPI*)(DWORD, DWORD, XINPUT_CAPABILITIES*);

template <typename T> T Symbol(HMODULE module, const char* name) noexcept {
  const auto address = GetProcAddress(module, name); T result{};
  static_assert(sizeof(result) == sizeof(address)); std::memcpy(&result, &address, sizeof(result)); return result;
}
#ifdef ED_CROSSOVER_COMPAT
bool IsArmCrossOver() noexcept {
  if (GetFileAttributesW(L"\\\\?\\unix/System/Library/CoreServices/SystemVersion.plist") == INVALID_FILE_ATTRIBUTES)
    return false;
  using MachineQuery = BOOL (WINAPI*)(HANDLE, USHORT*, USHORT*);
  const auto query = Symbol<MachineQuery>(GetModuleHandleW(L"kernel32.dll"), "IsWow64Process2");
  USHORT process = 0, native = 0;
  return query && query(GetCurrentProcess(), &process, &native) && native == IMAGE_FILE_MACHINE_ARM64;
}
#endif
std::wstring Path(HMODULE module) {
  std::array<wchar_t, 32768> value{};
  const DWORD size = GetModuleFileNameW(module, value.data(), static_cast<DWORD>(value.size()));
  return size && size < value.size() ? std::wstring(value.data(), size) : std::wstring{};
}
std::string Hash(const std::wstring& path) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  if (file == INVALID_HANDLE_VALUE) return {};
  BCRYPT_ALG_HANDLE algorithm = nullptr; BCRYPT_HASH_HANDLE hash = nullptr;
  std::string result;
  if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0) {
    if (BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) >= 0) {
      std::array<BYTE, 65536> buffer{}; DWORD size = 0; bool valid = true;
      for (;;) {
        if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &size, nullptr)) { valid = false; break; }
        if (!size) break;
        if (BCryptHashData(hash, buffer.data(), size, 0) < 0) { valid = false; break; }
      }
      std::array<BYTE, 32> digest{};
      if (valid && BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0) {
        constexpr char hex[] = "0123456789abcdef";
        for (const auto byte : digest) { result += hex[byte >> 4]; result += hex[byte & 15]; }
      }
      BCryptDestroyHash(hash);
    }
    BCryptCloseAlgorithmProvider(algorithm, 0);
  }
  CloseHandle(file); return result;
}
template <typename T> bool Read(std::uintptr_t address, T& value) noexcept {
  SIZE_T bytes = 0;
  return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(address), &value, sizeof(value), &bytes) && bytes == sizeof(value);
}
bool Profile(std::uintptr_t image, std::size_t& size) noexcept {
  IMAGE_DOS_HEADER dos{}; IMAGE_NT_HEADERS64 nt{};
  if (!Read(image, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0 || dos.e_lfanew >= 1024 * 1024 ||
      !Read(image + dos.e_lfanew, nt) || nt.Signature != IMAGE_NT_SIGNATURE || nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return false;
  size = nt.OptionalHeader.SizeOfImage;
  struct Site { std::uintptr_t rva; unsigned size; std::array<BYTE, 5> bytes; };
  for (const auto& site : {Site{0x5c8530, 5, {0xe8, 0x81, 0x7d, 0x34, 0x04}},
                           Site{0x4d05310, 2, {0xff, 0xd0, 0, 0, 0}}, Site{0x4d05393, 2, {0xff, 0xd0, 0, 0, 0}}}) {
    std::array<BYTE, 5> actual{}; SIZE_T count = 0;
    if (site.rva + site.size > size || !ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(image + site.rva), actual.data(), site.size, &count) ||
        count != site.size || std::memcmp(actual.data(), site.bytes.data(), site.size)) return false;
  }
  return true;
}
float FeedbackStrength(const std::wstring& path, const wchar_t* section) noexcept {
  std::array<wchar_t,32> text{};
  GetPrivateProfileStringW(section,L"Strength",L"60",text.data(),static_cast<DWORD>(text.size()),path.c_str());
  wchar_t* end = nullptr; long value = std::wcstol(text.data(),&end,10);
  if (end == text.data() || *end) return .6f;
  if (value < 0) value = 0;
  if (value > 100) value = 100;
  return static_cast<float>(value) / 100.0f;
}
void SaveStatus() noexcept {
  try {
    const auto s = RestorationGetStatus();
    const auto supplemental = GetOnFootRuntimeStats();
    const std::string text = std::string("{\"schema\":8,\"build\":\"" EDUP_BUILD_DESCRIBE "\",\"host\":\"dinput8\",\"output\":\"") +
#ifdef ED_CROSSOVER_COMPAT
      (MacOutputGetStats().active ? "macos-corehaptics" : MacOutputGetStats().blocks_fallback ? "macos-handoff" :
#endif
      (s.gameinput_selected ? (s.gameinput_compat_ready ? "gameinput-compat-hid" : "gameinput") : "xinput")
#ifdef ED_CROSSOVER_COMPAT
      )
#endif
      + "\",\"profile_match\":" + std::to_string(s.profile_match) +
      ",\"start_error\":" + std::to_string(s.start_error) + ",\"output_hook\":" + std::to_string(s.output_hook) +
      ",\"startup_stage\":\"" + startup_stage + "\"" +
      ",\"output_hook_error\":" + std::to_string(XInputOutputGetStats().error) +
      ",\"headroom_error\":" + std::to_string(MotorHeadroomGetStats().installation_error) +
      ",\"headroom_minhook_status\":" + std::to_string(MotorHeadroomGetStats().minhook_status) +
      ",\"minhook_fallback_slots\":" + std::to_string(MH_GetFallbackSlotsInUse()) +
      ",\"headroom_frame_slot\":" + std::to_string(MotorHeadroomGetStats().frame_slot) +
      ",\"headroom_frame_slot_rollback_error\":" + std::to_string(MotorHeadroomGetStats().frame_slot_rollback_error) +
      ",\"controller_output_mode\":\"" + controller_output_mode + "\"" +
#ifdef ED_CROSSOVER_COMPAT
      ",\"platform_profile\":\"macos-crossover\",\"output_routing\":\"game-slots\"" +
      ",\"output_restore_error\":" + std::to_string(output_restore_error.load()) +
      ",\"macos_bridge_error\":" + std::to_string(MacOutputGetStats().error) +
      ",\"macos_launch_error\":" + std::to_string(MacOutputGetStats().launch_error) +
      ",\"macos_helper_launched\":" + std::to_string(MacOutputGetStats().launched) +
      ",\"macos_helper_polls\":" + std::to_string(MacOutputGetStats().polls) +
      ",\"macos_frames\":" + std::to_string(MacOutputGetStats().frames) +
      ",\"macos_trigger_frames\":" + std::to_string(MacOutputGetStats().trigger_frames) +
      ",\"macos_client_error\":" + std::to_string(MacOutputGetStats().client_error) +
      ",\"macos_native_active\":" + std::to_string(MacOutputGetStats().active) +
      ",\"macos_blocks_fallback\":" + std::to_string(MacOutputGetStats().blocks_fallback) +
      ",\"macos_acquiring\":" + std::to_string(MacOutputGetStats().acquiring) +
      ",\"macos_controller_count\":" + std::to_string(MacOutputGetStats().controllers) +
      ",\"macos_motor_mask\":" + std::to_string(MacOutputGetStats().motors) +
#else
      ",\"platform_profile\":\"standard\",\"output_routing\":\"xinput-detour\"" +
#endif
      ",\"headroom\":" + std::to_string(s.headroom) + ",\"gain_enabled\":" + std::to_string(s.gain_enabled) +
      ",\"routing_ready\":" + std::to_string(s.routing_ready) + ",\"stopped\":" + std::to_string(s.stopped) +
      ",\"motor_calls\":" + std::to_string(s.motor_calls) + ",\"wwise_calls\":" + std::to_string(s.wwise_calls) +
      ",\"suppressed_engine_zeros\":" + std::to_string(s.suppressed_engine_zeros) +
      ",\"gameinput_error\":" + std::to_string(s.gameinput_error) + ",\"gameinput_initialized\":" + std::to_string(s.gameinput_initialized) +
      ",\"gameinput_attempted\":" + std::to_string(gameinput_attempted) +
      ",\"gameinput_selected\":" + std::to_string(s.gameinput_selected) + ",\"gameinput_matches\":" + std::to_string(s.gameinput_matches) +
      ",\"gameinput_submissions\":" + std::to_string(s.gameinput_submissions) + ",\"gameinput_stops\":" + std::to_string(s.gameinput_stops) +
      ",\"gameinput_runtime_calls\":" + std::to_string(s.gameinput_runtime_calls) + ",\"gameinput_runtime_nonzero_calls\":" + std::to_string(s.gameinput_runtime_nonzero_calls) +
      ",\"gameinput_runtime_last_result\":" + std::to_string(s.gameinput_runtime_last_result) +
      ",\"trigger_hook_error\":" + std::to_string(s.trigger_hook_error) + ",\"trigger_hook_ready\":" + std::to_string(s.trigger_hook_ready) +
      ",\"trigger_contexts\":" + std::to_string(s.trigger_contexts) + ",\"trigger_negotiated\":" + std::to_string(s.trigger_negotiated) +
      ",\"trigger_frames\":" + std::to_string(s.trigger_frames) + ",\"trigger_late_frames\":" + std::to_string(s.trigger_late_frames) +
      ",\"trigger_consume_two\":" + std::to_string(s.trigger_consume_two) + ",\"trigger_consume_four\":" + std::to_string(s.trigger_consume_four) +
      ",\"trigger_nonzero_frames\":" + std::to_string(s.trigger_nonzero_frames) +
      ",\"trigger_peak_left_milli\":" + std::to_string(s.trigger_peak_left_milli) +
      ",\"trigger_peak_right_milli\":" + std::to_string(s.trigger_peak_right_milli) +
      ",\"gameinput_supported_motors\":" + std::to_string(s.gameinput_supported_motors) + ",\"gameinput_trigger_submissions\":" + std::to_string(s.gameinput_trigger_submissions) +
      ",\"trigger_trace_error\":" + std::to_string(s.trigger_trace_error) +
      ",\"trigger_trace_time_limit_reached\":" + std::to_string(s.trigger_trace_time_limit_reached) +
      ",\"gameinput_compat_error\":" + std::to_string(s.gameinput_compat_error) + ",\"gameinput_compat_ready\":" + std::to_string(s.gameinput_compat_ready) +
      ",\"gameinput_compat_waiting_zero\":" + std::to_string(s.gameinput_compat_waiting_zero) + ",\"gameinput_compat_pending_stop\":" + std::to_string(s.gameinput_compat_pending_stop) +
      ",\"gameinput_hid_error\":" + std::to_string(s.gameinput_hid_error) + ",\"gameinput_hid_write_error\":" + std::to_string(s.gameinput_hid_write_error) +
      ",\"gameinput_hid_writes\":" + std::to_string(s.gameinput_hid_writes) + ",\"gameinput_hid_failures\":" + std::to_string(s.gameinput_hid_failures) +
      ",\"gameinput_compat_triggers\":" + std::to_string(s.gameinput_compat_triggers) + ",\"gameinput_compat_refreshes\":" + std::to_string(s.gameinput_compat_refreshes) +
      ",\"gameinput_compat_native_fences\":" + std::to_string(s.gameinput_compat_native_fences) + ",\"gameinput_compat_suppressed_native\":" + std::to_string(s.gameinput_compat_suppressed_native) +
      ",\"gameinput_compat_vendor\":" + std::to_string(s.gameinput_compat_vendor) + ",\"gameinput_compat_product\":" + std::to_string(s.gameinput_compat_product) +
      ",\"gameinput_compat_matches\":" + std::to_string(s.gameinput_compat_matches) + ",\"gameinput_compat_gamepads\":" + std::to_string(s.gameinput_compat_gamepads) +
      ",\"gameinput_compat_steam_alias\":" + std::to_string(s.gameinput_compat_steam_alias) +
      ",\"onfoot_trace_error\":" + std::to_string(s.onfoot_trace_error) + ",\"onfoot_trace_log_error\":" + std::to_string(s.onfoot_trace_log_error) +
      ",\"onfoot_trace_enabled\":" + std::to_string(s.onfoot_trace_enabled) + ",\"onfoot_trace_open\":" + std::to_string(s.onfoot_trace_open) +
      ",\"onfoot_trace_hooked\":" + std::to_string(s.onfoot_trace_hooked) + ",\"onfoot_trace_posts\":" + std::to_string(s.onfoot_trace_posts) +
      ",\"onfoot_trace_written\":" + std::to_string(s.onfoot_trace_written) + ",\"onfoot_trace_drops\":" + std::to_string(s.onfoot_trace_drops) +
      ",\"onfoot_trace_hook_mask\":" + std::to_string(s.onfoot_trace_hook_mask) + ",\"onfoot_trace_rtpc\":" + std::to_string(s.onfoot_trace_rtpc) +
      ",\"onfoot_trace_time_limit_reached\":" + std::to_string(s.onfoot_trace_time_limit_reached) +
      ",\"onfoot_trace_switch\":" + std::to_string(s.onfoot_trace_switch) + ",\"onfoot_trace_register\":" + std::to_string(s.onfoot_trace_register) +
      ",\"onfoot_trace_unregister\":" + std::to_string(s.onfoot_trace_unregister) +
      ",\"onfoot_configured\":" + std::to_string(s.onfoot_configured) + ",\"onfoot_active\":" + std::to_string(s.onfoot_active) +
      ",\"onfoot_faulted\":" + std::to_string(s.onfoot_faulted) + ",\"onfoot_queued\":" + std::to_string(s.onfoot_queued) +
      ",\"onfoot_processed\":" + std::to_string(s.onfoot_processed) + ",\"onfoot_dropped\":" + std::to_string(s.onfoot_dropped) +
      ",\"onfoot_effects\":" + std::to_string(s.onfoot_effects) + ",\"onfoot_rejected\":" + std::to_string(s.onfoot_rejected) +
      ",\"onfoot_duplicates\":" + std::to_string(s.onfoot_duplicates) + ",\"onfoot_expired_holds\":" + std::to_string(s.onfoot_expired_holds) +
      ",\"onfoot_actor_overflows\":" + std::to_string(s.onfoot_actor_overflows) + ",\"onfoot_output_frames\":" + std::to_string(s.onfoot_output_frames)
      + ",\"ship_combat_configured\":" + std::to_string(supplemental.ship_configured) +
      ",\"ship_combat_active\":" + std::to_string(supplemental.ship_active)
#ifdef ED_VR_HAPTICS_MIRROR
      + ",\"vr_listening\":" + std::to_string(VrOutputGetStats().listening)
      + ",\"vr_error\":" + std::to_string(VrOutputGetStats().error)
      + ",\"vr_polls\":" + std::to_string(VrOutputGetStats().polls)
      + ",\"vr_replies\":" + std::to_string(VrOutputGetStats().replies)
      + ",\"vr_frames\":" + std::to_string(VrOutputGetStats().frames)
      + ",\"vr_enabled\":" + std::to_string(VrOutputGetStats().enabled)
      + ",\"vr_session_focused\":" + std::to_string(VrOutputGetStats().focused)
      + ",\"vr_backend\":" + std::to_string(VrOutputGetStats().backend)
      + ",\"vr_generation\":" + std::to_string(VrOutputGetStats().generation)
      + ",\"vr_helper_running\":" + std::to_string(VrAutoLaunchGetStats().child_running)
      + ",\"vr_helper_error\":" + std::to_string(VrAutoLaunchGetStats().error)
      + ",\"vr_meta_error\":" + std::to_string(MetaOutputGetStats().error)
      + ",\"vr_meta_session\":" + std::to_string(MetaOutputGetStats().session)
#endif
      + "}\n";
    HANDLE file = CreateFileW((log_directory + L"\\restoration-status.json").c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                              nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) { DWORD count = 0; WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &count, nullptr); CloseHandle(file); }
  } catch (...) { /* Diagnostic I/O cannot change forwarding. */ }
}
DWORD WINAPI Run(void*) noexcept {
  DWORD result = ERROR_SUCCESS;
  std::wstring onfoot_marker;
  CaptureRequest capture_claim;
  try {
    std::array<wchar_t, 32768> temporary{};
    const DWORD count = GetTempPathW(static_cast<DWORD>(temporary.size()), temporary.data());
    if (!count || count >= temporary.size()) throw DWORD(ERROR_BAD_PATHNAME);
    const auto parent = std::wstring(temporary.data(), count) + L"EDRumbleRestoration";
    if (!CreateDirectoryW(parent.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) throw GetLastError();
    FILETIME time{}; GetSystemTimeAsFileTime(&time);
    const auto stamp = (static_cast<std::uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
    log_directory = parent + L"\\" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(stamp);
    if (!CreateDirectoryW(log_directory.c_str(), nullptr)) throw GetLastError();
    const auto image = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    std::size_t size = 0;
    if (Hash(Path(nullptr)) != kGameSha256 || !Profile(image, size)) throw DWORD(ERROR_REVISION_MISMATCH);
    profile_match.store(true, std::memory_order_release);
#ifdef ED_CROSSOVER_COMPAT
    if (IsArmCrossOver()) MH_EnableMainImageFallback();
#endif
    const auto executable = Path(nullptr);
    const auto directory = executable.substr(0, executable.find_last_of(L"\\/"));
    const auto onfoot_config = directory + L"\\ed-onfoot-haptics.ini";
    std::array<wchar_t,16> output_mode{};
    GetPrivateProfileStringW(L"Controller",L"Output",L"auto",output_mode.data(),static_cast<DWORD>(output_mode.size()),onfoot_config.c_str());
#ifdef ED_CROSSOVER_COMPAT
    const auto preference = ParseMacOutputPreference(output_mode.data());
    const bool use_gameinput = preference.gameinput;
    controller_output_mode = preference.name;
#else
    const bool use_gameinput = _wcsicmp(output_mode.data(),L"xinput") != 0;
    controller_output_mode = use_gameinput ? "auto" : "xinput";
#endif
    if (!use_gameinput) gameinput_error.store(ERROR_NOT_SUPPORTED);
    startup_stage = "xinput_backend";
    std::array<wchar_t, 32768> system{};
    const UINT n = GetSystemDirectoryW(system.data(), static_cast<UINT>(system.size()));
    if (!n || n >= system.size()) throw DWORD(ERROR_BAD_PATHNAME);
    const auto library_path = std::wstring(system.data(), n) + L"\\xinput9_1_0.dll";
    const HMODULE library = LoadLibraryExW(library_path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR);
    if (!library) throw GetLastError();
    const auto named = GetModuleHandleW(L"xinput9_1_0.dll");
    const auto set = Symbol<MotorBackend>(library, "XInputSetState");
    const auto capabilities = Symbol<GetCapabilitiesFn>(library, "XInputGetCapabilities");
    if (!set || !capabilities) throw DWORD(ERROR_INVALID_FUNCTION);
    startup_stage = "output_hook";
#ifdef ED_CROSSOVER_COMPAT
    // The proxy's loader-resolved import and the explicit system export
    // independently identify the two XInput bindings we can safely use.
    if (named != library) throw DWORD(ERROR_INVALID_FUNCTION);
    result = XInputOutputStart(__imp_XInputSetState, image, size, true, set);
#else
    MotorBackend imported = nullptr;
    if (!named || Symbol<MotorBackend>(named, "XInputSetState") != set ||
        !Read(image + 0x4db6c60, imported) || imported != set)
      throw DWORD(ERROR_INVALID_FUNCTION);
    result = XInputOutputStart(set, image, size, true);
#endif
    if (result) throw result;
    startup_stage = "headroom";
    result = MotorHeadroomStart(image, size, true);
    if (result) throw result;
    const bool frame_slot_headroom = MotorHeadroomGetStats().frame_slot;
#ifdef ED_CROSSOVER_COMPAT
    if (frame_slot_headroom) XInputOutputRequireScopedWwiseFrames();
#endif
    startup_stage = "optional_hooks";
    // Negotiate before native sink allocation; existing two-float contexts
    // are diagnosed, never resized in place. Optional failure preserves the
    // verified two-motor restoration instead of disabling that baseline.
    trigger_hook_error.store(frame_slot_headroom ? ERROR_NOT_SUPPORTED :
                             AuthoredTriggersStart(image, size, true));
    onfoot_marker = directory + L"\\ed-onfoot-trace.enable";
#ifdef ED_VR_HAPTICS_MIRROR
    VrOutputStart(GetPrivateProfileIntW(L"VR",L"Enabled",1,(directory+L"\\ed-vr-haptics.ini").c_str())==1);
#endif
    const bool feedback_requested = GetPrivateProfileIntW(L"OnFoot",L"Enabled",0,onfoot_config.c_str()) == 1;
    const bool ship_requested = GetPrivateProfileIntW(L"ShipCombat",L"Enabled",0,onfoot_config.c_str()) == 1;
    const bool supplemental_requested = feedback_requested || ship_requested;
    ScoFeedback sco;
    if (GetPrivateProfileIntW(L"SCO", L"Enabled", 1, onfoot_config.c_str()) == 1)
      sco.Start(FeedbackStrength(onfoot_config, L"SCO"));
    capture_claim = ClaimCaptureRequest(onfoot_marker.c_str());
    bool feedback_allowed = feedback_requested;
    bool ship_allowed = ship_requested;
    // Diagnostic source/audio reads are opt-in, separate from authored motor
    // routing. Each native writer also enforces its own monotonic deadline.
    if (capture_claim)
      trigger_trace_error.store(StartAuthoredTriggerTrace(image, size, true, log_directory.c_str()));
    if (supplemental_requested)
      StartOnFootFeedback(FeedbackStrength(onfoot_config,L"OnFoot"),ship_requested,
                          FeedbackStrength(onfoot_config,L"ShipCombat"),feedback_requested);
    const auto consumer = supplemental_requested ? &QueueOnFootInput : nullptr;
    const auto ship_begin = ship_requested ? &BeginShipWeaponInput : nullptr;
    const DWORD source_result = consumer ? StartOnFootEventSources(image,size,true,consumer,ship_begin) :
        StartOnFootEventTrace(image,size,true,static_cast<bool>(capture_claim),log_directory.c_str());
    const DWORD recording_result = !source_result && consumer && capture_claim ?
        StartOnFootEventRecording(log_directory.c_str()) : source_result;
    onfoot_trace_error.store(recording_result);
    if (source_result && supplemental_requested) StopOnFootFeedback();
    if (use_gameinput) {
      startup_stage = "gameinput";
      const auto gameinput_path = directory + L"\\GameInputRedist.dll";
      const auto helper_path = directory + L"\\GameInputRawInputProxy.exe";
      const auto existing_runtime = GetModuleHandleW(L"GameInputRedist.dll");
      if (Hash(gameinput_path) == gameinput_runtime_sha256 && Hash(helper_path) == gameinput_helper_sha256 &&
          (!existing_runtime || Hash(Path(existing_runtime)) == gameinput_runtime_sha256)) {
        gameinput_attempted = true;
#ifdef ED_CROSSOVER_COMPAT
        gameinput_error.store(ERROR_IO_PENDING);
        SaveStatus();
#endif
        gameinput_error.store(GameInputOutputStart(gameinput_path, XInputOutputNativeBackend()));
      } else if (GetFileAttributesW(gameinput_path.c_str()) == INVALID_FILE_ATTRIBUTES ||
                 GetFileAttributesW(helper_path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        gameinput_error.store(GetLastError());
      } else gameinput_error.store(ERROR_REVISION_MISMATCH);
    }
#ifdef ED_CROSSOVER_COMPAT
    startup_stage = "macos_bridge";
    SaveStatus();
    MacOutputStart(directory,log_directory,preference.native);
#endif
    startup_stage = "natural_gain";
    if (!MotorHeadroomReady()) throw MotorHeadroomGetStats().installation_error;
    result = NaturalRestorationStart(image, true, log_directory, &MotorHeadroomReady);
    if (result) throw result;
    start_error.store(ERROR_SUCCESS, std::memory_order_release);
    startup_stage = "running";
    bool previous_ready = false; unsigned ticks = 0, control_ticks = 0;
    while (WaitForSingleObject(stop_event, 20) != WAIT_OBJECT_0) {
      XINPUT_CAPABILITIES capability{};
      const auto gain = NaturalRestorationGetStatus();
      if (gain.error) throw gain.error;
      const bool connected = capabilities(0, 0, &capability) == ERROR_SUCCESS;
      DWORD foreground_pid = 0; GetWindowThreadProcessId(GetForegroundWindow(), &foreground_pid);
      const bool focused = foreground_pid == GetCurrentProcessId();
      bool output_connected = connected;
      bool haptic_focused = focused;
#ifdef ED_VR_HAPTICS_MIRROR
      if(VrOutputGetStats().enabled) {
        MetaOutputPoll();
        VrAutoLaunchPoll(VrOutputGetStats().backend<=1);
      }
      output_connected = VrOutputPoll(gain.enabled,focused) || connected;
      haptic_focused = focused || VrOutputGetStats().focused;
#endif
      const bool output_bound = XInputOutputRefresh();
#ifdef ED_CROSSOVER_COMPAT
      MacOutputPoll();
      unsigned connected_slots=connected?1:0;
      for(DWORD slot=1;slot<4;++slot) {
        XINPUT_CAPABILITIES other{};if(capabilities(slot,0,&other)==ERROR_SUCCESS) ++connected_slots;
      }
      AuthoredTriggersSetReady(gain.enabled && output_connected && haptic_focused && output_bound);
      XInputOutputSetReady(gain.enabled && haptic_focused && output_bound,output_connected,focused,connected_slots==1);
#else
      GameInputOutputPoll(gain.enabled && connected && output_bound, focused);
      AuthoredTriggersSetReady(gain.enabled && output_connected && haptic_focused && output_bound);
      XInputOutputSetReady(gain.enabled && haptic_focused && output_bound, output_connected, focused);
#endif
      const bool feedback_ready = gain.enabled && output_connected && haptic_focused && output_bound;
      const auto now = GetTickCount64();
      TickOnFootFeedback(feedback_allowed && feedback_ready, now, sco.Tick(feedback_ready, now), ship_allowed && feedback_ready);
      if (++control_ticks >= 25) {
        control_ticks = 0;
        if (feedback_requested) {
          feedback_allowed = GetPrivateProfileIntW(L"OnFoot",L"Enabled",0,onfoot_config.c_str()) == 1;
          SetOnFootFeedbackStrength(FeedbackStrength(onfoot_config,L"OnFoot"));
        }
        if (ship_requested) {
          ship_allowed = GetPrivateProfileIntW(L"ShipCombat",L"Enabled",0,onfoot_config.c_str()) == 1;
          SetShipWeaponFeedbackStrength(FeedbackStrength(onfoot_config,L"ShipCombat"));
        }
        const auto capture = GetOnFootEventTraceStats();
        const auto trigger_capture = GetAuthoredTriggerTraceStats();
        if (!capture_claim && (capture_claim = ClaimCaptureRequest(onfoot_marker.c_str()))) {
          const DWORD rearmed = capture.started ? StartOnFootEventRecording(log_directory.c_str()) :
            StartOnFootEventTrace(image,size,true,true,log_directory.c_str());
          onfoot_trace_error.store(rearmed);
          if (!GetAuthoredTriggerTraceStats().started)
            trigger_trace_error.store(StartAuthoredTriggerTrace(image,size,true,log_directory.c_str()));
          SaveStatus();
        } else if (capture_claim &&
                   (!CaptureRequestActive(onfoot_marker.c_str(), capture_claim) ||
                    capture.limit_reached || capture.time_limit_reached || capture.logging_error ||
                    ((trigger_capture.time_limit_reached || trigger_capture.logging_error) &&
                     !trigger_capture.stopped) ||
                    onfoot_trace_error.load())) {
          if (!StopOnFootEventTrace()) onfoot_trace_error.store(ERROR_TIMEOUT);
          if (!StopAuthoredTriggerTrace()) trigger_trace_error.store(ERROR_TIMEOUT);
          RetireCaptureRequest(onfoot_marker.c_str(), capture_claim);
          capture_claim = {};
          SaveStatus();
        }
      }
      const bool active = XInputOutputGetStats().ready;
      if (active != previous_ready || ++ticks >= 250) { SaveStatus(); ticks = 0; previous_ready = active; }
    }
  } catch (DWORD failure) { result = failure ? failure : ERROR_GEN_FAILURE; }
  catch (...) { result = ERROR_NOT_ENOUGH_MEMORY; }
  if (result) start_error.store(result, std::memory_order_release);
  // Never remove a live trampoline. Inactive hooks forward until process exit.
  ClearOnFootEventConsumer();
  XInputOutputSetReady(false, false, false);
#ifdef ED_CROSSOVER_COMPAT
  MacOutputStop();
#endif
  StopOnFootFeedback();
  AuthoredTriggersStop();
  GameInputOutputStop();
#ifdef ED_VR_HAPTICS_MIRROR
  VrOutputStop();
  MetaOutputStop();
  VrAutoLaunchStop();
#endif
  if (!StopOnFootEventTrace()) onfoot_trace_error.store(ERROR_TIMEOUT);
  if (!StopAuthoredTriggerTrace()) trigger_trace_error.store(ERROR_TIMEOUT);
  if (capture_claim) RetireCaptureRequest(onfoot_marker.c_str(), capture_claim);
  const DWORD restoration = NaturalProbeStop(10000);
#ifdef ED_CROSSOVER_COMPAT
  // This only retries a failed frame-slot installation before gain started.
  MotorHeadroomRetryFrameSlotRollback();
#endif
  if (restoration) start_error.store(restoration, std::memory_order_release);
#ifdef ED_CROSSOVER_COMPAT
  else {
    DWORD output_restore = OutputSlotsStop();
    if (output_restore) {
      Sleep(20);
      output_restore = OutputSlotsStop();
    }
    output_restore_error.store(output_restore, std::memory_order_release);
  }
#endif
  stopped.store(true, std::memory_order_release); SaveStatus();
  bool failed = start_error.load() != ERROR_SUCCESS;
#ifdef ED_CROSSOVER_COMPAT
  failed = failed || output_restore_error.load() != ERROR_SUCCESS ||
           MotorHeadroomGetStats().frame_slot_rollback_error != ERROR_SUCCESS;
#endif
  if (failed && error_event) SetEvent(error_event);
  SetEvent(done_event); return 0;
}
BOOL CALLBACK Start(PINIT_ONCE, PVOID context, PVOID*) noexcept {
  const auto self = static_cast<HMODULE>(context);
  HMODULE pinned = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                         reinterpret_cast<LPCWSTR>(self), &pinned)) { start_error.store(GetLastError()); return TRUE; }
  wchar_t name[100]{};
  EdTraceEventName(name, 100, GetCurrentProcessId(), L"Stop"); stop_event = CreateEventW(nullptr, TRUE, FALSE, name);
  EdTraceEventName(name, 100, GetCurrentProcessId(), L"Done"); done_event = CreateEventW(nullptr, TRUE, FALSE, name);
  EdTraceEventName(name, 100, GetCurrentProcessId(), L"Error"); error_event = CreateEventW(nullptr, TRUE, FALSE, name);
  if (!stop_event || !done_event || !error_event) { start_error.store(GetLastError()); return TRUE; }
  HANDLE thread = CreateThread(nullptr, 0, Run, nullptr, 0, nullptr);
  if (thread) CloseHandle(thread);
  else { start_error.store(GetLastError()); SetEvent(error_event); SetEvent(done_event); }
  return TRUE;
}
}}

namespace edtrace {
void RequestRestorationStart(HMODULE self) noexcept {
  const auto incoming = GetLastError(); InitOnceExecuteOnce(&once, Start, self, nullptr); SetLastError(incoming);
}
DWORD RestorationStop(DWORD timeout_ms) noexcept {
  if (!stop_event || !done_event) return ERROR_NOT_READY;
  SetEvent(stop_event);
  const DWORD wait = WaitForSingleObject(done_event, timeout_ms);
  if (wait == WAIT_TIMEOUT) return ERROR_TIMEOUT;
  if (wait != WAIT_OBJECT_0) return GetLastError();
  const DWORD error = start_error.load();
#ifdef ED_CROSSOVER_COMPAT
  if (!error) {
    const DWORD rollback = MotorHeadroomGetStats().frame_slot_rollback_error;
    return rollback ? rollback : output_restore_error.load();
  }
#endif
  return error;
}
RestorationStatus RestorationGetStatus() noexcept {
  const auto output = XInputOutputGetStats();
  RestorationStatus result{};
  result.start_error = start_error.load(); result.profile_match = profile_match.load(); result.output_hook = output.installed;
  result.headroom = MotorHeadroomGetStats().active; result.gain_enabled = NaturalRestorationGetStatus().enabled;
  result.routing_ready = output.ready; result.stopped = stopped.load(); result.motor_calls = output.calls; result.wwise_calls = output.wwise_calls;
  result.suppressed_engine_zeros = MotorOwnershipGetStats().suppressed;
  const auto gameinput = GameInputOutputGetStats();
  result.gameinput_error = gameinput_error.load(); result.gameinput_initialized = gameinput.initialized;
  result.gameinput_selected = gameinput.selected; result.gameinput_matches = gameinput.matches;
  result.gameinput_submissions = gameinput.submissions; result.gameinput_stops = gameinput.stops;
  result.gameinput_runtime_calls = output.runtime_calls; result.gameinput_runtime_nonzero_calls = output.runtime_nonzero_calls;
  result.gameinput_runtime_last_result = output.runtime_last_result;
  const auto triggers = AuthoredTriggersGetStats();
  result.trigger_hook_error = trigger_hook_error.load(); result.trigger_hook_ready = triggers.ready;
  result.trigger_contexts = static_cast<DWORD>(triggers.registered_contexts);
  result.trigger_negotiated = triggers.negotiated; result.trigger_frames = triggers.valid_frames; result.trigger_late_frames = triggers.existing_two_channel_frames;
  result.trigger_consume_two = triggers.consume_two_channel_buffers; result.trigger_consume_four = triggers.consume_four_channel_buffers;
  result.trigger_nonzero_frames = triggers.nonzero_frames;
  result.trigger_peak_left_milli = triggers.peak_left_milli; result.trigger_peak_right_milli = triggers.peak_right_milli;
  result.gameinput_supported_motors = gameinput.supported_motors; result.gameinput_trigger_submissions = gameinput.trigger_submissions;
  result.trigger_trace_error = trigger_trace_error.load();
  result.trigger_trace_time_limit_reached = GetAuthoredTriggerTraceStats().time_limit_reached;
  const auto compat = GameInputCompatGetStats();
  const auto hid = GameInputCompatHidGetStats();
  result.gameinput_compat_error = compat.error; result.gameinput_compat_ready = compat.ready;
  result.gameinput_compat_waiting_zero = compat.waiting_native_zero; result.gameinput_compat_pending_stop = compat.pending_stop;
  result.gameinput_hid_error = hid.error; result.gameinput_hid_write_error = hid.last_write_error;
  result.gameinput_hid_writes = hid.writes; result.gameinput_hid_failures = hid.write_failures;
  result.gameinput_compat_triggers = compat.trigger_submissions; result.gameinput_compat_refreshes = compat.refreshes;
  result.gameinput_compat_native_fences = compat.native_fences; result.gameinput_compat_suppressed_native = compat.stale_native_suppressed;
  result.gameinput_compat_vendor = compat.vendor; result.gameinput_compat_product = compat.product;
  result.gameinput_compat_matches = compat.matches; result.gameinput_compat_gamepads = compat.connected_gamepads;
  result.gameinput_compat_steam_alias = compat.steam_virtual_alias;
  const auto onfoot = GetOnFootEventTraceStats();
  result.onfoot_trace_error = onfoot_trace_error.load(); result.onfoot_trace_log_error = onfoot.logging_error;
  result.onfoot_trace_enabled = onfoot.enabled; result.onfoot_trace_open = onfoot.capture_open; result.onfoot_trace_hooked = onfoot.hooked;
  result.onfoot_trace_time_limit_reached = onfoot.time_limit_reached;
  result.onfoot_trace_posts = onfoot.post_calls; result.onfoot_trace_written = onfoot.written;
  result.onfoot_trace_drops = onfoot.queue_drops + onfoot.admission_drops + onfoot.storage_drops;
  result.onfoot_trace_hook_mask = onfoot.hook_mask; result.onfoot_trace_rtpc = onfoot.rtpc_calls;
  result.onfoot_trace_switch = onfoot.switch_calls; result.onfoot_trace_register = onfoot.register_calls;
  result.onfoot_trace_unregister = onfoot.unregister_calls;
  const auto feedback = GetOnFootRuntimeStats();
  result.onfoot_configured = feedback.configured; result.onfoot_active = feedback.active; result.onfoot_faulted = feedback.faulted;
  result.onfoot_queued = feedback.queued; result.onfoot_processed = feedback.processed; result.onfoot_dropped = feedback.dropped;
  result.onfoot_effects = feedback.accepted; result.onfoot_rejected = feedback.rejected; result.onfoot_duplicates = feedback.duplicate_cues;
  result.onfoot_expired_holds = feedback.expired_holds; result.onfoot_actor_overflows = feedback.actor_overflows;
  result.onfoot_output_frames = feedback.output_frames;
  return result;
}
}
