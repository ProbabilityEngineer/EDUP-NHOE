#include "edtrace/motor_headroom.hpp"
#include "edtrace/motor_sender_profile.hpp"
#ifdef ED_CROSSOVER_COMPAT
#include "edtrace/authored_trigger_profile.hpp"
#endif
#include "MinHook.h"

#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>

namespace edtrace { namespace {
using Sender = std::uint64_t (WINAPI*)(void*, float, float);
static_assert(sizeof(void*) == 8 && std::atomic<Sender>::is_always_lock_free);
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
std::atomic<Sender> original{nullptr};
std::atomic<bool> active{false}, installed{false}, fixture_mode{false};
std::atomic<bool> frame_slot{false};
std::atomic<int> minhook_status{MH_UNKNOWN};
std::atomic<DWORD> started{0}, error{ERROR_NOT_READY};
std::atomic<DWORD> frame_slot_rollback_error{ERROR_SUCCESS};
std::atomic<std::uint64_t> calls{0}, capped{0}, invalid{0};

float Bound(float value) noexcept {
  if (!std::isfinite(value)) {
    invalid.fetch_add(1, std::memory_order_relaxed);
    return 0.0f;
  }
  if (value > 1.0f) {
    capped.fetch_add(1, std::memory_order_relaxed);
    return 1.0f;
  }
  // Preserve all finite in-range/quiet values, including signed zero.
  return value;
}
std::uint64_t WINAPI Limited(void* self, float left, float right) noexcept {
  const auto sender = original.load(std::memory_order_acquire);
  if (!active.load(std::memory_order_acquire)) return sender(self, left, right);
  calls.fetch_add(1, std::memory_order_relaxed);
  left = Bound(left);
  right = Bound(right);
  const DWORD incoming = GetLastError();
  std::uint64_t result;
  DWORD outgoing;
  {
    const MotorSenderScope amplitudes(left, right);
    // MinGW's first access to thread-local storage can change LastError.
    SetLastError(incoming);
    result = sender(self, left, right);
    outgoing = GetLastError();
  }
  SetLastError(outgoing);
  return result;
}
bool Match(std::uintptr_t address, const unsigned char* expected, std::size_t length) noexcept {
  if (!address || length > motor_profile::sender_bytes.size() ||
      address > std::numeric_limits<std::uintptr_t>::max() - length) return false;
  std::array<unsigned char, motor_profile::sender_bytes.size()> bytes{};
  SIZE_T count = 0;
  return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(address), bytes.data(), length, &count) &&
         count == length && std::memcmp(bytes.data(), expected, length) == 0;
}
DWORD Fail(DWORD value) noexcept { error.store(value); return value; }
#ifdef ED_CROSSOVER_COMPAT
using Frame = void (WINAPI*)(void*);
std::atomic<Frame> native_frame{nullptr};
std::uintptr_t installed_frame_slot = 0;
std::uintptr_t pending_frame_slot = 0;
DWORD pending_frame_protection = 0;

template <typename T> bool Read(std::uintptr_t address, T& value) noexcept {
  if (!address || address > std::numeric_limits<std::uintptr_t>::max()-sizeof(value)) return false;
  SIZE_T count = 0;
  return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(address), &value, sizeof(value), &count) &&
         count == sizeof(value);
}
void WINAPI UpdateFrame(void* self) noexcept {
  if (!active.load(std::memory_order_acquire)) {
    native_frame.load(std::memory_order_acquire)(self);
    return;
  }
  const DWORD incoming = GetLastError();
  const auto object = reinterpret_cast<std::uintptr_t>(self);
  int index = -1;
  std::uintptr_t accumulator = 0;
  std::array<float,2> inputs{};
  const bool ready = object && object <= std::numeric_limits<std::uintptr_t>::max()-0x18 &&
                     Read(object+0x18,index) && index != -1 && Read(object+0x10,accumulator) &&
                     accumulator && Read(accumulator,inputs);
  SetLastError(incoming);
  // The verified native FrameUpdate loads these two floats and tailcalls
  // the native converter. The normal termination path supplies zero directly.
  if (ready) Limited(self,inputs[0],inputs[1]);
}
DWORD RetryFrameSlotRollback() noexcept {
  if (!pending_frame_slot) return ERROR_SUCCESS;
  auto* page = reinterpret_cast<void*>(pending_frame_slot);
  DWORD ignored = 0;
  if (!VirtualProtect(page,sizeof(void*),PAGE_READWRITE,&ignored)) {
    const DWORD error = GetLastError();
    return error ? error : ERROR_ACCESS_DENIED;
  }
  InterlockedCompareExchangePointer(reinterpret_cast<void* volatile*>(pending_frame_slot),
                                    reinterpret_cast<void*>(native_frame.load(std::memory_order_acquire)),
                                    reinterpret_cast<void*>(&UpdateFrame));
  if (VirtualProtect(page,sizeof(void*),pending_frame_protection,&ignored)) {
    pending_frame_slot = 0;
    return ERROR_SUCCESS;
  }
  const DWORD error = GetLastError();
  return error ? error : ERROR_ACCESS_DENIED;
}
DWORD QueueFrameSlotRollback(std::uintptr_t address, DWORD protection) noexcept {
  pending_frame_slot = address;
  pending_frame_protection = protection;
  const DWORD result = RetryFrameSlotRollback();
  frame_slot_rollback_error.store(result);
  return result;
}
DWORD InstallFrameSlot(std::uintptr_t address, std::uintptr_t frame, std::uintptr_t sender) noexcept {
  auto* slot = reinterpret_cast<void* volatile*>(address);
  auto* expected = reinterpret_cast<void*>(frame);
  auto* replacement = reinterpret_cast<void*>(&UpdateFrame);
  MEMORY_BASIC_INFORMATION memory{};
  if (!address || address % alignof(void*) || !frame || !sender ||
      !VirtualQuery(reinterpret_cast<void*>(address), &memory, sizeof(memory)) ||
      memory.State != MEM_COMMIT ||
      (memory.Protect != PAGE_READONLY && memory.Protect != PAGE_READWRITE && memory.Protect != PAGE_WRITECOPY))
    return Fail(ERROR_INVALID_ADDRESS);
  DWORD old_protect = 0;
  if (!VirtualProtect(reinterpret_cast<void*>(address), sizeof(void*), PAGE_READWRITE, &old_protect)) {
    const auto failure = GetLastError();
    return Fail(failure ? failure : ERROR_ACCESS_DENIED);
  }
  original.store(reinterpret_cast<Sender>(sender),std::memory_order_release);
  native_frame.store(reinterpret_cast<Frame>(frame),std::memory_order_release);
  active.store(true,std::memory_order_release);
  const bool changed = InterlockedCompareExchangePointer(slot,replacement,expected) == expected;
  DWORD ignored = 0;
  if (!VirtualProtect(reinterpret_cast<void*>(address), sizeof(void*), old_protect, &ignored)) {
    const auto failure = GetLastError();
    active.store(false,std::memory_order_release);
    const DWORD rollback_error = QueueFrameSlotRollback(address,old_protect);
    return Fail(rollback_error ? rollback_error : failure ? failure : ERROR_ACCESS_DENIED);
  }
  if (!changed) {
    active.store(false,std::memory_order_release);
    return Fail(ERROR_ALREADY_EXISTS);
  }
  void* observed = nullptr;
  const bool readable = Read(address,observed);
  if (!readable || observed != replacement) {
    active.store(false,std::memory_order_release);
    if (!readable) QueueFrameSlotRollback(address,old_protect);
    return Fail(readable ? ERROR_ALREADY_EXISTS : ERROR_NOACCESS);
  }
  installed_frame_slot = address;
  frame_slot.store(true,std::memory_order_release);
  error.store(ERROR_SUCCESS);
  installed.store(true,std::memory_order_release);
  return ERROR_SUCCESS;
}
DWORD StartFrameSlot(std::uintptr_t image, std::size_t size) noexcept {
  namespace profile = authored_trigger_profile;
  constexpr auto last = profile::vtable_rva + sizeof(profile::vtable_method_rvas);
  if (size < last || image > std::numeric_limits<std::uintptr_t>::max()-size ||
      !Match(image+profile::frame_update_rva,profile::frame_update.data(),profile::frame_update.size()) ||
      !Match(image+profile::frame_end_rva,profile::frame_end.data(),profile::frame_end.size()) ||
      !Match(image+profile::term_rva,profile::term.data(),profile::term.size())) return Fail(ERROR_REVISION_MISMATCH);
  std::array<std::uintptr_t,profile::vtable_method_rvas.size()> methods{};
  if (!Read(image+profile::vtable_rva,methods)) return Fail(ERROR_NOACCESS);
  for (std::size_t i=0; i<methods.size(); ++i) {
    if (methods[i] != image+profile::vtable_method_rvas[i]) return Fail(ERROR_REVISION_MISMATCH);
  }
  return InstallFrameSlot(image+profile::vtable_rva+6*sizeof(void*),methods[6],image+motor_profile::sender_rva);
}
#endif
DWORD StartAt(std::uintptr_t sender, bool fixture, bool& trampoline_denied) noexcept {
  DWORD expected = 0;
  if (!started.compare_exchange_strong(expected, 1)) return ERROR_ALREADY_INITIALIZED;
  fixture_mode.store(fixture);
  if (!Match(sender, motor_profile::sender_bytes.data(), motor_profile::sender_bytes.size()))
    return Fail(ERROR_REVISION_MISMATCH);
  for (const auto& constant : motor_profile::constants) {
    if (sender > std::numeric_limits<std::uintptr_t>::max() - constant.offset ||
        !Match(sender + constant.offset, constant.bytes.data(), constant.size)) return Fail(ERROR_REVISION_MISMATCH);
  }
  auto status = MH_Initialize();
  minhook_status.store(status);
  if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) return Fail(ERROR_NOT_SUPPORTED);
  void* trampoline = nullptr;
  status = MH_CreateHook(reinterpret_cast<void*>(sender), reinterpret_cast<void*>(&Limited), &trampoline);
  minhook_status.store(status);
  trampoline_denied = status == MH_ERROR_MEMORY_ALLOC;
  if (status != MH_OK) return Fail(ERROR_NOT_SUPPORTED);
  Sender typed = nullptr;
  static_assert(sizeof(typed) == sizeof(trampoline));
  std::memcpy(&typed, &trampoline, sizeof(typed));
  original.store(typed, std::memory_order_release);
  active.store(true, std::memory_order_release);
  status = MH_EnableHook(reinterpret_cast<void*>(sender));
  minhook_status.store(status);
  if (status != MH_OK) {
    // Retain any potentially published trampoline until exit. Do not unhook
    // while a callback could be using it, and never initialize the gain probe.
    active.store(false, std::memory_order_release);
    return Fail(ERROR_NOT_SUPPORTED);
  }
  error.store(ERROR_SUCCESS);
  installed.store(true,std::memory_order_release);
  return ERROR_SUCCESS;
}
}}

namespace edtrace {
DWORD MotorHeadroomStart(std::uintptr_t imageBase, std::size_t size, bool verified) noexcept {
  if (!verified || !imageBase || imageBase > std::numeric_limits<std::uintptr_t>::max() - motor_profile::sender_rva)
    return Fail(ERROR_REVISION_MISMATCH);
  bool trampoline_denied = false;
  const DWORD result = StartAt(imageBase + motor_profile::sender_rva, false,trampoline_denied);
#ifdef ED_CROSSOVER_COMPAT
  if (trampoline_denied) return StartFrameSlot(imageBase,size);
#else
  (void)size;
#endif
  return result;
}
DWORD MotorHeadroomStartControlledFixture(std::uintptr_t sender) noexcept {
  bool trampoline_denied = false;
  return StartAt(sender,true,trampoline_denied);
}
bool MotorHeadroomReady() noexcept {
  if (!installed.load(std::memory_order_acquire) || !active.load(std::memory_order_acquire)) return false;
#ifdef ED_CROSSOVER_COMPAT
  if (frame_slot.load(std::memory_order_acquire)) {
    Frame current = nullptr;
    const bool readable = Read(installed_frame_slot,current);
    if (!readable || current != &UpdateFrame) {
      Fail(readable ? ERROR_ALREADY_EXISTS : ERROR_NOACCESS);
      active.store(false,std::memory_order_release);
      return false;
    }
  }
#endif
  return true;
}
#ifdef ED_CROSSOVER_COMPAT
void MotorHeadroomRetryFrameSlotRollback() noexcept {
  frame_slot_rollback_error.store(RetryFrameSlotRollback());
}
#endif
MotorHeadroomStats MotorHeadroomGetStats() noexcept {
  MotorHeadroomStats stats;
  stats.installation_error = error.load();
  stats.frame_slot_rollback_error = frame_slot_rollback_error.load();
  stats.active = installed.load(std::memory_order_acquire) && active.load(std::memory_order_acquire);
  stats.minhook_status = minhook_status.load();
  stats.frame_slot = frame_slot.load(std::memory_order_acquire);
  stats.controlled_fixture = fixture_mode.load(); stats.calls = calls.load();
  stats.capped_channels = capped.load(); stats.invalid_channels = invalid.load();
  return stats;
}
}  // namespace edtrace
