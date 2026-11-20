#include "edtrace/motor_headroom.hpp"
#include "edtrace/motor_sender_profile.hpp"
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
std::atomic<bool> active{false}, fixture_mode{false};
std::atomic<DWORD> started{0}, error{ERROR_NOT_READY};
std::atomic<std::uint64_t> calls{0}, capped{0}, invalid{0};
std::atomic<int> mh_stage{0}, mh_status{0};
std::atomic<DWORD> target_protect{0}, detour_protect{0}, target_state{0}, detour_state{0};
std::atomic<std::uintptr_t> target_address{0}, detour_address{0}, trampoline_address{0};
void Query(const void* address, std::atomic<DWORD>& protect, std::atomic<DWORD>& state) noexcept {
  MEMORY_BASIC_INFORMATION info{};
  if (VirtualQuery(address, &info, sizeof(info)) == sizeof(info)) { protect.store(info.Protect); state.store(info.State); }
}
MotorHeadroomStats::AllocationProbe probe{};
std::atomic<bool> probe_ready{false};
// Mirrors MinHook's GetMemoryBlock search (buffer.c) without keeping memory.
void ProbeAllocation(std::uintptr_t origin) noexcept {
  constexpr std::uintptr_t kRange = 0x40000000, kBlock = 0x1000;
  auto& p = probe;
  SYSTEM_INFO si{}; GetSystemInfo(&si);
  p.granularity = si.dwAllocationGranularity ? si.dwAllocationGranularity : 0x10000;
  p.app_min = reinterpret_cast<std::uintptr_t>(si.lpMinimumApplicationAddress);
  p.app_max = reinterpret_cast<std::uintptr_t>(si.lpMaximumApplicationAddress);
  p.range_min = p.app_min; p.range_max = p.app_max;
  if (origin > kRange && p.range_min < origin - kRange) p.range_min = origin - kRange;
  if (p.range_max > origin + kRange) p.range_max = origin + kRange;
  std::uintptr_t best = 0, best_distance = ~std::uintptr_t{0};
  for (std::uintptr_t address = p.range_min - p.range_min % p.granularity; address < p.range_max;) {
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(reinterpret_cast<void*>(address), &info, sizeof(info)) != sizeof(info) || !info.RegionSize) break;
    const auto base = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
    const auto end = base + info.RegionSize;
    if (info.State == MEM_FREE) {
      ++p.free_regions; p.free_bytes += info.RegionSize;
      if (info.RegionSize > p.largest_free) p.largest_free = info.RegionSize;
      // Aligned candidates inside this free region, as MinHook would pick.
      std::uintptr_t first = base + p.granularity - 1; first -= first % p.granularity;
      std::uintptr_t last = end > kBlock ? end - kBlock : 0; last -= last % p.granularity;
      if (first <= last && last >= first) {
        const std::uintptr_t candidate = origin <= first ? first : origin >= last ? last : origin - origin % p.granularity;
        const std::uintptr_t distance = candidate > origin ? candidate - origin : origin - candidate;
        if (distance < best_distance && candidate >= p.range_min && candidate < p.range_max) { best = candidate; best_distance = distance; }
      }
    } else if (info.State == MEM_RESERVE) ++p.reserved_regions;
    else ++p.committed_regions;
    if (end <= address) break;
    address = end;
  }
  p.probe_address = best;
  if (best) {
    void* block = VirtualAlloc(reinterpret_cast<void*>(best), kBlock, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    p.rwx_error = block ? ERROR_SUCCESS : GetLastError();
    if (block) VirtualFree(block, 0, MEM_RELEASE);
    block = VirtualAlloc(reinterpret_cast<void*>(best), kBlock, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    p.rw_error = block ? ERROR_SUCCESS : GetLastError();
    if (block) {
      DWORD old = 0;
      p.rw_to_rx_error = VirtualProtect(block, kBlock, PAGE_EXECUTE_READ, &old) ? ERROR_SUCCESS : GetLastError();
      p.rw_to_rwx_error = VirtualProtect(block, kBlock, PAGE_EXECUTE_READWRITE, &old) ? ERROR_SUCCESS : GetLastError();
      VirtualFree(block, 0, MEM_RELEASE);
    }
  }
  void* anywhere = VirtualAlloc(nullptr, kBlock, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
  p.rwx_anywhere_error = anywhere ? ERROR_SUCCESS : GetLastError();
  p.rwx_anywhere_address = reinterpret_cast<std::uintptr_t>(anywhere);
  if (anywhere) VirtualFree(anywhere, 0, MEM_RELEASE);
  p.ran = true;
  probe_ready.store(true, std::memory_order_release);
}
DWORD MinHookFail(int stage, MH_STATUS status) noexcept {
  mh_stage.store(stage); mh_status.store(static_cast<int>(status));
  error.store(ERROR_NOT_SUPPORTED); return ERROR_NOT_SUPPORTED;
}

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
DWORD StartAt(std::uintptr_t sender, bool fixture) noexcept {
  DWORD expected = 0;
  if (!started.compare_exchange_strong(expected, 1)) return ERROR_ALREADY_INITIALIZED;
  fixture_mode.store(fixture);
  if (!Match(sender, motor_profile::sender_bytes.data(), motor_profile::sender_bytes.size()))
    return Fail(ERROR_REVISION_MISMATCH);
  for (const auto& constant : motor_profile::constants) {
    if (sender > std::numeric_limits<std::uintptr_t>::max() - constant.offset ||
        !Match(sender + constant.offset, constant.bytes.data(), constant.size)) return Fail(ERROR_REVISION_MISMATCH);
  }
  target_address.store(sender); detour_address.store(reinterpret_cast<std::uintptr_t>(&Limited));
  Query(reinterpret_cast<const void*>(sender), target_protect, target_state);
  Query(reinterpret_cast<const void*>(&Limited), detour_protect, detour_state);
  auto status = MH_Initialize();
  if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) return MinHookFail(1, status);
  void* trampoline = nullptr;
  status = MH_CreateHook(reinterpret_cast<void*>(sender), reinterpret_cast<void*>(&Limited), &trampoline);
  if (status != MH_OK) {
    if (status == MH_ERROR_MEMORY_ALLOC) ProbeAllocation(sender);
    return MinHookFail(2, status);
  }
  trampoline_address.store(reinterpret_cast<std::uintptr_t>(trampoline));
  Sender typed = nullptr;
  static_assert(sizeof(typed) == sizeof(trampoline));
  std::memcpy(&typed, &trampoline, sizeof(typed));
  original.store(typed, std::memory_order_release);
  active.store(true, std::memory_order_release);
  status = MH_EnableHook(reinterpret_cast<void*>(sender));
  if (status != MH_OK) {
    // Retain any potentially published trampoline until exit. Do not unhook
    // while a callback could be using it, and never initialize the gain probe.
    active.store(false, std::memory_order_release);
    return MinHookFail(3, status);
  }
  error.store(ERROR_SUCCESS);
  return ERROR_SUCCESS;
}
}}

namespace edtrace {
DWORD MotorHeadroomStart(std::uintptr_t imageBase, bool verified) noexcept {
  if (!verified || !imageBase || imageBase > std::numeric_limits<std::uintptr_t>::max() - motor_profile::sender_rva)
    return Fail(ERROR_REVISION_MISMATCH);
  return StartAt(imageBase + motor_profile::sender_rva, false);
}
DWORD MotorHeadroomStartControlledFixture(std::uintptr_t sender) noexcept { return StartAt(sender, true); }
MotorHeadroomStats MotorHeadroomGetStats() noexcept {
  MotorHeadroomStats stats;
  stats.installation_error = error.load(); stats.active = active.load(std::memory_order_acquire);
  stats.controlled_fixture = fixture_mode.load(); stats.calls = calls.load();
  stats.capped_channels = capped.load(); stats.invalid_channels = invalid.load();
  stats.mh_stage = mh_stage.load(); stats.mh_status = mh_status.load();
  stats.target_protect = target_protect.load(); stats.detour_protect = detour_protect.load();
  stats.target_state = target_state.load(); stats.detour_state = detour_state.load();
  stats.target = target_address.load(); stats.detour = detour_address.load(); stats.trampoline = trampoline_address.load();
  if (probe_ready.load(std::memory_order_acquire)) stats.alloc = probe;
  return stats;
}
}  // namespace edtrace
