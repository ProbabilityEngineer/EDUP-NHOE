#pragma once
#include "edtrace/haptic_frame.hpp"
#include <windows.h>
#include <cstdint>
#include <cwchar>
#include <string>
namespace edtrace {
struct MacOutputPreference {
  const char* name;
  bool native;
  bool gameinput;
};
inline MacOutputPreference ParseMacOutputPreference(const wchar_t* setting) noexcept {
  if (_wcsicmp(setting, L"xinput") == 0) return {"xinput", false, false};
  if (_wcsicmp(setting, L"macos") == 0) return {"macos", true, false};
  return {"auto", true, true};
}
struct MacOutputStats {
  DWORD error=ERROR_NOT_READY,launch_error=ERROR_NOT_READY,client_error=0;
  bool listening=false,launched=false,active=false,blocks_fallback=false,acquiring=false;
  unsigned controllers=0,motors=0,port=0;
  std::uint64_t polls=0,frames=0,generation=0;
  std::uint64_t trigger_frames=0; // frames sent to the companion with a non-zero trigger channel
};
DWORD MacOutputStart(const std::wstring& directory,const std::wstring& status_directory,bool enabled) noexcept;
// Worker thread only. Audio callbacks update a bounded latest frame, never I/O.
void MacOutputPoll() noexcept;
// Called under the output mixer's lock before choosing GameInput/XInput.
bool MacOutputSelect(bool allowed) noexcept;
bool MacOutputBlocksFallback() noexcept;
bool MacOutputSubmit(HapticFrame frame) noexcept;
MacOutputStats MacOutputGetStats() noexcept;
void MacOutputStop() noexcept;
#ifdef ED_MAC_OUTPUT_FIXTURE
DWORD MacOutputStartControlled(std::uint64_t (*clock)() noexcept) noexcept;
DWORD MacOutputLaunchControlled(const std::wstring& opener,const std::wstring& directory) noexcept;
#endif
} // namespace edtrace
