#include "edtrace/xinput_output.hpp"
#include "edtrace/motor_headroom.hpp"
#include "edtrace/motor_ownership.hpp"
#include "edtrace/motor_sender_profile.hpp"
#include "edtrace/gameinput_output.hpp"
#include "edtrace/authored_triggers.hpp"
#include "MinHook.h"
#ifdef ED_CROSSOVER_COMPAT
#include "edtrace/output_slot_router.hpp"
#include "edtrace/mac_output.hpp"
#endif
#ifdef ED_VR_HAPTICS_MIRROR
#include "edtrace/vr_output.hpp"
#endif
#include <atomic>
#include <cstring>

namespace edtrace { namespace {
std::atomic<MotorBackend> original{nullptr};
std::atomic<bool> installed{false}, ready{false};
bool shutdown_mute = false, wwise_original_active = false;
SRWLOCK original_slot_lock = SRWLOCK_INIT;
#ifdef ED_CROSSOVER_COMPAT
std::atomic<bool> require_scoped_wwise{false};
#endif
std::atomic<DWORD> error{ERROR_NOT_READY};
std::atomic<std::uint64_t> calls{0}, wwise_calls{0};
std::atomic<std::uint64_t> runtime_calls{0}, runtime_nonzero_calls{0};
std::atomic<DWORD> runtime_last_result{ERROR_NOT_READY};
std::uintptr_t main_image = 0;
std::size_t main_size = 0;
thread_local bool selected_request_known = false;
thread_local XINPUT_VIBRATION selected_request{};
thread_local float selected_left = 0, selected_right = 0;
thread_local bool selected_override = false;
SRWLOCK mixer_lock = SRWLOCK_INIT;
XINPUT_VIBRATION authored_request{};
float authored_left = 0, authored_right = 0;
HapticFrame onfoot_frame{};
bool gamepad_focused = true, fallback_active = false;

WORD Word(float value) noexcept { return static_cast<WORD>(HapticUnit(value) * 65535.0f); }
WORD ExtendQuietMotor(WORD native, float amplitude, double endpoint) noexcept {
  // Both comparisons reject NaN and infinities before the integer conversion.
  if (native || !(amplitude > 0.0f && amplitude <= motor_profile::quiet_cutoff)) return native;
  return static_cast<WORD>(amplitude * (endpoint / motor_profile::quiet_cutoff));
}
struct Mixed { XINPUT_VIBRATION main{}; float left = 0, right = 0; bool supplemental = false; };
Mixed Mix() noexcept {
  const WORD low = Word(onfoot_frame.low), high = Word(onfoot_frame.high);
  return {{low > authored_request.wLeftMotorSpeed ? low : authored_request.wLeftMotorSpeed,
           high > authored_request.wRightMotorSpeed ? high : authored_request.wRightMotorSpeed},
          HapticUnit(onfoot_frame.left) > HapticUnit(authored_left) ? HapticUnit(onfoot_frame.left) : HapticUnit(authored_left),
          HapticUnit(onfoot_frame.right) > HapticUnit(authored_right) ? HapticUnit(onfoot_frame.right) : HapticUnit(authored_right),
          HapticNonzero(onfoot_frame)};
}

// Slot 1 can also be used by unrelated callers. Serialize its native writes
// separately from mixing so only the latest successful writer owns a stop.
DWORD ForwardOriginal(DWORD index, XINPUT_VIBRATION* vibration, bool authored) noexcept {
  const auto incoming = GetLastError();
  if (index == 1) AcquireSRWLockExclusive(&original_slot_lock);
  XINPUT_VIBRATION value{}; SIZE_T count = 0;
  const bool active = authored && vibration &&
    ReadProcessMemory(GetCurrentProcess(),vibration,&value,sizeof(value),&count) && count == sizeof(value) &&
    (value.wLeftMotorSpeed || value.wRightMotorSpeed);
  SetLastError(incoming);
  const auto result = original.load(std::memory_order_acquire)(index,vibration);
  const auto outgoing = GetLastError();
  if (index == 1) {
    if (result == ERROR_SUCCESS) wwise_original_active = active;
    ReleaseSRWLockExclusive(&original_slot_lock);
  }
  SetLastError(outgoing); return result;
}
DWORD WINAPI UnownedOriginal(DWORD index, XINPUT_VIBRATION* vibration) noexcept {
  return ForwardOriginal(index,vibration,false);
}
DWORD WINAPI AuthoredOriginal(DWORD index, XINPUT_VIBRATION* vibration) noexcept {
  return ForwardOriginal(index,vibration,true);
}
DWORD ServiceShutdown() noexcept {
  auto outgoing = GetLastError();
  AcquireSRWLockExclusive(&original_slot_lock);
  DWORD result = ERROR_SUCCESS;
  if (wwise_original_active && original.load(std::memory_order_acquire)) {
    XINPUT_VIBRATION zero{};
    result = original.load(std::memory_order_acquire)(1,&zero);
    outgoing = GetLastError();
    if (result == ERROR_SUCCESS) wwise_original_active = false;
  }
  ReleaseSRWLockExclusive(&original_slot_lock);
  SetLastError(outgoing); return result;
}

DWORD WINAPI SelectedOutput(DWORD index, XINPUT_VIBRATION* vibration) noexcept {
  const DWORD incoming = GetLastError();
  if (selected_request_known) {
#ifdef ED_CROSSOVER_COMPAT
    if(index==0&&MacOutputSubmit({selected_request.wLeftMotorSpeed/65535.0f,selected_request.wRightMotorSpeed/65535.0f,selected_left,selected_right})) {
      SetLastError(incoming);return ERROR_SUCCESS;
    }
#endif
#ifdef ED_VR_HAPTICS_MIRROR
    VrOutputPublish({selected_request.wLeftMotorSpeed/65535.0f,selected_request.wRightMotorSpeed/65535.0f,selected_left,selected_right});
#endif
    const auto outcome = GameInputOutputSubmit(index, selected_request, selected_left, selected_right);
    if (outcome != GameInputSubmission::NativeFallback) {
      // SetRumbleState is void: this is accepted submission/quieting, not a
      // fabricated claim that a native XInput call reached physical motors.
      SetLastError(incoming); return ERROR_SUCCESS;
    }
    if (index == 0 && !gamepad_focused) { SetLastError(incoming); return ERROR_SUCCESS; }
  }
  SetLastError(incoming);
  const DWORD result = original.load(std::memory_order_acquire)(index, selected_override ? &selected_request : vibration);
  if (selected_request_known && index == 0 && result == ERROR_SUCCESS)
    fallback_active = selected_request.wLeftMotorSpeed || selected_request.wRightMotorSpeed;
  return result;
}

DWORD WINAPI Output(DWORD index, XINPUT_VIBRATION* vibration) noexcept {
  const DWORD incoming = GetLastError();
  const auto caller = reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
  // GameInput has an asynchronous XInput provider. Its own output must reach
  // the original function without looping back or releasing Wwise's lease.
  if (GameInputOutputOwnsCaller(caller)) {
    XINPUT_VIBRATION value{}; SIZE_T count = 0;
    const bool known = vibration && ReadProcessMemory(GetCurrentProcess(), vibration, &value, sizeof(value), &count) && count == sizeof(value);
    SetLastError(incoming);
    const DWORD result = UnownedOriginal(index, vibration);
    const DWORD outgoing = GetLastError();
    runtime_calls.fetch_add(1, std::memory_order_relaxed);
    if (known && (value.wLeftMotorSpeed || value.wRightMotorSpeed)) runtime_nonzero_calls.fetch_add(1, std::memory_order_relaxed);
    runtime_last_result.store(result, std::memory_order_relaxed);
    SetLastError(outgoing); return result;
  }
  const bool inside = caller >= main_image && caller - main_image < main_size;
  const auto writer = Classify(true, inside, inside ? caller - main_image : 0);
  calls.fetch_add(1, std::memory_order_relaxed);
  if (writer == Writer::WwiseShared || writer == Writer::WwiseTerminationRetry)
    wwise_calls.fetch_add(1, std::memory_order_relaxed);
  XINPUT_VIBRATION snapshot{};
  SIZE_T bytes = 0;
  bool known = vibration && ReadProcessMemory(GetCurrentProcess(), vibration, &snapshot, sizeof(snapshot), &bytes) && bytes == sizeof(snapshot);
  const bool wwise = writer == Writer::WwiseShared || writer == Writer::WwiseTerminationRetry;
#ifdef ED_CROSSOVER_COMPAT
  XINPUT_VIBRATION quiet{};
  // The game may lose the frame-slot cap while the gain worker is still
  // restoring gain. Quiet any unscoped request that could carry motor output.
  if (wwise && require_scoped_wwise.load(std::memory_order_acquire) && !MotorSenderScope::Engaged() &&
      (!known || snapshot.wLeftMotorSpeed || snapshot.wRightMotorSpeed)) {
    snapshot = quiet;
    vibration = &quiet;
    known = true;
  }
#endif
  // Keep mixer updates and their backend submissions in one order. In
  // particular a delayed supplemental expiry cannot overtake an authored stop.
  if (wwise) AcquireSRWLockExclusive(&mixer_lock);
  if (wwise && shutdown_mute && (index == 0 || index == 1)) {
    // Muting discards the late request. Only retry a stop for a slot-1
    // command still owned by Wwise; another observed writer keeps its output.
    SetLastError(incoming);
    const auto result = index == 1 ? ServiceShutdown() : ERROR_SUCCESS;
    const auto outgoing = index == 1 && result != ERROR_SUCCESS ? GetLastError() : incoming;
    ReleaseSRWLockExclusive(&mixer_lock);
    SetLastError(outgoing);
    return result;
  }
  const bool enabled = ready.load(std::memory_order_acquire);
  if (wwise && known && !enabled && !gamepad_focused && RoutedIndex(enabled,writer,index) == 0) {
    ReleaseSRWLockExclusive(&mixer_lock); SetLastError(incoming); return ERROR_SUCCESS;
  }
  const bool saved_known = selected_request_known, saved_override = selected_override; const auto saved_request = selected_request;
  const float saved_left = selected_left, saved_right = selected_right;
  const auto trigger_frame = AuthoredTriggersCurrentFrame();
  Mixed mixed{snapshot, trigger_frame.valid ? trigger_frame.left : 0, trigger_frame.valid ? trigger_frame.right : 0, false};
  const bool managed = wwise && enabled && RoutedIndex(enabled,writer,index) == 0;
  bool recovered_quiet = false;
  if (managed && known) {
    authored_request = snapshot;
    if (writer == Writer::WwiseShared) {
      const auto raw = MotorSenderScope::Current();
      authored_request.wLeftMotorSpeed = ExtendQuietMotor(snapshot.wLeftMotorSpeed, raw.left, motor_profile::quiet_left_endpoint);
      authored_request.wRightMotorSpeed = ExtendQuietMotor(snapshot.wRightMotorSpeed, raw.right, motor_profile::quiet_right_endpoint);
      recovered_quiet = authored_request.wLeftMotorSpeed != snapshot.wLeftMotorSpeed ||
        authored_request.wRightMotorSpeed != snapshot.wRightMotorSpeed;
    }
    authored_left = mixed.left; authored_right = mixed.right; mixed = Mix();
  }
  selected_request_known = known; selected_request = mixed.main; selected_left = mixed.left; selected_right = mixed.right;
  selected_override = managed && known && (mixed.supplemental || recovered_quiet);
  const auto gameinput = GameInputOutputGetStats();
  const bool trigger_output = (gameinput.selected && (gameinput.supported_motors & 12) == 12)
#ifdef ED_CROSSOVER_COMPAT
    || MacOutputGetStats().active
#endif
#ifdef ED_VR_HAPTICS_MIRROR
    || VrOutputGetStats().enabled || VrOutputGetStats().listening
#endif
    ;
  const bool trigger_active = managed && trigger_output && (mixed.left > 0 || mixed.right > 0);
  SetLastError(incoming);
  const auto dispatched = DispatchOwnedMotor(managed ? &SelectedOutput : (wwise ? &AuthoredOriginal : &UnownedOriginal), enabled, enabled,
                                        writer, index, vibration, known, managed && known ? mixed.main : snapshot, nullptr, nullptr, trigger_active);
  const auto result = dispatched.result;
  const DWORD outgoing = GetLastError();
  selected_request_known = saved_known; selected_request = saved_request; selected_override = saved_override;
  selected_left = saved_left; selected_right = saved_right;
  if (wwise) ReleaseSRWLockExclusive(&mixer_lock);
  SetLastError(outgoing); return result;
}
}}

namespace edtrace {
#ifdef ED_CROSSOVER_COMPAT
void XInputOutputRequireScopedWwiseFrames() noexcept {
  require_scoped_wwise.store(true,std::memory_order_release);
}
#endif
DWORD XInputOutputStart(MotorBackend target, std::uintptr_t image, std::size_t size, bool verified, MotorBackend dynamic_target) noexcept {
  if (!verified || !image || size <= kWwiseRetryReturnRva || !target) return ERROR_REVISION_MISMATCH;
  if (original.load()) return ERROR_ALREADY_INITIALIZED;
  shutdown_mute = wwise_original_active = false;
  main_image = image; main_size = size;
#ifdef ED_CROSSOVER_COMPAT
  original.store(target, std::memory_order_release);
  const DWORD result = OutputSlotsStart(image, size, verified, target, dynamic_target ? dynamic_target : target, &Output);
  if (result) { error.store(result); return result; }
#else
  (void)dynamic_target;
  const auto initialization = MH_Initialize();
  if (initialization != MH_OK && initialization != MH_ERROR_ALREADY_INITIALIZED) return ERROR_NOT_SUPPORTED;
  void* trampoline = nullptr;
  if (MH_CreateHook(reinterpret_cast<void*>(target), reinterpret_cast<void*>(&Output), &trampoline) != MH_OK) return ERROR_NOT_SUPPORTED;
  MotorBackend function = nullptr;
  static_assert(sizeof(function) == sizeof(trampoline));
  std::memcpy(&function, &trampoline, sizeof(function));
  original.store(function, std::memory_order_release);
  if (MH_EnableHook(reinterpret_cast<void*>(target)) != MH_OK) {
    error.store(ERROR_NOT_SUPPORTED); return ERROR_NOT_SUPPORTED;
  }
#endif
  installed.store(true, std::memory_order_release); error.store(ERROR_SUCCESS);
  return ERROR_SUCCESS;
}
bool XInputOutputRefresh() noexcept {
#ifdef ED_CROSSOVER_COMPAT
  const bool result = OutputSlotsRefresh(); error.store(OutputSlotsError()); return result;
#else
  return installed.load(std::memory_order_acquire);
#endif
}
void XInputOutputSetReady(bool gain_enabled, bool controller_connected, bool focused, bool unique_gamepad) noexcept {
  const DWORD incoming = GetLastError();
  AcquireSRWLockExclusive(&mixer_lock);
  const bool enabled = installed.load(std::memory_order_acquire) && gain_enabled && controller_connected
#ifdef ED_CROSSOVER_COMPAT
    && OutputSlotsReady()
#endif
    ;
  gamepad_focused = focused;
#ifdef ED_CROSSOVER_COMPAT
  const auto previous_generation=MacOutputGetStats().generation;
  MacOutputSelect(enabled&&focused&&unique_gamepad);
  if(previous_generation!=MacOutputGetStats().generation) {
    authored_request={};authored_left=0;authored_right=0;onfoot_frame={};
  }
  const bool mac_owned=MacOutputBlocksFallback();
  // Backend handoff and output submissions share the mixer's order. Native
  // Core Haptics must stop (or expire) before either fallback can resume.
  GameInputOutputPoll(enabled&&!mac_owned,focused);
  if(mac_owned&&fallback_active) {
    XINPUT_VIBRATION zero{};original.load(std::memory_order_acquire)(0,&zero);fallback_active=false;
  }
#else
  (void)unique_gamepad;
#endif
  ready.store(enabled, std::memory_order_release);
  if (!enabled) { authored_request = {}; authored_left = 0; authored_right = 0; onfoot_frame = {}; }
  if ((!enabled || !focused) && fallback_active) {
    XINPUT_VIBRATION zero{};
    DispatchOwnedMotor(original.load(std::memory_order_acquire), true, true, Writer::OnFoot, 0, &zero, true, zero);
    fallback_active = false;
  }
  ReleaseSRWLockExclusive(&mixer_lock); SetLastError(incoming);
}
void XInputOutputBeginShutdown() noexcept {
  const auto incoming = GetLastError();
  AcquireSRWLockExclusive(&mixer_lock);
  shutdown_mute = true;
  ServiceShutdown();
  ReleaseSRWLockExclusive(&mixer_lock);
  SetLastError(incoming);
}
DWORD XInputOutputServiceShutdown() noexcept {
  const auto incoming = GetLastError();
  const auto result = ServiceShutdown();
  SetLastError(incoming); return result;
}
void XInputOutputEndShutdown() noexcept {
  const auto incoming = GetLastError();
  AcquireSRWLockExclusive(&mixer_lock);
  shutdown_mute = false;
  ReleaseSRWLockExclusive(&mixer_lock);
  SetLastError(incoming);
}
XInputOutputStats XInputOutputGetStats() noexcept {
  return {error.load(), installed.load(std::memory_order_acquire), ready.load(std::memory_order_acquire), calls.load(), wwise_calls.load(),
          runtime_calls.load(), runtime_nonzero_calls.load(), runtime_last_result.load()};
}
MotorBackend XInputOutputNativeBackend() noexcept { return original.load(std::memory_order_acquire); }
void XInputOutputSetOnFootFrame(HapticFrame frame, bool active) noexcept {
  const DWORD incoming = GetLastError();
  if (!installed.load(std::memory_order_acquire)) return;
  AcquireSRWLockExclusive(&mixer_lock);
  const bool route_ready = ready.load(std::memory_order_acquire);
  active = active && route_ready;
  const bool previous = HapticNonzero(onfoot_frame);
  onfoot_frame = active ? HapticScale(frame, 1) : HapticFrame{};
  // Supplemental disable/fault removes only its contribution. Global route
  // cancellation clears authored state in SetReady under this same lock.
  const bool current = HapticNonzero(onfoot_frame);
  Mixed mixed = route_ready ? Mix() : Mixed{};
  if (!previous && !current) { ReleaseSRWLockExclusive(&mixer_lock); SetLastError(incoming); return; }
  const bool saved_known = selected_request_known, saved_override = selected_override;
  const auto saved_request = selected_request; const float saved_left = selected_left, saved_right = selected_right;
  selected_request_known = true; selected_override = true; selected_request = mixed.main;
  selected_left = mixed.left; selected_right = mixed.right;
  const auto gi = GameInputOutputGetStats();
  const bool triggers = ((gi.selected && (gi.supported_motors & 12) == 12)
#ifdef ED_CROSSOVER_COMPAT
    || MacOutputGetStats().active
#endif
#ifdef ED_VR_HAPTICS_MIRROR
    || VrOutputGetStats().enabled || VrOutputGetStats().listening
#endif
    ) && (mixed.left > 0 || mixed.right > 0);
  SetLastError(incoming);
  DispatchOwnedMotor(&SelectedOutput, true, true, Writer::OnFoot, 0, &mixed.main, true, mixed.main, nullptr, nullptr, triggers);
  selected_request_known = saved_known; selected_override = saved_override; selected_request = saved_request;
  ReleaseSRWLockExclusive(&mixer_lock);
  selected_left = saved_left; selected_right = saved_right; SetLastError(incoming);
}
}
