#include <windows.h>
#include "MinHook.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>

static unsigned image_restore_failures = 0;
static unsigned patch_restore_failures = 0;
static void* patch_restore_failure = nullptr;
static bool allow_near_allocation = false;
extern "C" decltype(&VirtualAlloc) __real___imp_VirtualAlloc;
extern "C" decltype(&VirtualProtect) __real___imp_VirtualProtect;

static LPVOID WINAPI RejectNearAllocation(LPVOID address, SIZE_T size, DWORD type, DWORD protection) {
  if (address && !allow_near_allocation) {
    SetLastError(ERROR_INVALID_ADDRESS);
    return nullptr;
  }
  return __real___imp_VirtualAlloc(address, size, type, protection);
}

static BOOL WINAPI ProtectImage(LPVOID address, SIZE_T size, DWORD protection, PDWORD old) {
  if (image_restore_failures && size == 1 && protection == PAGE_EXECUTE_READ) {
    --image_restore_failures;
    SetLastError(ERROR_ACCESS_DENIED);
    return FALSE;
  }
  if (patch_restore_failure && address == patch_restore_failure && protection != PAGE_EXECUTE_READWRITE) {
    if (--patch_restore_failures == 0)
      patch_restore_failure = nullptr;
    SetLastError(ERROR_ACCESS_DENIED);
    return FALSE;
  }
  return __real___imp_VirtualProtect(address, size, protection, old);
}

extern "C" {
decltype(&VirtualAlloc) __wrap___imp_VirtualAlloc = &RejectNearAllocation;
decltype(&VirtualProtect) __wrap___imp_VirtualProtect = &ProtectImage;
}

namespace {
using Operation = int (WINAPI*)(int);
Operation original = nullptr;
constexpr std::array<unsigned char, 64> zero{};

void Check(bool ok, const char* message) {
  if (ok) return;
  std::fprintf(stderr, "FAIL: %s\n", message);
  std::exit(1);
}

__attribute__((noinline)) int WINAPI Target(int value) {
  volatile int result = value + 5;
  return result;
}

// Only decoded by the fixture. Its original displacement already uses the
// entire negative range, so moving it toward the tail makes it unencodable.
__attribute__((naked)) void RipRelativeTarget() {
  __asm__("lea -2147483648(%rip), %rax\n\tret");
}

__attribute__((naked)) int WINAPI ValidRipRelativeTarget(int) {
  __asm__("mov 1(%rip), %eax\n\tret\n\t.long 19");
}

int WINAPI Replacement(int value) {
  return original(value) + 20;
}

void CheckImageProtection(const void* buffer) {
  MEMORY_BASIC_INFORMATION memory{};
  Check(VirtualQuery(buffer, &memory, sizeof(memory)) &&
        memory.AllocationBase == GetModuleHandleW(nullptr) && memory.Protect == PAGE_EXECUTE_READ,
        "fallback buffer is in the main executable and read/execute");
}

__attribute__((noinline, section(".fixture")))
void CheckPatchClearance(void* buffer, void* target, void* detour) {
  constexpr ULONG_PTR page_size = 4096;
  const auto boundary = reinterpret_cast<ULONG_PTR>(buffer) + zero.size();
  Check((boundary % page_size) <= page_size - 2 * zero.size() && boundary % page_size != 0,
        "fixture needs room for two slots beyond the simulated aligned .text extent");

  const auto module = reinterpret_cast<ULONG_PTR>(GetModuleHandleW(nullptr));
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(module + dos->e_lfanew);
  auto* section = IMAGE_FIRST_SECTION(nt);
  auto* end = section + nt->FileHeader.NumberOfSections;
  while (section != end && std::memcmp(section->Name, ".text\0\0", IMAGE_SIZEOF_SHORT_NAME) != 0)
    ++section;
  Check(section != end, "fixture has a .text extent");

  DWORD saved_protection = 0, ignored = 0;
  auto& extent = section->Misc.VirtualSize;
  const auto original_extent = extent;
  Check(VirtualProtect(&extent, sizeof(extent), PAGE_READWRITE, &saved_protection),
        "fixture can update its own section header");
  extent = static_cast<DWORD>(boundary - module - section->VirtualAddress);
  Check(VirtualProtect(&extent, sizeof(extent), saved_protection, &ignored), "fixture header is protected");

  void* created = nullptr;
  Check(MH_Initialize() == MH_OK && MH_EnableMainImageFallback() &&
        MH_CreateHook(target, detour, &created) == MH_OK, "aligned extent supports a fallback slot");
  Check(reinterpret_cast<ULONG_PTR>(created) >= boundary + 4,
        "slot stays outside the bytes reached by a patch at the end of .text");
  Check(MH_Uninitialize() == MH_OK, "patch clearance fixture releases the slot");

  Check(VirtualProtect(&extent, sizeof(extent), PAGE_READWRITE, &ignored), "fixture can restore its section header");
  extent = original_extent;
  Check(VirtualProtect(&extent, sizeof(extent), saved_protection, &ignored), "restored fixture header is protected");
}

__attribute__((noinline, section(".fixture")))
void CheckNearAllocationRecovery(void* buffer, void* target, void* detour) {
  constexpr ULONG_PTR page_size = 4096;
  const auto padding = page_size - reinterpret_cast<ULONG_PTR>(buffer) % page_size;
  DWORD saved_protection = 0, ignored = 0;
  Check(VirtualProtect(buffer, padding, PAGE_EXECUTE_READWRITE, &saved_protection),
        "fixture can mark its unused tail unavailable");
  std::memset(buffer, 0xcc, padding);
  Check(VirtualProtect(buffer, padding, saved_protection, &ignored), "fixture tail is protected");

  allow_near_allocation = true;
  void* created = nullptr;
  MEMORY_BASIC_INFORMATION memory{};
  Check(MH_CreateHook(target, detour, &created) == MH_OK && created &&
        VirtualQuery(created, &memory, sizeof(memory)) && memory.Type == MEM_PRIVATE,
        "a previously failed nearby allocation is retried when the tail is unavailable");
  Check(MH_Uninitialize() == MH_OK && MH_GetFallbackSlotsInUse() == 0,
        "ordinary allocation releases cleanly");
  Check(VirtualProtect(buffer, padding, PAGE_EXECUTE_READWRITE, &ignored), "fixture can clear its tail");
  std::memset(buffer, 0, padding);
  Check(VirtualProtect(buffer, padding, saved_protection, &ignored), "cleared fixture tail is protected");
}

__attribute__((section(".fixture"))) DWORD WINAPI OwnedThread(LPVOID) {
  return 0;
}

__attribute__((noinline, section(".fixture")))
void CheckSuspendedThread(void* target, void* detour) {
  const auto second = reinterpret_cast<void*>(&ValidRipRelativeTarget);
  void* trampoline = nullptr;
  Check(MH_CreateHook(second, detour, &trampoline) == MH_OK, "second fixture target has a trampoline");
  HANDLE thread = CreateThread(nullptr, 0, &OwnedThread, nullptr, CREATE_SUSPENDED, nullptr);
  Check(thread != nullptr, "owned fixture thread starts suspended");
  CONTEXT saved{};
  saved.ContextFlags = CONTEXT_CONTROL;
  Check(GetThreadContext(thread, &saved), "fixture can preserve the thread's startup context");
  CONTEXT context = saved;
  context.Rip = reinterpret_cast<DWORD64>(second);
  Check(SetThreadContext(thread, &context), "fixture parks its thread at the second target");

  const auto check_position = [&](const void* expected, const char* message) {
    Check(GetThreadContext(thread, &context) && context.Rip == reinterpret_cast<DWORD64>(expected), message);
  };
  for (bool queued : {false, true}) {
    if (queued)
      Check(MH_QueueEnableHook(MH_ALL_HOOKS) == MH_OK, "both hooks can be queued");
    patch_restore_failure = target;
    patch_restore_failures = 1;
    Check((queued ? MH_ApplyQueued() : MH_EnableHook(MH_ALL_HOOKS)) == MH_ERROR_MEMORY_PROTECT,
          "a failed patch aborts the batch");
    check_position(second, "an unattempted hook leaves its suspended caller at the original target");
    Check((queued ? MH_ApplyQueued() : MH_EnableHook(MH_ALL_HOOKS)) == MH_OK, "the batch can be retried");
    check_position(trampoline, "a committed hook moves its suspended caller into the trampoline");
    if (queued)
      Check(MH_QueueDisableHook(MH_ALL_HOOKS) == MH_OK, "both hooks can be queued for disable");
    Check((queued ? MH_ApplyQueued() : MH_DisableHook(MH_ALL_HOOKS)) == MH_OK, "both hooks disable");
    check_position(second, "disabled hooks return suspended callers to their original instructions");
  }
  Check(SetThreadContext(thread, &saved) && ResumeThread(thread) == 1 &&
        WaitForSingleObject(thread, INFINITE) == WAIT_OBJECT_0, "owned thread exits through its saved startup context");
  Check(CloseHandle(thread), "owned thread handle is released");
  Check(MH_Uninitialize() == MH_OK && MH_GetFallbackSlotsInUse() == 0, "thread fixture releases its hooks");
}
}

int wmain(int argc, wchar_t** argv) {
  Check(argc == 2 || argc == 3, "owned test DLL path supplied");
  const wchar_t* mode = argc == 3 ? argv[2] : L"baseline";
  const auto library = LoadLibraryExW(argv[1], nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
  Check(library != nullptr, "owned secondary DLL loads");
  const auto foreign = reinterpret_cast<void*>(GetProcAddress(library, "XInputSetState"));
  Check(foreign != nullptr, "owned secondary DLL exposes a callable target");
  const auto target = reinterpret_cast<void*>(&Target);
  const auto detour = reinterpret_cast<void*>(&Replacement);
  Operation volatile invoke = &Target;

  Check(MH_Initialize() == MH_OK, "MinHook initializes");
  Check(MH_CreateHook(target, detour, nullptr) == MH_ERROR_MEMORY_ALLOC,
        "near allocation failure uses the ordinary error without opt-in");
  Check(MH_EnableMainImageFallback() != FALSE,
        "fixture .text needs at least one aligned fallback slot in its final page");
  void* buffer = nullptr;
  Check(MH_CreateHook(target, detour, &buffer) == MH_OK && buffer,
        "checked executable target gets a trampoline after near allocation fails");
  CheckImageProtection(buffer);
  Check(MH_GetFallbackSlotsInUse() == 1, "one fallback trampoline is live");
  Check(MH_CreateHook(foreign, detour, nullptr) == MH_ERROR_MEMORY_ALLOC,
        "a target in a different DLL cannot use the main executable fallback");
  original = reinterpret_cast<Operation>(buffer);
  if (std::wcscmp(mode, L"thread-context") == 0) {
    CheckSuspendedThread(target, detour);
    return 0;
  }
  if (std::wcscmp(mode, L"patch-restore") == 0) {
    patch_restore_failure = target;
    patch_restore_failures = 1;
    Check(MH_EnableHook(target) == MH_ERROR_MEMORY_PROTECT && !patch_restore_failure,
          "enabling reports a failed protection restore");
    Check(invoke(7) == 12, "failed enable leaves the original function callable");
    Check(MH_EnableHook(target) == MH_OK && invoke(7) == 32, "retry enables the hook");
    CheckImageProtection(target);
    patch_restore_failure = target;
    patch_restore_failures = 1;
    Check(MH_DisableHook(target) == MH_ERROR_MEMORY_PROTECT && !patch_restore_failure,
          "disabling reports a failed protection restore");
    Check(invoke(7) == 12, "failed protection restore during disable leaves original code callable");
    Check(MH_RemoveHook(target) == MH_OK, "removal completes protection recovery");
    CheckImageProtection(target);
    void* recreated = nullptr;
    Check(MH_CreateHook(target, detour, &recreated) == MH_OK, "fixture can recreate a disabled hook");
    patch_restore_failure = target;
    patch_restore_failures = 3;
    Check(MH_EnableHook(target) == MH_ERROR_MEMORY_PROTECT && patch_restore_failures == 1 &&
          invoke(7) == 12, "persistent protection failure leaves original instructions callable");
    Check(MH_RemoveHook(target) == MH_ERROR_MEMORY_PROTECT && !patch_restore_failure &&
          MH_GetFallbackSlotsInUse() == 1, "removal retains its slot while protection recovery fails");
    Check(MH_RemoveHook(target) == MH_OK && MH_GetFallbackSlotsInUse() == 0,
          "removal retries the saved protection after the failure clears");
    CheckImageProtection(target);
    Check(MH_Uninitialize() == MH_OK, "patch fixture shuts down");
    return 0;
  }
  if (std::wcscmp(mode, L"remove-retry") == 0) {
    image_restore_failures = 1;
    Check(MH_RemoveHook(target) == MH_ERROR_MEMORY_PROTECT && image_restore_failures == 0,
          "slot protection failure requires removal retry");
    Check(MH_EnableHook(target) == MH_ERROR_MEMORY_PROTECT && invoke(7) == 12,
          "direct enable cannot reactivate a hook being removed");
    Check(MH_EnableHook(MH_ALL_HOOKS) == MH_ERROR_MEMORY_PROTECT,
          "bulk enable cannot reactivate a hook being removed");
    Check(MH_QueueEnableHook(target) == MH_ERROR_MEMORY_PROTECT,
          "queued enable cannot reactivate a hook being removed");
    Check(MH_QueueEnableHook(MH_ALL_HOOKS) == MH_ERROR_MEMORY_PROTECT,
          "bulk queued enable cannot reactivate a hook being removed");
    Check(MH_RemoveHook(target) == MH_OK && MH_GetFallbackSlotsInUse() == 0,
          "removal retry clears the slot");
    CheckImageProtection(buffer);
    Check(MH_Uninitialize() == MH_OK, "removal fixture shuts down");
    return 0;
  }
  if (std::wcscmp(mode, L"rip-range") == 0) {
    const auto rip = reinterpret_cast<void*>(&RipRelativeTarget);
    Check(reinterpret_cast<ULONG_PTR>(rip) < reinterpret_cast<ULONG_PTR>(buffer),
          "fixture relocates the instruction toward the tail");
    void* relocated = nullptr;
    Check(MH_CreateHook(rip, detour, &relocated) == MH_ERROR_UNSUPPORTED_FUNCTION && !relocated,
          "out-of-range RIP-relative instruction is rejected");
    const auto valid = reinterpret_cast<void*>(&ValidRipRelativeTarget);
    Operation volatile invoke_valid = &ValidRipRelativeTarget;
    Check(MH_CreateHook(valid, detour, &relocated) == MH_OK && relocated,
          "an in-range RIP-relative load remains supported");
    original = reinterpret_cast<Operation>(relocated);
    Check(MH_EnableHook(valid) == MH_OK && invoke_valid(0) == 39,
          "the relocated load reads its original value through the hook");
    Check(MH_RemoveHook(valid) == MH_OK && invoke_valid(0) == 19,
          "removal restores the RIP-relative function");
    Check(MH_Uninitialize() == MH_OK, "relocation fixture shuts down");
    return 0;
  }
  Check(MH_EnableHook(target) == MH_OK && invoke(7) == 32, "the hook calls the original through the finalized buffer");
  Check(MH_RemoveHook(target) == MH_OK && invoke(7) == 12, "removal restores the original target");
  Check(MH_GetFallbackSlotsInUse() == 0 && std::memcmp(buffer, zero.data(), zero.size()) == 0,
        "removal releases the slot and clears its bytes");
  CheckImageProtection(buffer);

  if (std::wcscmp(mode, L"patch-clearance") == 0) {
    Check(MH_Uninitialize() == MH_OK, "fixture can rediscover its tail with a changed extent");
    CheckPatchClearance(buffer, target, detour);
    return 0;
  }
  if (std::wcscmp(mode, L"near-retry") == 0) {
    CheckNearAllocationRecovery(buffer, target, detour);
    return 0;
  }
  if (std::wcscmp(mode, L"create-retry") == 0) {
    image_restore_failures = 2;
    void* failed = nullptr;
    Check(MH_CreateHook(target, detour, &failed) == MH_ERROR_MEMORY_PROTECT && !failed &&
          MH_GetFallbackSlotsInUse() == 1, "failed rollback uses an occupied, unpublished slot");
    void* recovered = nullptr;
    Check(MH_CreateHook(target, detour, &recovered) == MH_OK && recovered == buffer &&
          MH_GetFallbackSlotsInUse() == 1, "next creation recovers and reuses the unpublished slot");
    Check(MH_Uninitialize() == MH_OK, "creation retry fixture shuts down");
    return 0;
  }

  Check(std::wcscmp(mode, L"baseline") == 0, "known verification mode");
  image_restore_failures = 1;
  void* failed = nullptr;
  Check(MH_CreateHook(target, detour, &failed) == MH_ERROR_MEMORY_ALLOC && !failed && !image_restore_failures,
        "failed permission restore never exposes a trampoline");
  Check(MH_GetFallbackSlotsInUse() == 0 && std::memcmp(buffer, zero.data(), zero.size()) == 0,
        "creation rollback clears the rejected slot");
  CheckImageProtection(buffer);

  void* reused = nullptr;
  Check(MH_CreateHook(target, detour, &reused) == MH_OK && reused == buffer,
        "a released zero slot is reusable");
  Check(MH_Uninitialize() == MH_OK && MH_GetFallbackSlotsInUse() == 0 &&
        std::memcmp(buffer, zero.data(), zero.size()) == 0, "uninitialization clears remaining slots");
  CheckImageProtection(buffer);
  Check(MH_Initialize() == MH_OK && MH_CreateHook(target, detour, nullptr) == MH_ERROR_MEMORY_ALLOC &&
        MH_Uninitialize() == MH_OK, "a new session requires its own opt-in");
  Check(FreeLibrary(library) != FALSE, "owned secondary DLL is released");
  return 0;
}
