#include "edtrace/output_slot_router.hpp"
#include "edtrace/page_protection.hpp"
#include <atomic>
#include <cstring>
#include <limits>

namespace edtrace { namespace {
SRWLOCK lifecycle = SRWLOCK_INIT;
std::uintptr_t import_slot = 0, wwise_slot = 0;
MotorBackend previous_import = nullptr, previous_wwise = nullptr;
MotorBackend native_import = nullptr, native_dynamic = nullptr, replacement = nullptr;
std::atomic<bool> active{false}, available{false};
std::atomic<DWORD> failure{ERROR_NOT_READY};
bool faulted = false;

bool Read(std::uintptr_t slot, MotorBackend& value) noexcept {
  SIZE_T copied = 0;
  return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(slot), &value, sizeof(value), &copied) && copied == sizeof(value);
}
void* Bits(MotorBackend function) noexcept {
  void* value = nullptr; static_assert(sizeof(value) == sizeof(function));
  std::memcpy(&value, &function, sizeof(value)); return value;
}
DWORD Exchange(std::uintptr_t slot, MotorBackend expected, MotorBackend value) noexcept {
  DWORD old = 0;
  if (!VirtualProtect(reinterpret_cast<void*>(slot), sizeof(void*), PAGE_READWRITE, &old)) return GetLastError();
  const auto observed = InterlockedCompareExchangePointer(reinterpret_cast<void* volatile*>(slot), Bits(value), Bits(expected));
  const bool changed = observed == Bits(expected);
  const DWORD restore = changed ? RestorableProtection(old) : old;
  DWORD ignored = 0;
  if (!VirtualProtect(reinterpret_cast<void*>(slot), sizeof(void*), restore, &ignored)) {
    const DWORD error = GetLastError();
    if (changed)
      InterlockedCompareExchangePointer(reinterpret_cast<void* volatile*>(slot), Bits(expected), Bits(value));
    VirtualProtect(reinterpret_cast<void*>(slot), sizeof(void*), restore, &ignored);
    return error ? error : ERROR_ACCESS_DENIED;
  }
  return changed ? ERROR_SUCCESS : ERROR_ALREADY_EXISTS;
}
DWORD RestoreSlot(std::uintptr_t slot, MotorBackend& previous) noexcept {
  if (!previous) return ERROR_SUCCESS;
  MotorBackend current = nullptr;
  if (!Read(slot, current)) return ERROR_NOACCESS;
  if (current == replacement) {
    const DWORD result = Exchange(slot, replacement, previous);
    if (result && result != ERROR_ALREADY_EXISTS) return result;
  }
  previous = nullptr;
  return ERROR_SUCCESS;
}
DWORD RestoreOwnedSlots() noexcept {
  available.store(false, std::memory_order_release);
  faulted = true; // Leave failed restorations retryable without reacquiring the slots.
  const DWORD audio_error = RestoreSlot(wwise_slot, previous_wwise);
  const DWORD import_error = RestoreSlot(import_slot, previous_import);
  const DWORD result = audio_error ? audio_error : import_error;
  active.store(result != ERROR_SUCCESS, std::memory_order_release);
  failure.store(result ? result : ERROR_NOT_READY);
  return result;
}
bool Refresh() noexcept {
  available.store(false, std::memory_order_release);
  if (!active.load(std::memory_order_acquire) || faulted) return false;
  MotorBackend imported = nullptr, dynamic = nullptr;
  if (!Read(import_slot, imported) || !Read(wwise_slot, dynamic)) {
    faulted = true; failure.store(ERROR_NOACCESS); return false;
  }
  if (imported != replacement) {
    faulted = true; failure.store(ERROR_ALREADY_EXISTS); return false;
  }
  // A recognized rebind may change again before our compare-exchange. Retry
  // bounded collisions; a null slot waits, and a foreign writer is preserved.
  for (unsigned attempt = 0;; ++attempt) {
    if (!dynamic) { failure.store(ERROR_NOT_READY); return false; }
    if (dynamic == replacement) {
      available.store(true,std::memory_order_release); failure.store(ERROR_SUCCESS); return true;
    }
    if (dynamic != native_dynamic && dynamic != native_import) {
      faulted = true; failure.store(ERROR_ALREADY_EXISTS); return false;
    }
    if (attempt == 3) { failure.store(ERROR_RETRY); return false; }
    const DWORD error = Exchange(wwise_slot,dynamic,replacement);
    if (error == ERROR_ALREADY_EXISTS) {
      if (!Read(wwise_slot,dynamic)) { faulted = true; failure.store(ERROR_NOACCESS); return false; }
      continue;
    }
    if (error) { faulted = true; failure.store(error); return false; }
    previous_wwise = dynamic;
    available.store(true,std::memory_order_release); failure.store(ERROR_SUCCESS); return true;
  }
}
}}

namespace edtrace {
DWORD OutputSlotsStart(std::uintptr_t image, std::size_t size, bool verified,
                       MotorBackend imported, MotorBackend dynamic, MotorBackend wrapper) noexcept {
  const DWORD incoming = GetLastError();
  if (!verified || !image || image % alignof(void*) || !imported || !dynamic || !wrapper || imported == wrapper || dynamic == wrapper ||
      size < kMotorWwiseSlotRva + sizeof(void*) || image > std::numeric_limits<std::uintptr_t>::max() - size)
    return ERROR_REVISION_MISMATCH;
  AcquireSRWLockExclusive(&lifecycle);
  if (active.load()) { ReleaseSRWLockExclusive(&lifecycle); SetLastError(incoming); return ERROR_ALREADY_INITIALIZED; }
  MotorBackend current_import = nullptr, current_dynamic = nullptr;
  const auto iat = image + kMotorImportSlotRva, audio = image + kMotorWwiseSlotRva;
  DWORD error = ERROR_SUCCESS;
  if (!Read(iat, current_import) || !Read(audio, current_dynamic)) error = ERROR_NOACCESS;
  else if ((current_import != imported && current_import != dynamic) ||
           (current_dynamic && current_dynamic != dynamic && current_dynamic != imported))
    error = ERROR_ALREADY_EXISTS;
  if (!error) {
    import_slot = iat; wwise_slot = audio; native_import = imported; native_dynamic = dynamic; replacement = wrapper;
    previous_import = current_import; previous_wwise = nullptr;
    error = Exchange(import_slot, current_import, replacement);
    if (!error) {
      faulted = false;
      active.store(true, std::memory_order_release);
      Refresh();
      if (faulted) {
        error = failure.load();
        const DWORD rollback = RestoreOwnedSlots();
        if (rollback) error = rollback;
      }
    }
  }
  if (error) failure.store(error);
  ReleaseSRWLockExclusive(&lifecycle); SetLastError(incoming); return error;
}
bool OutputSlotsRefresh() noexcept {
  const DWORD incoming = GetLastError();
  AcquireSRWLockExclusive(&lifecycle); const bool result = Refresh(); ReleaseSRWLockExclusive(&lifecycle);
  SetLastError(incoming); return result;
}
bool OutputSlotsReady() noexcept { return available.load(std::memory_order_acquire); }
DWORD OutputSlotsError() noexcept { return failure.load(std::memory_order_acquire); }
DWORD OutputSlotsStop() noexcept {
  const DWORD incoming = GetLastError();
  AcquireSRWLockExclusive(&lifecycle);
  const DWORD result = active.load(std::memory_order_acquire) ? RestoreOwnedSlots() : ERROR_SUCCESS;
  ReleaseSRWLockExclusive(&lifecycle);
  SetLastError(incoming);
  return result;
}
}
