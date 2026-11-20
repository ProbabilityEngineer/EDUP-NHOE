#pragma once
#include <windows.h>
#include <cstdint>
#include <string>
namespace edtrace {
DWORD NaturalProbeStart(std::uintptr_t base, bool verified, const std::wstring& directory) noexcept;
DWORD NaturalProbeStop(DWORD timeout_ms) noexcept;
// Persistent authored restoration: no diagnostic effect posts, marker enable or
// three-minute timeout. Contextual state gates remain in the native mixer.
DWORD NaturalRestorationStart(std::uintptr_t base, bool verified, const std::wstring& directory,
                              bool (*headroom_ready)() noexcept) noexcept;
struct NaturalRestorationStatus { DWORD error; bool enabled; bool owns_change; };
NaturalRestorationStatus NaturalRestorationGetStatus() noexcept;
}
