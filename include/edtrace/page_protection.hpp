#pragma once
#include <windows.h>

namespace edtrace {
// Protection to put back after writing to a page. Never restore a copy-on-write
// protection: the write has just made the page private, and putting
// PAGE_WRITECOPY / PAGE_EXECUTE_WRITECOPY back can drop that private copy and
// bring back the image's original contents, silently undoing the write. The
// non-copy equivalents keep the page exactly as readable and executable.
// Modifier bits (PAGE_GUARD, PAGE_NOCACHE, PAGE_WRITECOMBINE) are kept.
inline DWORD RestorableProtection(DWORD protection) noexcept {
  const DWORD modifiers = protection & ~DWORD{0xFF};
  switch (protection & 0xFF) {
    case PAGE_WRITECOPY: return PAGE_READONLY | modifiers;
    case PAGE_EXECUTE_WRITECOPY: return PAGE_EXECUTE_READ | modifiers;
    default: return protection;
  }
}
}  // namespace edtrace
