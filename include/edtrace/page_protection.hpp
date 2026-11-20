#pragma once
#include <windows.h>

namespace edtrace {
// Protection to put back after writing to a page. Never restore a copy-on-write
// protection: the write has just made the page private, and putting
// PAGE_WRITECOPY / PAGE_EXECUTE_WRITECOPY back can drop that private copy and
// bring back the image's original contents, silently undoing the write.
//
// Copy-on-write pages are WRITABLE (an image's .data section reports
// PAGE_WRITECOPY until first written), so the substitute must keep write
// access: PAGE_READWRITE / PAGE_EXECUTE_READWRITE. Mapping them to a read-only
// protection makes the program's own later writes to that page fault (seen in
// Elite build 334925: the audio motor slot shares a page with .data globals).
// Modifier bits (PAGE_GUARD, PAGE_NOCACHE, PAGE_WRITECOMBINE) are kept.
inline DWORD RestorableProtection(DWORD protection) noexcept {
  const DWORD modifiers = protection & ~DWORD{0xFF};
  switch (protection & 0xFF) {
    case PAGE_WRITECOPY: return PAGE_READWRITE | modifiers;
    case PAGE_EXECUTE_WRITECOPY: return PAGE_EXECUTE_READWRITE | modifiers;
    default: return protection;
  }
}
}  // namespace edtrace
