#include "edtrace/authored_trigger_profile.hpp"
#include "edtrace/motor_headroom.hpp"
#include "edtrace/motor_sender_profile.hpp"
#include "MinHook.h"
#include <xinput.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

static bool fail_initialization = false;
static bool fail_first_rollback = false;
static const void* fail_readback_at = nullptr;
static const void* fail_protection_at = nullptr;
static std::uintptr_t* writecopy_slot = nullptr;
static std::uintptr_t competing_callback = 0;
static DWORD writecopy_restore = 0;
static bool opened_writecopy = false;
extern "C" decltype(&ReadProcessMemory) __real___imp_ReadProcessMemory;
extern "C" decltype(&VirtualProtect) __real___imp_VirtualProtect;
static BOOL WINAPI ReadFixtureMemory(HANDLE process, const void* address, void* buffer,
                                     SIZE_T size, SIZE_T* read) {
  if (address == fail_readback_at && size == sizeof(void*)) {
    fail_readback_at = nullptr;
    if (fail_first_rollback) fail_protection_at = address;
    if (read) *read = 0;
    SetLastError(ERROR_PARTIAL_COPY);
    return FALSE;
  }
  return __real___imp_ReadProcessMemory(process,address,buffer,size,read);
}
static BOOL WINAPI ProtectFixtureMemory(void* address, SIZE_T size, DWORD protection, DWORD* old) {
  if (address == writecopy_slot) {
    if (!opened_writecopy) {
      if (!__real___imp_VirtualProtect(address,size,protection,old)) return FALSE;
      opened_writecopy = true;
      *old = PAGE_WRITECOPY;
      if (competing_callback) *writecopy_slot = competing_callback;
      return TRUE;
    }
    // Model the copy-on-write attribute at the Win32 boundary. The fixture
    // table itself lives in private memory.
    writecopy_restore = protection;
    if (protection == PAGE_WRITECOPY) protection = PAGE_READWRITE;
  }
  if (address == fail_protection_at && protection == PAGE_READWRITE) {
    fail_protection_at = nullptr;
    SetLastError(ERROR_ACCESS_DENIED);
    return FALSE;
  }
  return __real___imp_VirtualProtect(address,size,protection,old);
}
extern "C" {
decltype(&ReadProcessMemory) __wrap___imp_ReadProcessMemory = &ReadFixtureMemory;
decltype(&VirtualProtect) __wrap___imp_VirtualProtect = &ProtectFixtureMemory;
}
extern "C" MH_STATUS WINAPI MH_Initialize() { return fail_initialization ? MH_ERROR_MEMORY_ALLOC : MH_OK; }
extern "C" MH_STATUS WINAPI MH_CreateHook(void*, void*, void**) {
  if (fail_initialization) std::abort();
  return MH_ERROR_MEMORY_ALLOC;
}
extern "C" MH_STATUS WINAPI MH_EnableHook(void*) { std::abort(); }

namespace {
namespace profile = edtrace::authored_trigger_profile;
namespace motor = edtrace::motor_profile;
using Frame = void (WINAPI*)(void*);
using Output = DWORD (WINAPI*)(DWORD, XINPUT_VIBRATION*);
constexpr auto output_rva = motor::sender_rva + motor::output_pointer_offset;
struct Sink {
  std::uintptr_t methods;
  unsigned no_data_frames, channels;
  float* amplitudes;
  int index;
};
static_assert(offsetof(Sink, amplitudes) == 0x10 && offsetof(Sink, index) == 0x18);
XINPUT_VIBRATION received{};
edtrace::MotorSenderScope::Amplitudes scoped{};
DWORD received_index = 99, incoming_error = 0;
unsigned outputs = 0;

void Check(bool condition, const char* name) {
  if (!condition) { std::fprintf(stderr, "FAIL %s\n", name); std::exit(1); }
}
DWORD WINAPI ObserveOutput(DWORD index, XINPUT_VIBRATION* request) {
  incoming_error = GetLastError();
  received_index = index; received = *request; ++outputs;
  scoped = edtrace::MotorSenderScope::Current();
  SetLastError(0x2222);
  return ERROR_SUCCESS;
}
void Copy(BYTE* image, std::uintptr_t rva, const void* data, std::size_t size) {
  Check(VirtualAlloc(image+rva,size,MEM_COMMIT,PAGE_READWRITE)!=nullptr,"owned image page commits");
  std::memcpy(image+rva,data,size);
}
void Protect(BYTE* image, std::uintptr_t rva, std::size_t size, DWORD protection) {
  DWORD old = 0;
  Check(VirtualProtect(image+rva,size,protection,&old)!=FALSE,"owned image page protection changes");
}
}

int main(int argc, char** argv) {
  constexpr auto size = output_rva + sizeof(void*);
  auto* image = static_cast<BYTE*>(VirtualAlloc(nullptr,size,MEM_RESERVE,PAGE_NOACCESS));
  Check(image!=nullptr,"owned image is reserved");
  const auto base = reinterpret_cast<std::uintptr_t>(image);
  Copy(image,motor::sender_rva,motor::sender_bytes.data(),motor::sender_bytes.size());
  Copy(image,profile::frame_update_rva,profile::frame_update.data(),profile::frame_update.size());
  Copy(image,profile::frame_end_rva,profile::frame_end.data(),profile::frame_end.size());
  Copy(image,profile::term_rva,profile::term.data(),profile::term.size());
  for (const auto& constant : motor::constants)
    Copy(image,motor::sender_rva+constant.offset,constant.bytes.data(),constant.size);
  const Output output = &ObserveOutput;
  Copy(image,output_rva,&output,sizeof(output));
  auto methods = profile::vtable_method_rvas;
  for (auto& method : methods) method += base;
  const bool foreign = argc==2 && std::strcmp(argv[1],"foreign")==0;
  const bool writecopy_conflict = argc==2 && std::strcmp(argv[1],"writecopy-conflict")==0;
  const bool writecopy = writecopy_conflict || (argc==2 && std::strcmp(argv[1],"writecopy")==0);
  fail_first_rollback = argc==2 && std::strcmp(argv[1],"readback-retry")==0;
  const bool fail_readback = fail_first_rollback || (argc==2 && std::strcmp(argv[1],"readback")==0);
  fail_initialization = argc==2 && std::strcmp(argv[1],"initialize")==0;
  if (foreign) methods[6] = methods[2];
  Copy(image,profile::vtable_rva,methods.data(),sizeof(methods));
  Protect(image,motor::sender_rva,0x1000-(motor::sender_rva&0xfff),PAGE_EXECUTE_READ);
  Protect(image,profile::vtable_rva,sizeof(methods),PAGE_READONLY);

  if (writecopy) writecopy_slot = reinterpret_cast<std::uintptr_t*>(image+profile::vtable_rva)+6;
  if (writecopy_conflict) competing_callback = methods[2];
  if (fail_readback) fail_readback_at = image+profile::vtable_rva+6*sizeof(void*);
  const auto result = edtrace::MotorHeadroomStart(base,size,true);
  const auto stats = edtrace::MotorHeadroomGetStats();
  auto* live = reinterpret_cast<std::uintptr_t*>(image+profile::vtable_rva);
  if (writecopy_conflict) {
    Check(result==ERROR_ALREADY_EXISTS&&!stats.active&&live[6]==competing_callback&&
          writecopy_restore==PAGE_WRITECOPY,"a losing swap preserves the foreign callback and original copy protection");
    return 0;
  }
  if (writecopy) {
    Check(result==ERROR_SUCCESS&&stats.frame_slot&&live[6]!=methods[6]&&writecopy_restore==PAGE_READWRITE,
          "a winning swap preserves the private table's writability");
    return 0;
  }
  if (fail_readback) {
    MEMORY_BASIC_INFORMATION page{};
    Check(result==ERROR_NOACCESS&&!edtrace::MotorHeadroomReady()&&!fail_readback_at,
          "failed slot readback blocks gain");
    if (fail_first_rollback) {
      Check(stats.frame_slot_rollback_error==ERROR_ACCESS_DENIED&&live[6]!=methods[6]&&!fail_protection_at,
            "a denied rollback leaves a forwarding callback pending cleanup");
      edtrace::MotorHeadroomRetryFrameSlotRollback();
    }
    Check(std::memcmp(live,methods.data(),sizeof(methods))==0&&
          VirtualQuery(live,&page,sizeof(page))==sizeof(page)&&page.Protect==PAGE_READONLY&&
          edtrace::MotorHeadroomGetStats().frame_slot_rollback_error==ERROR_SUCCESS,
          "failed slot readback restores the original callback and table protection");
    return 0;
  }
  if (fail_initialization) {
    Check(result==ERROR_NOT_SUPPORTED&&!edtrace::MotorHeadroomReady()&&
          std::memcmp(live,methods.data(),sizeof(methods))==0,"only a create-hook allocation failure activates the fallback");
    return 0;
  }
  if (foreign) {
    Check(result==ERROR_REVISION_MISMATCH&&!stats.active&&!stats.frame_slot,"foreign frame slot is rejected");
    Check(std::memcmp(live,methods.data(),sizeof(methods))==0,"rejected profile keeps every original callback");
    return 0;
  }
  Check(result==ERROR_SUCCESS&&edtrace::MotorHeadroomReady()&&stats.frame_slot&&stats.minhook_status==9,
        "the cap starts when trampoline allocation is denied");
  Check(std::memcmp(live,methods.data(),6*sizeof(std::uintptr_t))==0&&live[6]!=methods[6],
        "only the frame-update callback changes");
  MEMORY_BASIC_INFORMATION page{};
  Check(VirtualQuery(live,&page,sizeof(page))==sizeof(page)&&page.Protect==PAGE_READONLY,
        "the callback table regains its read-only protection");
  Check(std::memcmp(image+motor::sender_rva,motor::sender_bytes.data(),motor::sender_bytes.size())==0,
        "the original converter code remains intact");

  std::array<float,2> amplitudes{{2.0f,0.0f}};
  Sink sink{base+profile::vtable_rva,0,2,amplitudes.data(),1};
  const auto frame_end = reinterpret_cast<Frame>(base+profile::frame_end_rva);
  SetLastError(0x1111);
  frame_end(&sink);
  Check(received_index==1&&received.wLeftMotorSpeed==65535&&received.wRightMotorSpeed==0&&
        scoped.left==1.0f&&scoped.right==0.0f&&incoming_error==0x1111&&GetLastError()==0x2222,
        "native frame dispatch caps the overrange motor without changing device or LastError");
  Check(amplitudes[0]==0&&amplitudes[1]==0,"native frame end still clears its accumulators");
  amplitudes = {{std::numeric_limits<float>::quiet_NaN(),3.0f}};
  frame_end(&sink);
  Check(received.wLeftMotorSpeed==0&&received.wRightMotorSpeed==65535&&outputs==2,
        "a nonfinite amplitude stays quiet while the other motor is capped");
  sink.index = -1;
  reinterpret_cast<Frame>(live[6])(&sink);
  Check(outputs==2,"disabled sink sends no vibration");

  const auto saved_callback = reinterpret_cast<Frame>(live[6]);
  Protect(image,profile::vtable_rva,sizeof(methods),PAGE_READWRITE);
  live[6] = methods[6];
  Protect(image,profile::vtable_rva,sizeof(methods),PAGE_READONLY);
  Check(!edtrace::MotorHeadroomReady(),"a replaced callback no longer permits authored gain");
  amplitudes = {{1.0f,0.0f}};
  sink.index = 1;
  saved_callback(&sink);
  Check(outputs==3&&received.wLeftMotorSpeed==65535&&scoped.left==0.0f,
        "an already-fetched callback forwards to the native frame after losing ownership");
  std::puts("PASS native frame dispatch and guarded motor cap with MinHook allocation denied");
}
