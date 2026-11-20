// Regression fixture for restoring protection after a slot write on an image
// .data page. Such a page reports PAGE_WRITECOPY until first written. The
// game keeps writable globals on the page that holds its audio motor slot, so
// after the slot exchange those globals must stay writable.
//
// Usage: page_protection_verification.exe [readonly] <result-file>
//   (default) restore via RestorableProtection: the later write must succeed.
//   readonly  restore PAGE_READONLY (the broken mapping): the write must fault,
//             proving this fixture reproduces the field crash.
#include "edtrace/page_protection.hpp"
#include "result_file.hpp"

#include <cstdio>
#include <string>
#include <cwchar>

namespace {
// A whole page of initialised, never-written image data in its own section.
__attribute__((section(".edpgt"), aligned(4096), used))
volatile unsigned char g_page[4096] = {1};
volatile LONG g_faults = 0;

LONG CALLBACK CountWriteFault(EXCEPTION_POINTERS* info) {
  const auto* record = info->ExceptionRecord;
  if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2 &&
      record->ExceptionInformation[0] == 1 &&
      record->ExceptionInformation[1] == reinterpret_cast<ULONG_PTR>(&g_page[0x98])) {
    InterlockedIncrement(&g_faults);
    DWORD ignored = 0;  // let the faulting write complete so the fixture can report
    VirtualProtect(const_cast<unsigned char*>(&g_page[0]), sizeof(g_page), PAGE_READWRITE, &ignored);
    return EXCEPTION_CONTINUE_EXECUTION;
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

int Finish(bool passed, const char* message) {
  std::printf("%s %s\n", passed ? "PASS" : "FAIL", message);
  VerificationResult(passed ? std::string("PASS") : std::string("FAIL: ") + message);
  return passed ? 0 : 1;
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
  const bool broken = argc == 3 && std::wcscmp(argv[1], L"readonly") == 0;
  if (argc >= 2) verification_result_path = argv[argc - 1];

  MEMORY_BASIC_INFORMATION before{};
  auto* page = const_cast<unsigned char*>(&g_page[0]);
  if (VirtualQuery(page, &before, sizeof(before)) != sizeof(before))
    return Finish(false, "fixture page is queryable");
  std::printf("initial protection 0x%lx\n", before.Protect);
  if ((before.Protect & 0xFF) != PAGE_WRITECOPY)
    return Finish(false, "fixture page starts copy-on-write, like the game's .data");

  // The slot exchange: writable, store, restore.
  DWORD old = 0, ignored = 0;
  auto* slot = reinterpret_cast<void* volatile*>(page + 0x30);
  if (!VirtualProtect(page + 0x30, sizeof(void*), PAGE_READWRITE, &old))
    return Finish(false, "slot page becomes writable");
  InterlockedExchangePointer(slot, reinterpret_cast<void*>(&CountWriteFault));
  const DWORD restore = broken ? PAGE_READONLY : edtrace::RestorableProtection(old);
  if (!VirtualProtect(page + 0x30, sizeof(void*), restore, &ignored))
    return Finish(false, "slot page protection restores");

  MEMORY_BASIC_INFORMATION after{};
  VirtualQuery(page, &after, sizeof(after));
  std::printf("restored protection 0x%lx\n", after.Protect);
  if (*slot != reinterpret_cast<void*>(&CountWriteFault))
    return Finish(false, "the slot keeps the written pointer after restore");

  // The program's own later write to a neighbouring global on the same page.
  void* handler = AddVectoredExceptionHandler(1, &CountWriteFault);
  g_page[0x98] = 7;
  RemoveVectoredExceptionHandler(handler);
  std::printf("neighbour write faults: %ld\n", g_faults);

  if (broken)
    return Finish(g_faults == 1, "read-only restore faults the neighbouring write (reproduces the crash)");
  // Wine keeps reporting image pages as PAGE_WRITECOPY after a read-write
  // protect; what matters is that the page stays writable and keeps the slot.
  const DWORD access = after.Protect & 0xFF;
  return Finish(g_faults == 0 && (access == PAGE_READWRITE || access == PAGE_WRITECOPY) &&
                    *slot == reinterpret_cast<void*>(&CountWriteFault) && g_page[0x98] == 7,
                "copy-on-write restore keeps the page writable, the slot and the neighbouring write");
}
