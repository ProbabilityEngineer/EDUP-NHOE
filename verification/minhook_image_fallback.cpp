#include <windows.h>
#include "MinHook.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static bool fail_restore = false;
extern "C" decltype(&VirtualAlloc) __real___imp_VirtualAlloc;
extern "C" decltype(&VirtualProtect) __real___imp_VirtualProtect;

static LPVOID WINAPI RejectNearAllocation(LPVOID address, SIZE_T size, DWORD type, DWORD protection) {
  if (address) {
    SetLastError(ERROR_INVALID_ADDRESS);
    return nullptr;
  }
  return __real___imp_VirtualAlloc(address, size, type, protection);
}

static BOOL WINAPI ProtectImage(LPVOID address, SIZE_T size, DWORD protection, PDWORD old) {
  if (fail_restore && protection == PAGE_EXECUTE_READ) {
    fail_restore = false;
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

int WINAPI Replacement(int value) {
  return original(value) + 20;
}

void CheckImageProtection(const void* buffer) {
  MEMORY_BASIC_INFORMATION memory{};
  Check(VirtualQuery(buffer, &memory, sizeof(memory)) &&
        memory.AllocationBase == GetModuleHandleW(nullptr) && memory.Protect == PAGE_EXECUTE_READ,
        "fallback buffer is in the main executable and read/execute");
}
}

int wmain(int argc, wchar_t** argv) {
  Check(argc == 2, "owned test DLL path supplied");
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
  Check(MH_EnableMainImageFallback() != FALSE, "opt-in validates a code tail in the fixture executable");
  void* buffer = nullptr;
  Check(MH_CreateHook(target, detour, &buffer) == MH_OK && buffer,
        "checked executable target gets a trampoline after near allocation fails");
  CheckImageProtection(buffer);
  Check(MH_GetFallbackSlotsInUse() == 1, "one fallback trampoline is live");
  Check(MH_CreateHook(foreign, detour, nullptr) == MH_ERROR_MEMORY_ALLOC,
        "a target in a different DLL cannot use the main executable fallback");
  original = reinterpret_cast<Operation>(buffer);
  Check(MH_EnableHook(target) == MH_OK && invoke(7) == 32, "the hook calls the original through the finalized buffer");
  Check(MH_RemoveHook(target) == MH_OK && invoke(7) == 12, "removal restores the original target");
  Check(MH_GetFallbackSlotsInUse() == 0 && std::memcmp(buffer, zero.data(), zero.size()) == 0,
        "removal releases the slot and clears its bytes");
  CheckImageProtection(buffer);

  fail_restore = true;
  void* failed = nullptr;
  Check(MH_CreateHook(target, detour, &failed) == MH_ERROR_MEMORY_ALLOC && !failed && !fail_restore,
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
