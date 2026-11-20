#ifdef ED_CROSSOVER_COMPAT
#include <winsock2.h>
#include "edtrace/mac_output.hpp"
#include "edtrace/mac_haptics.hpp"
#endif
#include "edtrace/xinput_output.hpp"
#include "edtrace/motor_headroom.hpp"
#include "edtrace/onfoot_runtime.hpp"
#ifdef ED_CROSSOVER_COMPAT
#include "edtrace/output_slot_router.hpp"
#endif
#include "mock_api.hpp"
#include "result_file.hpp"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <limits>

#ifdef ED_CROSSOVER_COMPAT
static void* rebind_race_slot = nullptr;
static void* rebind_race_value = nullptr;
static std::array<void*,3> rebind_race_values{};
static unsigned rebind_race_count = 0, rebind_race_index = 0;
extern "C" decltype(&VirtualProtect) __real___imp_VirtualProtect;
static BOOL WINAPI ProtectFixture(void* address, SIZE_T size, DWORD protection, DWORD* old) {
  const auto result = __real___imp_VirtualProtect(address,size,protection,old);
  if (result && address == rebind_race_slot && protection == PAGE_READWRITE) {
    *static_cast<void**>(address) = rebind_race_count ? rebind_race_values[rebind_race_index++] : rebind_race_value;
    if (!rebind_race_count || rebind_race_index == rebind_race_count) rebind_race_slot = nullptr;
  }
  return result;
}
extern "C" { decltype(&VirtualProtect) __wrap___imp_VirtualProtect = &ProtectFixture; }
#endif

namespace {
#ifdef ED_CROSSOVER_COMPAT
std::uint64_t native_instant=100;
std::uint64_t NativeClock() noexcept { return native_instant; }
#endif
void Check(bool ok, const char* message) {
  if (!ok) { VerificationResult(std::string("FAIL: ") + message); std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
template <typename T> T Symbol(HMODULE module, const char* name) {
  const auto raw = GetProcAddress(module, name); T value{}; std::memcpy(&value, &raw, sizeof(value)); Check(value != nullptr, name); return value;
}
using Set = DWORD (WINAPI*)(DWORD, XINPUT_VIBRATION*);
using Get = DWORD (WINAPI*)(DWORD, XINPUT_STATE*);
using Caps = DWORD (WINAPI*)(DWORD, DWORD, XINPUT_CAPABILITIES*);
#ifndef ED_CROSSOVER_COMPAT
Set Stub(BYTE* image, std::uintptr_t return_rva, Set target) {
  // A real executable caller with the literal verified return RVA; no mock of
  // the internal classifier. The only external dependency is the DLL backend.
  BYTE* code = image + return_rva - 16;
  const BYTE prologue[]{0x48, 0x83, 0xec, 0x28, 0x48, 0xb8};
  std::memcpy(code, prologue, sizeof(prologue)); std::memcpy(code + 6, &target, 8);
  const BYTE tail[]{0xff, 0xd0, 0x48, 0x83, 0xc4, 0x28, 0xc3};
  std::memcpy(code + 14, tail, sizeof(tail));
  FlushInstructionCache(GetCurrentProcess(), code, 21);
  Set function{}; std::memcpy(&function, &code, sizeof(function)); return function;
}
#endif
#ifdef ED_CROSSOVER_COMPAT
Set SlotStub(BYTE* image, std::uintptr_t return_rva, void* slot) {
  BYTE* code = image + return_rva - 16;
  const BYTE prologue[]{0x48,0x83,0xec,0x28,0x48,0xb8};
  std::memcpy(code,prologue,sizeof(prologue)); std::memcpy(code+6,&slot,8);
  const BYTE tail[]{0xff,0x10,0x48,0x83,0xc4,0x28,0xc3};
  std::memcpy(code+14,tail,sizeof(tail)); FlushInstructionCache(GetCurrentProcess(),code,21);
  Set function{}; std::memcpy(&function,&code,sizeof(function)); return function;
}
#endif
}
int wmain(int argc, wchar_t** argv) {
  if (argc == 4) verification_result_path = argv[3];
  Check(argc == 4, "backend, mode and result file required");
  HMODULE library = LoadLibraryW(argv[1]); Check(library != nullptr, "controlled backend loads");
  const auto set = Symbol<Set>(library, "XInputSetState");
  const auto get = Symbol<Get>(library, "XInputGetState");
  const auto caps = Symbol<Caps>(library, "XInputGetCapabilities");
  const auto reset = Symbol<MockResetFn>(library, "MockReset");
  const auto read = Symbol<MockReadFn>(library, "MockRead");
  constexpr std::size_t size =
#ifdef ED_CROSSOVER_COMPAT
    0x6058000;
#else
    0x4d10000;
#endif
  auto* image = static_cast<BYTE*>(VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
  Check(image != nullptr, "owned caller image allocated");
#ifdef ED_CROSSOVER_COMPAT
  auto* import_slot = reinterpret_cast<Set*>(image+edtrace::kMotorImportSlotRva);
  auto* audio_slot = reinterpret_cast<Set*>(image+edtrace::kMotorWwiseSlotRva);
  const auto alternate = Symbol<Set>(library,"MockAlternateSet");
  *import_slot = set; *audio_slot = nullptr;
  const auto engine = SlotStub(image,0x5c8535,import_slot);
  const auto wwise = SlotStub(image,0x4d05312,audio_slot);
  const auto retry = SlotStub(image,0x4d05395,audio_slot);
  std::array<BYTE,32> set_before{}; std::memcpy(set_before.data(),reinterpret_cast<void*>(set),set_before.size());
  const auto base = reinterpret_cast<std::uintptr_t>(image);
  const bool alias_at_start = std::wcscmp(argv[2], L"alias") == 0;
  Check(edtrace::OutputSlotsStart(base,edtrace::kMotorWwiseSlotRva+7,true,set,alternate,engine)==ERROR_REVISION_MISMATCH,
        "truncated image cannot install pointer route");
  Check(edtrace::OutputSlotsStart(base+1,size,true,set,alternate,engine)==ERROR_REVISION_MISMATCH,
        "unaligned image cannot install atomic pointer route");
  *audio_slot=reinterpret_cast<Set>(static_cast<std::uintptr_t>(0x12345678));
  Check(edtrace::OutputSlotsStart(base,size,true,set,alternate,engine)==ERROR_ALREADY_EXISTS&&*import_slot==set,
        "foreign audio binding rejects installation without changing import");
  const auto unrelated_export = Symbol<Set>(library, "XInputGetState");
  *import_slot=unrelated_export; *audio_slot=unrelated_export;
  Check(edtrace::OutputSlotsStart(base,size,true,set,alternate,retry)==ERROR_ALREADY_EXISTS&&
        *import_slot==unrelated_export&&*audio_slot==unrelated_export,
        "another export from the same DLL does not identify the motor function");
  *audio_slot=nullptr;
  Check(edtrace::OutputSlotsStart(base,size,true,set,alternate,retry)==ERROR_ALREADY_EXISTS&&
        *import_slot==unrelated_export&&!*audio_slot,
        "an unknown game import is rejected before the audio slot initializes");
  *import_slot=alias_at_start ? alternate : set;
  *audio_slot=alias_at_start ? set : nullptr;
#else
  const auto engine = Stub(image, 0x5c8535, set);
  const auto wwise = Stub(image, 0x4d05312, set);
  const auto retry = Stub(image, 0x4d05395, set);
#endif
  std::array<BYTE, 32> get_before{}, caps_before{};
  std::memcpy(get_before.data(), reinterpret_cast<void*>(get), get_before.size());
  std::memcpy(caps_before.data(), reinterpret_cast<void*>(caps), caps_before.size());
  const bool verified = std::wcscmp(argv[2], L"unknown") != 0;
  const DWORD installed = edtrace::XInputOutputStart(set, reinterpret_cast<std::uintptr_t>(image), size, verified
#ifdef ED_CROSSOVER_COMPAT
    ,alternate
#endif
    );
  Check(installed == (verified ? ERROR_SUCCESS : ERROR_REVISION_MISMATCH), "only verified profiles install motor hook");
#ifdef ED_CROSSOVER_COMPAT
  Check(!std::memcmp(set_before.data(),reinterpret_cast<void*>(set),set_before.size()),"CrossOver never detours native XInput code");
  if(verified) {
    Check(edtrace::XInputOutputRefresh()==alias_at_start,"only an initialized audio binding is ready");
    edtrace::XInputOutputSetReady(true,true);
    Check(edtrace::XInputOutputGetStats().ready==alias_at_start,"a verified import alias supports startup without a lookup-address match");
  } else Check(*import_slot==set&&!*audio_slot,"unknown profile changes neither output slot");
  if (!alias_at_start) *audio_slot=alternate;
  if(verified) Check(edtrace::XInputOutputRefresh(),"late audio binding supports a distinct interop thunk");
  if (verified) {
    *audio_slot = alternate; rebind_race_slot = audio_slot; rebind_race_value = reinterpret_cast<void*>(set);
    Check(edtrace::XInputOutputRefresh(),"a recognized concurrent rebind remains guarded");
    *audio_slot = alternate; rebind_race_slot = audio_slot; rebind_race_value = nullptr;
    Check(!edtrace::XInputOutputRefresh() && !*audio_slot,"a concurrent null binding is a retryable quiet wait");
    *audio_slot = set;
    Check(edtrace::XInputOutputRefresh(),"a recognized binding recovers after a concurrent clear");
  }
#endif
  Check(!std::memcmp(get_before.data(), reinterpret_cast<void*>(get), get_before.size()) &&
        !std::memcmp(caps_before.data(), reinterpret_cast<void*>(caps), caps_before.size()), "detection and input entry points remain unchanged");
  reset();
  XINPUT_STATE state{}; SetLastError(0x31415926);
  Check(get(3, &state) == 0x10203040 && GetLastError() == 0x11223344 && state.dwPacketNumber == 0x13579bdf &&
        state.Gamepad.sThumbLX == -1234 && state.Gamepad.sThumbRY == 4567, "original input state, return and LastError preserved");
  XINPUT_CAPABILITIES capability{}; SetLastError(0x12344321);
  Check(caps(2, 0x3333, &capability) == 0x20304050 && GetLastError() == 0x22334455 && capability.Flags == 0x1357 &&
        capability.Vibration.wRightMotorSpeed == 0xabcd, "original detection capabilities preserved");
  XINPUT_VIBRATION pulse{12345, 54321}, zero{};
  MockSnapshot snapshot{};
  edtrace::XInputOutputSetReady(true, true);
  SetLastError(0x10293847);
  Check(wwise(1, &pulse) == 0 && GetLastError() == 0x88776655, "Wwise backend result preserved");
  read(&snapshot);
  Check(snapshot.index == (verified ? 0U : 1U) && snapshot.left == 12345 && snapshot.right == 54321 &&
        snapshot.pointer == reinterpret_cast<ULONG_PTR>(&pulse) && snapshot.incoming_error == 0x10293847,
        "only verified Wwise output routes to zero with original pointer and words");
  if (std::wcscmp(argv[2],L"shutdown") == 0) {
    edtrace::XInputOutputSetReady(false,true,true);
    Check(wwise(1,&pulse) == ERROR_SUCCESS,"an unready authored writer can still reach its original slot");
    read(&snapshot);
    Check(snapshot.index == 1 && snapshot.left == 12345 && snapshot.right == 54321,
          "the fixture has an active authored command on the original slot");
    Check(engine(1,&pulse) == ERROR_SUCCESS,"an unrelated writer replaces the original authored command");
    read(&snapshot); const auto unrelated_calls = snapshot.set_calls;
    edtrace::XInputOutputBeginShutdown();
    read(&snapshot);
    Check(snapshot.set_calls == unrelated_calls,"shutdown never cancels the latest unrelated slot-1 writer");
    edtrace::XInputOutputEndShutdown();
    Check(wwise(1,&pulse) == ERROR_SUCCESS,"inactive forwarding resumes after successful shutdown");
    read(&snapshot);
    Check(snapshot.left == 12345 && snapshot.right == 54321,"ending the mute restores original Wwise forwarding");
    const auto fail_stops = Symbol<void (WINAPI*)(DWORD)>(library,"MockFailStops");
    fail_stops(0x80000002);
    edtrace::XInputOutputBeginShutdown();
    SetLastError(0xabcdef01);
    Check(wwise(1,reinterpret_cast<XINPUT_VIBRATION*>(1)) == ERROR_BUSY && GetLastError() == 0xabcdef01,
          "a refused late stop preserves caller LastError despite an unreadable request");
    read(&snapshot); const auto refused_calls = snapshot.set_calls;
    SetLastError(0x12345678);
    Check(edtrace::XInputOutputServiceShutdown() == ERROR_SUCCESS && GetLastError() == 0x12345678,
          "shutdown retries the zero to acceptance without changing caller LastError");
    read(&snapshot);
    Check(snapshot.set_calls == refused_calls + 1 && snapshot.index == 1 && snapshot.left == 0 && snapshot.right == 0,
          "the retry sends a fresh zero to the backend rather than only reporting success");
    edtrace::XInputOutputEndShutdown();
    Check(wwise(1,&pulse) == ERROR_SUCCESS,"authored slot active for the pending-stop takeover");
    fail_stops(1); edtrace::XInputOutputBeginShutdown();
    Check(engine(1,&pulse) == ERROR_SUCCESS,"an observed writer takes over before the refused stop is retried");
    read(&snapshot); const auto takeover_calls = snapshot.set_calls;
    Check(wwise(1,&pulse) == ERROR_SUCCESS && retry(1,&pulse) == ERROR_SUCCESS &&
          edtrace::XInputOutputServiceShutdown() == ERROR_SUCCESS,"late writes and retry service respect takeover");
    read(&snapshot);
    Check(snapshot.set_calls == takeover_calls && snapshot.left == 12345 && snapshot.right == 54321,
          "no backend stop is injected after an observed takeover of a pending authored command");
    edtrace::XInputOutputEndShutdown();
    Check(wwise(1,&pulse) == ERROR_SUCCESS,"authored output resumes for the final quieting check");
    edtrace::XInputOutputBeginShutdown();
    edtrace::XInputOutputSetReady(false,false,false);
    read(&snapshot);
    Check(snapshot.index == 1 && snapshot.left == 0 && snapshot.right == 0,
          "shutdown clears an authored command already sent to the original slot");
    Check(wwise(1,&pulse) == ERROR_SUCCESS && retry(1,&pulse) == ERROR_SUCCESS,
          "late shared and termination writes are accepted as quiet commands");
    read(&snapshot);
    Check(snapshot.index == 1 && snapshot.left == 0 && snapshot.right == 0,
          "Wwise cannot revive its original slot while native stops are pending");
    Check(set(1,&pulse) == ERROR_SUCCESS,"an unrelated caller retains its command");
    read(&snapshot);
    Check(snapshot.index == 1 && snapshot.left == 12345 && snapshot.right == 54321,
          "shutdown muting preserves unrelated callers");
    Check(wwise(1,&pulse) == ERROR_SUCCESS && retry(1,&pulse) == ERROR_SUCCESS,
          "late muted requests are discarded after an unrelated writer takes over");
    read(&snapshot);
    Check(snapshot.index == 1 && snapshot.left == 12345 && snapshot.right == 54321,
          "late muted Wwise writes preserve another observed writer's slot-1 command");
    Check(wwise(4,&pulse) == ERROR_SUCCESS,"other authored indices retain forwarding");
    read(&snapshot);
    Check(snapshot.index == 4 && snapshot.left == 12345 && snapshot.right == 54321,
          "shutdown muting is limited to the verified route's two slots");
    Check(wwise(1,&zero) == ERROR_SUCCESS,"the owned test slot ends quiet");
#ifdef ED_CROSSOVER_COMPAT
    *audio_slot = alternate; rebind_race_slot = audio_slot;
    rebind_race_values = {reinterpret_cast<void*>(set),reinterpret_cast<void*>(alternate),reinterpret_cast<void*>(unrelated_export)};
    rebind_race_count = 3; rebind_race_index = 0;
    Check(!edtrace::XInputOutputRefresh() && edtrace::OutputSlotsError() == ERROR_ALREADY_EXISTS && *audio_slot == unrelated_export,
          "a foreign binding observed on the final collision is preserved and latched");
    *audio_slot = alternate;
    Check(!edtrace::XInputOutputRefresh(),"a known binding cannot erase the observed foreign-writer fault");
    Check(edtrace::OutputSlotsStop() == ERROR_SUCCESS,"owned CrossOver bindings restore after the shutdown test");
#endif
    Check(VerificationResult("PASS"),"shutdown result file written");
    std::puts("PASS scoped authored shutdown mute");
    return 0;
  }
  const auto calls = snapshot.set_calls;
  SetLastError(0x55554444); Check(engine(0, &zero) == 0, "engine stop returns success");
  if (verified) Check(GetLastError() == 0x55554444, "suppressed engine stop preserves incoming LastError");
  read(&snapshot); Check(snapshot.set_calls == calls + (verified ? 0 : 1), "fresh Wwise effect survives competing engine zero");
  Check(wwise(1, &zero) == 0, "authored Wwise stop accepted");
  read(&snapshot); Check(snapshot.left == 0 && snapshot.right == 0 && snapshot.set_calls == calls + (verified ? 1 : 2), "authored zero is forwarded");
  Check(engine(0, &zero) == 0, "engine stop after authored stop forwarded");
  read(&snapshot); Check(snapshot.set_calls == calls + (verified ? 2 : 3), "authored stop releases ownership");
  if (verified) {
    {
      // A positive main-motor pair recorded after gain restoration reached
      // the original converter as two zero WORDs. Exercise the real router.
      const edtrace::MotorSenderScope observed(0.00253476342f, 0.0494424514f);
      SetLastError(0x25253535);
      Check(wwise(1, &zero) == 0 && GetLastError() == 0x88776655, "recovered quiet frame preserves native result and LastError");
      read(&snapshot);
      Check(snapshot.index == 0 && snapshot.left == 294 && snapshot.right == 10935 &&
            snapshot.incoming_error == 0x25253535 && zero.wLeftMotorSpeed == 0 && zero.wRightMotorSpeed == 0,
            "captured quiet frame reaches both motors without modifying the original request");
      const auto quiet_writes = snapshot.set_calls;
      Check(engine(0, &zero) == 0, "engine zero during a quiet authored pulse accepted"); read(&snapshot);
      Check(snapshot.set_calls == quiet_writes, "recovered quiet frame owns the same short lease");
      Check(retry(1, &zero) == 0, "native termination retry still sends zero"); read(&snapshot);
      Check(snapshot.left == 0 && snapshot.right == 0, "quiet-frame scope does not alter a termination retry");
    }
    {
      const edtrace::MotorSenderScope boundary(0.04999999701976776f, 0.04999999701976776f);
      Check(wwise(1, &zero) == 0, "last float below the converter cutoff accepted"); read(&snapshot);
      Check(snapshot.left == 5816 && snapshot.right == 11059, "quiet extension joins the native cutoff without a motor-word step");
    }
    {
      const edtrace::MotorSenderScope boundary(0.05000000074505806f, 0.05000000074505806f);
      XINPUT_VIBRATION native_words{5816, 11059};
      Check(wwise(1, &native_words) == 0, "first float above the converter cutoff accepted"); read(&snapshot);
      Check(snapshot.left == 5816 && snapshot.right == 11059 &&
            snapshot.pointer == reinterpret_cast<ULONG_PTR>(&native_words), "native calibrated words and pointer pass through above the cutoff");
    }
    {
      const edtrace::MotorSenderScope invalid(std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN());
      Check(wwise(1, &zero) == 0, "nonfinite source frame accepted as silence"); read(&snapshot);
      Check(snapshot.left == 0 && snapshot.right == 0, "nonfinite inputs cannot create vibration");
    }
    {
      const edtrace::MotorSenderScope stopped(0, 0);
      Check(wwise(1, &zero) == 0, "native zero stop accepted after quiet frame"); read(&snapshot);
      Check(snapshot.left == 0 && snapshot.right == 0 && snapshot.pointer == reinterpret_cast<ULONG_PTR>(&zero),
            "explicit native zero reaches the backend without substitution");
    }
    {
      const edtrace::MotorSenderScope unowned(.025f, .025f);
      Check(wwise(4, &zero) == 0, "unowned slot forwards the original request"); read(&snapshot);
      Check(snapshot.index == 4 && snapshot.left == 0 && snapshot.right == 0 && snapshot.pointer == reinterpret_cast<ULONG_PTR>(&zero),
            "quiet extension applies only to the owned route");
    }
    Check(wwise(1, &pulse) == 0, "authored frame before supplemental test");
    edtrace::XInputOutputSetOnFootFrame({.5f,.25f,.25f,.5f},true); read(&snapshot);
    Check(snapshot.left == 32767 && snapshot.right == 54321 && snapshot.index == 0,
          "supplemental main motors mix by maximum with exact authored words");
    const auto mixed_calls = snapshot.set_calls;
    Check(engine(0,&zero) == 0, "engine zero during supplemental pulse accepted"); read(&snapshot);
    Check(snapshot.set_calls == mixed_calls, "fresh supplemental effect survives competing engine zero");
    Check(wwise(1,&zero) == 0, "authored stop during supplemental effect accepted"); read(&snapshot);
    Check(snapshot.left == 32767 && snapshot.right == 16383,
          "authored zero does not erase an independent supplemental pulse");
    edtrace::XInputOutputSetOnFootFrame({},true); read(&snapshot);
    Check(snapshot.left == 0 && snapshot.right == 0, "supplemental expiry restores the authored zero");
    Check(wwise(1,&pulse) == 0, "authored frame before cancellation");
    edtrace::XInputOutputSetOnFootFrame({.5f,0,0,0},true);
    edtrace::XInputOutputSetOnFootFrame({},false); read(&snapshot);
    Check(snapshot.left == 12345 && snapshot.right == 54321,"supplemental disable preserves original authored output");
    edtrace::XInputOutputSetOnFootFrame({.5f,0,0,0},true);
    edtrace::XInputOutputSetReady(false,true,false);
    edtrace::XInputOutputSetOnFootFrame({},false); read(&snapshot);
    Check(snapshot.left == 0 && snapshot.right == 0, "context loss cancels without replaying cached authored motion");
    edtrace::XInputOutputSetReady(true,true,true);
    edtrace::XInputOutputSetOnFootFrame({.25f,0,0,0},true); read(&snapshot);
    Check(snapshot.left == 16383 && snapshot.right == 0, "resumed supplemental effect does not resurrect pre-focus authored motion");
    edtrace::XInputOutputSetOnFootFrame({},true);
    Check(wwise(1,&zero) == 0, "authored frame reset after supplemental test");
    Check(wwise(1,&pulse) == 0,"fallback authored effect before focus loss");
    edtrace::XInputOutputSetReady(false,true,false); read(&snapshot);
    Check(snapshot.index == 0 && snapshot.left == 0 && snapshot.right == 0,
          "focus loss explicitly stops fallback without a supplemental pulse");
    XINPUT_VIBRATION unrelated = pulse;
    Check(wwise(7,&unrelated) == 0x76543210 && GetLastError() == 0x4040 &&
          unrelated.wLeftMotorSpeed == 0xbeef && unrelated.wRightMotorSpeed == 0x1357,
          "background unrelated index preserves native errors and pointer mutation"); read(&snapshot);
    const auto quiet_calls = snapshot.set_calls;
    SetLastError(0x55667788);
    Check(wwise(0,&pulse) == 0 && GetLastError() == 0x55667788,"background authored request is quiet success"); read(&snapshot);
    Check(snapshot.set_calls == quiet_calls,"background authored frames cannot restart fallback vibration");
    // A focused VR session can remain ready while the desktop gamepad is quiet.
    edtrace::XInputOutputSetReady(true,true,false);
    Check(wwise(1,&pulse) == 0,"focused VR output can coexist with quiet desktop fallback"); read(&snapshot);
    Check(snapshot.set_calls == quiet_calls,"VR readiness never enables unfocused XInput fallback");
    unrelated = pulse;
    Check(wwise(7,&unrelated) == 0x76543210 && GetLastError() == 0x4040 &&
          unrelated.wLeftMotorSpeed == 0xbeef && unrelated.wRightMotorSpeed == 0x1357,
          "VR background quieting preserves unrelated index contract");
    edtrace::XInputOutputSetReady(true,true,true);
    Check(wwise(1,&pulse) == 0,"fresh foreground authored request resumes fallback");
    edtrace::XInputOutputSetReady(false,false,false); read(&snapshot);
    Check(snapshot.index == 0 && snapshot.left == 0 && snapshot.right == 0,"shutdown explicitly stops the last owned fallback frame");
    const auto shutdown_calls = snapshot.set_calls;
    Check(wwise(0,&pulse) == 0,"late authored request after shutdown stays quiet"); read(&snapshot);
    Check(snapshot.set_calls == shutdown_calls,"shutdown cannot replay late fallback output");
    edtrace::XInputOutputSetReady(true,true,true);
    XINPUT_VIBRATION travel_native{8000,40000};
    Check(wwise(1,&travel_native) == 0,"authored frame reaches output alongside travel feedback");
    edtrace::TickOnFootFeedback(false,850,{.57f,.45f,0,0}); read(&snapshot);
    Check(snapshot.left == 37354 && snapshot.right == 40000,"SCO works with Odyssey additions off and preserves a stronger authored motor");
    edtrace::TickOnFootFeedback(false,860); read(&snapshot);
    Check(snapshot.left == 8000 && snapshot.right == 40000,"ending the SCO pulse restores the current authored frame");
    Check(wwise(1,&zero) == 0,"authored stop after travel pulse");
    edtrace::StartOnFootFeedback(.5f,true,.5f);
    edtrace::TickOnFootFeedback(true,900);
    edtrace::QueueOnFootInput({edtrace::OnFootInputKind::Register,1100,1000,0,0,0,1,0});
    edtrace::QueueOnFootInput({edtrace::OnFootInputKind::Parameter,1100,1000,1497268741,0,0,1,1});
    edtrace::QueueOnFootInput({edtrace::OnFootInputKind::Parameter,1100,1000,103566125,0,0,1,1});
    edtrace::QueueOnFootInput({edtrace::OnFootInputKind::Switch,1100,1000,1979902012,998496889,0,1,0});
    read(&snapshot); const auto queued_calls = snapshot.set_calls;
    edtrace::QueueOnFootInput({edtrace::OnFootInputKind::Post,1100,1000,1697380579,0,1,0,0});
    read(&snapshot); Check(snapshot.set_calls == queued_calls,"audio-side event admission issues no controller writes");
    edtrace::TickOnFootFeedback(true,1000); read(&snapshot);
    Check(snapshot.left == 3932 && snapshot.right == 5898,"queued local rifle onset reaches native fallback at configured strength");
    edtrace::TickOnFootFeedback(true,1100); read(&snapshot);
    Check(snapshot.left == 983 && snapshot.right == 2621,"queued held effect produces its intended sustained fallback");
    edtrace::QueueOnFootInput({edtrace::OnFootInputKind::Post,1100,1100,1119934880,0,1,0,0});
    edtrace::TickOnFootFeedback(true,1100); read(&snapshot);
    Check(snapshot.left == 0 && snapshot.right == 0,"queued native release reaches the physical-output boundary");
    edtrace::QueueOnFootInput({edtrace::OnFootInputKind::Post,1100,1200,3666194424,0,1,0,0});
    edtrace::TickOnFootFeedback(true,1212); read(&snapshot);
    Check(snapshot.left == 19660 && snapshot.right == 13107,"queued jetpack ignition reaches output at configured strength");
    edtrace::TickOnFootFeedback(true,1290); read(&snapshot);
    Check(snapshot.left == 9830 && snapshot.right == 5242,"queued jetpack sustained state reaches output");
    edtrace::QueueOnFootInput({edtrace::OnFootInputKind::Post,1100,1300,3666194431,0,1,0,0});
    edtrace::TickOnFootFeedback(true,1350); read(&snapshot);
    Check(snapshot.left == 4915 && snapshot.right == 2621,"queued jetpack cutoff fades at output");
    edtrace::TickOnFootFeedback(true,1400); read(&snapshot);
    Check(snapshot.left == 0 && snapshot.right == 0,"queued jetpack cutoff ends at output");
    edtrace::QueueOnFootInput({edtrace::OnFootInputKind::Register,1200,1500,0,0,0,1,0});
    edtrace::QueueOnFootInput({edtrace::OnFootInputKind::Parameter,1200,1500,1497268741,0,0,1,1});
    edtrace::QueueOnFootInput({edtrace::OnFootInputKind::Switch,1200,1500,1979902012,931160808,0,1,0});
    edtrace::QueueOnFootInput({edtrace::OnFootInputKind::Post,1200,1500,476288594,0,1,0,0});
    edtrace::TickOnFootFeedback(true,1500); read(&snapshot);
    Check(snapshot.left == 1310 && snapshot.right == 2293,"queued mounted shot reaches fallback without an on-foot inside marker");
    edtrace::TickOnFootFeedback(true,1560); read(&snapshot);
    Check(snapshot.left == 0 && snapshot.right == 0,"queued mounted shot expires without held-fire output");
    edtrace::QueueOnFootInput({edtrace::OnFootInputKind::Post,1200,1600,313889439,0,1,0,0});
    edtrace::TickOnFootFeedback(true,1625); read(&snapshot);
    Check(snapshot.left == 13107 && snapshot.right == 3932,"queued missile launch reaches its ejection peak");
    edtrace::TickOnFootFeedback(true,1820); read(&snapshot);
    Check(snapshot.left == 0 && snapshot.right == 0,"queued missile ejection ends at output");
    edtrace::QueueOnFootInput({edtrace::OnFootInputKind::Register,1400,1840,0,0,0,1,0});
    edtrace::QueueOnFootInput({edtrace::OnFootInputKind::Parameter,1400,1840,1497268741,0,0,1,1});
    edtrace::QueueOnFootInput({edtrace::OnFootInputKind::Parameter,1400,1840,103566125,0,0,1,1});
    edtrace::TickOnFootFeedback(false,1840);
    edtrace::QueueOnFootInput({edtrace::OnFootInputKind::Parameter,1400,1850,2410709172,0,0,1,6});
    edtrace::TickOnFootFeedback(true,1860);
    edtrace::QueueOnFootInput({edtrace::OnFootInputKind::Post,1400,1870,63371630,0,1,0,0});
    edtrace::TickOnFootFeedback(true,1870); read(&snapshot);
    Check(snapshot.left == 0 && snapshot.right == 0,"queued inactive speed cannot enable running after focus regain");
    edtrace::QueueOnFootInput({edtrace::OnFootInputKind::Parameter,1400,1880,2410709172,0,0,1,6});
    edtrace::QueueOnFootInput({edtrace::OnFootInputKind::Post,1400,1880,63371630,0,1,0,0});
    edtrace::TickOnFootFeedback(true,1880); read(&snapshot);
    Check(snapshot.left == 2621 && snapshot.right == 1966,"fresh foreground running metadata restores contact feedback");
    edtrace::TickOnFootFeedback(false,1940);
    edtrace::QueueOnFootInput({edtrace::OnFootInputKind::Post,1100,1950,1697380579,0,1,0,0});
    edtrace::TickOnFootFeedback(true,1960); read(&snapshot);
    Check(snapshot.left == 0 && snapshot.right == 0,"queued background weapon onset cannot replay after focus regain");

    using Input = edtrace::OnFootInput;
    using Kind = edtrace::OnFootInputKind;
    constexpr std::uint64_t hardpoint = 8800;
    const auto begin_ship = [](Input input) {
      input.ship = edtrace::BeginShipWeaponInput(input.kind,input.object,input.id);
      return input;
    };
    const auto ship = [&](Input input) { edtrace::QueueOnFootInput(begin_ship(input)); };
    edtrace::TickOnFootFeedback(false,2000,{},true);
    ship({Kind::Register,hardpoint,2010,0,0,0,1});
    ship({Kind::Parameter,hardpoint,2010,1497268741,0,0,1,1});
    XINPUT_VIBRATION ship_native{2500,7000};
    Check(wwise(1,&ship_native) == 0,"native frame before ship beam cue");
    ship({Kind::Post,hardpoint,2020,4054225129,0,1});
    edtrace::TickOnFootFeedback(false,2020,{},true); read(&snapshot);
    Check(snapshot.left == 4915 && snapshot.right == 8191,"owned medium beam starts on both main motors with on-foot effects off");
    edtrace::TickOnFootFeedback(false,2120,{},true); read(&snapshot);
    Check(snapshot.left == 2949 && snapshot.right == 7000,"beam settles into a sustain and preserves the stronger authored motor");
    const auto unrelated_registration = begin_ship({Kind::Register,9901,2130,0,0,0,1});
    ship({Kind::Post,hardpoint,2140,4054225128,0,1});
    edtrace::TickOnFootFeedback(false,2140,{},true); read(&snapshot);
    Check(snapshot.left == 2949 && snapshot.right == 7000,"alternate LOD preserves the hold with unrelated work in flight");
    edtrace::QueueOnFootInput(unrelated_registration);
    ship({Kind::Post,hardpoint,2160,3037970381,0,1});
    edtrace::TickOnFootFeedback(false,2160,{},true); read(&snapshot);
    Check(snapshot.left == 2500 && snapshot.right == 7000,"beam release from a different audio LOD preserves authored vibration");
    Check(wwise(1,&zero) == 0,"native stop before ship overlap control");

    const auto pending_start = begin_ship({Kind::Post,hardpoint,2200,4054225129,0,1});
    ship({Kind::Post,hardpoint,2210,3037970380,0,1});
    edtrace::TickOnFootFeedback(false,2220,{},true); read(&snapshot);
    Check(snapshot.left == 0 && snapshot.right == 0,"ship output remains quiet while earlier native work is pending");
    edtrace::QueueOnFootInput(pending_start);
    edtrace::TickOnFootFeedback(false,2230,{},true); read(&snapshot);
    Check(snapshot.left == 0 && snapshot.right == 0,"late successful start cannot overtake an overlapping release");

    ship({Kind::Post,hardpoint,2240,4054225129,0,1});
    edtrace::TickOnFootFeedback(false,2240,{},true); read(&snapshot);
    Check(snapshot.left == 4915 && snapshot.right == 8191,"isolated firing resumes after ambiguous callbacks");
    const auto pending_release = begin_ship({Kind::Post,hardpoint,2250,3037970380,0,1});
    edtrace::TickOnFootFeedback(false,2260,{},true); read(&snapshot);
    Check(snapshot.left == 0 && snapshot.right == 0,"in-flight release silences the active hold");
    edtrace::QueueOnFootInput(pending_release);
    edtrace::TickOnFootFeedback(false,2270,{},true); read(&snapshot);
    Check(snapshot.left == 0 && snapshot.right == 0,"completed release keeps the hold quiet");

    ship({Kind::Post,hardpoint,2280,1616298863,0,1});
    edtrace::TickOnFootFeedback(false,2280,{},true); read(&snapshot);
    Check(snapshot.left == 0 && snapshot.right == 0,"multicannon spool is quiet before its shot");
    ship({Kind::Post,hardpoint,2400,1489337830,0,1});
    edtrace::TickOnFootFeedback(false,2400,{},true); read(&snapshot);
    Check(snapshot.left == 9437 && snapshot.right == 7339,"small multicannon delivers a pulse on its shot cue");
    edtrace::TickOnFootFeedback(false,3230,{},true); read(&snapshot);
    Check(snapshot.left == 0 && snapshot.right == 0,"multicannon pulse ends during a pause");
    ship({Kind::Post,hardpoint,3240,1489337830,0,1});
    edtrace::TickOnFootFeedback(false,3240,{},true); read(&snapshot);
    Check(snapshot.left == 9437 && snapshot.right == 7339,"later shots can resume during the same multicannon hold");
    ship({Kind::Post,hardpoint,3260,909378176,0,1});
    ship({Kind::Post,hardpoint,3290,1489337830,0,1});
    edtrace::TickOnFootFeedback(false,3290,{},true); read(&snapshot);
    Check(snapshot.left == 0 && snapshot.right == 0,"multicannon release cancels recoil and rejects late shot markers");

    ship({Kind::Post,hardpoint,3300,3728187784,0,1});
    edtrace::TickOnFootFeedback(false,3300,{},true); read(&snapshot);
    Check(snapshot.left == 3276 && snapshot.right == 10485,"offline-inferred pulse laser delivers the intended sharp impulse");
    edtrace::TickOnFootFeedback(false,3365,{},true); read(&snapshot);
    Check(snapshot.left == 0 && snapshot.right == 0,"inferred pulse laser expires without a release cue");
    ship({Kind::Register,hardpoint,3400,0,0,0,1});
    ship({Kind::Parameter,hardpoint,3410,1497268741,0,0,1,0});
    ship({Kind::Post,hardpoint,3420,3728187784,0,1});
    edtrace::TickOnFootFeedback(false,3420,{},true); read(&snapshot);
    Check(snapshot.left == 0 && snapshot.right == 0,"reused hardpoint with detached ownership produces no recoil");

    edtrace::TickOnFootFeedback(true,3500);
    edtrace::QueueOnFootInput({Kind::Post,1100,3510,1697380579,0,1,0,0});
    edtrace::TickOnFootFeedback(true,3510);
    for (unsigned i = 0; i < 4096; ++i)
      edtrace::QueueOnFootInput({Kind::Post,9000,3511,1697380579,0,1,0,0});
    edtrace::TickOnFootFeedback(true,3511,{},true); read(&snapshot);
    const auto feedback = edtrace::GetOnFootRuntimeStats();
    Check(feedback.faulted && !feedback.active && !feedback.ship_active && feedback.dropped == 2048 && snapshot.left == 0 && snapshot.right == 0,
          "input queue loss disables new effects and sends zero");
    edtrace::StopOnFootFeedback();
  }
  Check(retry(1, &pulse) == 0, "termination retry accepted");
  read(&snapshot); Check(snapshot.index == (verified ? 0U : 1U), "termination retry uses same verified controller");
  Check(set(7, &pulse) == 0x76543210 && GetLastError() == 0x4040 && pulse.wLeftMotorSpeed == 0xbeef && pulse.wRightMotorSpeed == 0x1357,
        "unknown callers retain backend errors and pointer mutations");
  Check(set(0, nullptr) == ERROR_INVALID_PARAMETER && GetLastError() == 0x55aa0000, "null request forwarded unchanged");
  Check(set(0, reinterpret_cast<XINPUT_VIBRATION*>(1)) == ERROR_NOACCESS && GetLastError() == 0x55aa0001, "unreadable request forwarded unchanged");
  edtrace::XInputOutputSetReady(true, false);
  Check(wwise(1, &zero) == 0, "disconnected target call remains native"); read(&snapshot);
  Check(snapshot.index == 1, "disconnect disables remapping without changing detection");
  edtrace::XInputOutputSetReady(false, true);
  Check(wwise(1, &zero) == 0, "disabled restoration remains native"); read(&snapshot);
  Check(snapshot.index == 1, "unverified gain disables remapping");
#ifdef ED_CROSSOVER_COMPAT
  if(verified) {
    namespace mb=edtrace::macbridge;
    Check(edtrace::MacOutputStartControlled(&NativeClock)==0,"native mixer fixture starts owned relay");
    SOCKET client=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);Check(client!=INVALID_SOCKET,"native mixer client created");
    DWORD timeout=500;setsockopt(client,SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<const char*>(&timeout),sizeof(timeout));
    sockaddr_in endpoint{};endpoint.sin_family=AF_INET;endpoint.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    endpoint.sin_port=htons(static_cast<u_short>(edtrace::MacOutputGetStats().port));
    mb::Message poll{};poll.token.fill(0x5a);poll.active=true;poll.controllers=1;poll.motors=15;poll.device_epoch=1;poll.zeroed=true;
    const auto exchange=[&]() {
      ++poll.sequence;const auto packet=mb::Encode(poll);
      Check(sendto(client,reinterpret_cast<const char*>(packet.data()),packet.size(),0,reinterpret_cast<const sockaddr*>(&endpoint),sizeof(endpoint))==static_cast<int>(packet.size()),"native mixer poll sent");
      edtrace::MacOutputPoll();unsigned char bytes[mb::kPacketSize+1]{};
      const int count=recv(client,reinterpret_cast<char*>(bytes),sizeof(bytes),0);mb::Message reply{};
      Check(count>0&&mb::Decode(bytes,static_cast<std::size_t>(count),reply)&&reply.sequence==poll.sequence,"native mixer reply read");
      poll.generation=reply.generation;return reply;
    };
    exchange();edtrace::XInputOutputSetReady(true,true,true,true);
    native_instant=550;exchange();native_instant=600;edtrace::XInputOutputSetReady(true,true,true,true);
    Check(edtrace::MacOutputGetStats().active,"mixer selects ready native route after provider fence");
    reset();
    {
      const edtrace::MotorSenderScope quiet(.025f, .025f);
      Check(wwise(1, &zero) == 0, "quiet frame accepted by native Mac relay");
      const auto native_quiet = exchange(); read(&snapshot);
      Check(native_quiet.active && std::fabs(native_quiet.frame.low - 0.04437324f) < .00002f &&
            std::fabs(native_quiet.frame.high - 0.08436713f) < .00002f && snapshot.set_calls == 0,
            "native Mac relay receives the recovered main channels without duplicating XInput");
    }
    Check(wwise(1, &zero) == 0 && !edtrace::HapticNonzero(exchange().frame), "native Mac relay clears the recovered pulse on an explicit stop");
    reset();edtrace::XInputOutputSetOnFootFrame({.2f,.3f,.4f,.5f},true);const auto native=exchange();read(&snapshot);
    Check(native.active&&std::fabs(native.frame.low-.2f)<.00002f&&std::fabs(native.frame.high-.3f)<.00002f&&native.frame.left==.4f&&native.frame.right==.5f,
          "production mixer delivers body and trigger amplitudes to native relay");
    Check(snapshot.set_calls==0,"selected native route does not duplicate output through XInput");
    edtrace::XInputOutputSetOnFootFrame({},false);Check(!edtrace::HapticNonzero(exchange().frame),"supplemental native stop reaches all four channels");
    edtrace::XInputOutputSetReady(true,true,false,true);Check(!exchange().active,"native route cancels on game focus loss");
    exchange();edtrace::XInputOutputSetReady(true,true,false,true);
    Check(!edtrace::MacOutputBlocksFallback(),"native stop acknowledgement clears mixer handoff");
    reset();edtrace::XInputOutputSetOnFootFrame({.25f,.5f,.7f,.8f},true);read(&snapshot);
    Check(snapshot.set_calls==0,"background focus never enables XInput fallback");
    edtrace::XInputOutputSetReady(true,true,true,false);reset();edtrace::XInputOutputSetOnFootFrame({.25f,.5f,.7f,.8f},true);read(&snapshot);
    Check(!edtrace::MacOutputGetStats().active&&snapshot.set_calls==1&&snapshot.index==0&&snapshot.left==16383&&snapshot.right==32767,
          "ambiguous native selection returns the same main-motor mix through XInput");
    edtrace::XInputOutputSetReady(false,true);closesocket(client);edtrace::MacOutputStop();
    *audio_slot=nullptr;
    Check(!edtrace::XInputOutputRefresh(),"audio shutdown releases slot readiness");
    edtrace::XInputOutputSetReady(true,true);
    Check(!edtrace::XInputOutputGetStats().ready,"audio shutdown cannot keep output ready");
    *audio_slot=set;
    Check(edtrace::XInputOutputRefresh(),"audio reinitialization can use the verified game import");
    if (alias_at_start) {
      edtrace::XInputOutputSetReady(true,true);
      edtrace::XInputOutputRequireScopedWwiseFrames();
      reset();
      XINPUT_VIBRATION authored{12345,54321};
      Check(wwise(1,&authored)==ERROR_SUCCESS,"an uncapped authored request is accepted as silence");
      read(&snapshot);
      Check(snapshot.set_calls==1&&snapshot.left==0&&snapshot.right==0,
            "uncapped motor words never reach the backend during the gain handoff");
      {
        const edtrace::MotorSenderScope capped(1,1);
        Check(wwise(1,&authored)==ERROR_SUCCESS,"a capped request is accepted");
        read(&snapshot);
        Check(snapshot.set_calls==2&&snapshot.left==12345&&snapshot.right==54321,
              "capped motor words still reach the backend");
      }
      Check(wwise(1,reinterpret_cast<XINPUT_VIBRATION*>(std::uintptr_t{1}))==ERROR_SUCCESS,
            "an unreadable unscoped request is accepted as silence");
      read(&snapshot);
      Check(snapshot.set_calls==3&&snapshot.left==0&&snapshot.right==0&&snapshot.pointer!=1,
            "an unreadable request never reaches the backend as the original pointer");
      edtrace::XInputOutputSetReady(false,true);
      DWORD previous_protection = 0, ignored = 0;
      Check(VirtualProtect(audio_slot,sizeof(Set),PAGE_NOACCESS,&previous_protection),"fixture hides the owned audio slot");
      const auto blocked_restore = edtrace::OutputSlotsStop();
      Check(VirtualProtect(audio_slot,sizeof(Set),previous_protection,&ignored),"fixture restores access to the audio slot");
      Check(blocked_restore==ERROR_NOACCESS&&*import_slot==alternate&&*audio_slot!=set,
            "shutdown reports the unreadable slot and restores the independent import");
      Check(VirtualProtect(import_slot,sizeof(Set),PAGE_NOACCESS,&previous_protection),
            "fixture hides the already-restored import slot");
      const auto retried_restore = edtrace::OutputSlotsStop();
      Check(VirtualProtect(import_slot,sizeof(Set),previous_protection,&ignored),
            "fixture restores access to the import slot");
      Check(retried_restore==ERROR_SUCCESS&&*audio_slot==set,
            "shutdown retry restores the audio slot without touching the released import");
    }
    if (!alias_at_start) {
      *audio_slot=unrelated_export;
      Check(!edtrace::XInputOutputRefresh()&&*audio_slot==unrelated_export,
            "foreign audio slot is never overwritten");
      edtrace::XInputOutputSetReady(true,true);
      Check(!edtrace::XInputOutputGetStats().ready,"foreign slot ownership faults output quiet");
      *audio_slot=alternate;
      Check(!edtrace::XInputOutputRefresh()&&*audio_slot==alternate,"faulted slot route never silently reacquires");
    }
    Check(edtrace::OutputSlotsStop()==ERROR_SUCCESS&&!edtrace::OutputSlotsReady()&&
          *import_slot==(alias_at_start ? alternate : set)&&
          *audio_slot==(alias_at_start ? set : alternate),"shutdown restores exact owned bindings and preserves a replaced slot");
  }
  Check(!std::memcmp(set_before.data(),reinterpret_cast<void*>(set),set_before.size()),"all CrossOver output leaves native API body untouched");
#endif
  std::puts("PASS output interception, authored stops, ownership, input/detection preservation and profile/readiness guards");
  Check(VerificationResult("PASS"), "result file written");
  return 0;
}
