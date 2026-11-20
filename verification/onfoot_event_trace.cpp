#include "edtrace/onfoot_event_trace.hpp"
#include "edtrace/capture_request.hpp"
#include "edtrace/onfoot_runtime.hpp"
#include "result_file.hpp"
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>

namespace {
edtrace::HapticFrame output{};
}
namespace edtrace {
void XInputOutputSetOnFootFrame(HapticFrame frame, bool active) noexcept { output = active ? frame : HapticFrame{}; }
}
namespace {
using U32 = std::uint32_t;
using U64 = std::uint64_t;
using Address = std::uintptr_t;
using Callback = void (WINAPI*)(std::int32_t,void*);
using Post = U32 (WINAPI*)(U32,U64,U32,Callback,void*,const void*,U32);
using Rtpc = std::int32_t (WINAPI*)(U32,float,U64,std::int32_t,std::int32_t,bool);
using EventAction = std::int32_t (WINAPI*)(U32,U32,U64,std::int32_t,U32,U32);
std::atomic<U64> clock_ms{1000};
unsigned consumed = 0, consumed_parameters = 0;
unsigned ship_admissions = 0, shipped_results = 0;
bool attach_forwarded_after_admission = false;
bool change_focus_during_post = false;
bool use_prior_activity = false;
U64 fixture_time = 0;
U64 prior_activity = 0;
U64 Clock() noexcept { return clock_ms.load(); }
void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void VerifySingleUseRequest(const wchar_t* directory) {
  const auto request = std::wstring(directory) + L"\\ed-onfoot-trace.enable";
  constexpr char contents[] = "ed-onfoot-event-trace-v1\n";
  const auto create = [&](DWORD share = FILE_SHARE_READ) {
    const HANDLE file = CreateFileW(request.c_str(), GENERIC_WRITE, share,
                                    nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(file != INVALID_HANDLE_VALUE,"could not create an owned recording request");
    DWORD written = 0;
    const bool stored = WriteFile(file,contents,sizeof(contents)-1,&written,nullptr) &&
                        written == sizeof(contents)-1;
    if (!stored) CloseHandle(file);
    Check(stored,"could not write an owned recording request");
    return file;
  };
  const HANDLE file = create();
  const auto claimed_while_locked = edtrace::ClaimCaptureRequest(request.c_str());
  CloseHandle(file);
  Check(!claimed_while_locked,"recording opened when request could not be consumed");
  const auto first = edtrace::ClaimCaptureRequest(request.c_str());
  Check(first && edtrace::CaptureRequestActive(request.c_str(), first),"fresh recording request did not open");
  for (const DWORD share : {DWORD{0}, DWORD{FILE_SHARE_READ}}) {
    const HANDLE locked = CreateFileW(request.c_str(), GENERIC_WRITE, share, nullptr,
                                     OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(locked != INVALID_HANDLE_VALUE,"could not open the active marker in the fixture");
    const bool still_active = edtrace::CaptureRequestActive(request.c_str(), first);
    CloseHandle(locked);
    Check(still_active,"a temporary sharing conflict cancelled the active recording");
  }
  Check(!edtrace::ClaimCaptureRequest(request.c_str()),"recording rearmed when an old marker survived shutdown");
  Check(DeleteFileW(request.c_str()),"could not cancel the first request");
  const HANDLE replacement = create(0);
  const bool old_request_active = edtrace::CaptureRequestActive(request.c_str(), first);
  CloseHandle(replacement);
  Check(!old_request_active,"a locked replacement marker kept the canceled recording active");
  Check(!edtrace::RetireCaptureRequest(request.c_str(), first),"cleanup deleted a fresh recording request");
  const auto second = edtrace::ClaimCaptureRequest(request.c_str());
  Check(second && !edtrace::CaptureRequestActive(request.c_str(), first) &&
        !edtrace::RetireCaptureRequest(request.c_str(), first),"cleanup affected another claimed request");
  Check(edtrace::RetireCaptureRequest(request.c_str(), second) && GetFileAttributesW(request.c_str()) == INVALID_FILE_ATTRIBUTES,
        "cleanup did not remove the consumed request");
}
struct PostObservation { U32 event; U64 object; U32 flags; Callback callback; void* cookie; const void* external; U32 playing; DWORD error; };
struct RtpcObservation { U32 parameter; float value; U64 object; std::int32_t duration,curve; bool bypass; DWORD error; };
struct EventActionObservation { U32 event, action; U64 object; std::int32_t transition; U32 curve, playing; DWORD error; };
PostObservation post_observed{};
RtpcObservation rtpc_observed{};
EventActionObservation event_action_observed{};
extern "C" __attribute__((noinline)) U32 WINAPI OwnedPost(U32 event,U64 object,U32 flags,Callback callback,
                                                         void* cookie,const void* external,U32 playing) {
  post_observed = {event,object,flags,callback,cookie,external,playing,GetLastError()};
  if (change_focus_during_post) {
    edtrace::TickOnFootFeedback(false,fixture_time);
    edtrace::TickOnFootFeedback(false,fixture_time,{},true);
  }
  SetLastError(0x2323); return event == 1851848329u ? 0 : 31337;
}
extern "C" __attribute__((noinline)) std::int32_t WINAPI OwnedRtpc(U32 parameter,float value,U64 object,
                                                                std::int32_t duration,std::int32_t curve,bool bypass) {
  rtpc_observed = {parameter,value,object,duration,curve,bypass,GetLastError()};
  if (parameter == 1497268741) attach_forwarded_after_admission = ship_admissions == 1;
  SetLastError(0x3434); return parameter == 2581359525u ? -17 : 1;
}
extern "C" __attribute__((noinline)) std::int32_t WINAPI OwnedSwitch(U32 group,U32 state,U64 object) {
  SetLastError(group ^ state); return object ? 1 : 2;
}
extern "C" __attribute__((noinline)) std::int32_t WINAPI OwnedRegister(U64 object) {
  SetLastError(static_cast<DWORD>(object)); return object ? 1 : 2;
}
extern "C" __attribute__((noinline)) std::int32_t WINAPI OwnedUnregister(U64 object) {
  SetLastError(static_cast<DWORD>(object + 1)); return object ? 1 : 2;
}
extern "C" __attribute__((noinline)) std::int32_t WINAPI OwnedEventAction(
    U32 event,U32 action,U64 object,std::int32_t transition,U32 curve,U32 playing) {
  event_action_observed = {event,action,object,transition,curve,playing,GetLastError()};
  SetLastError(0x4545);
  return transition < 0 ? 2 : 1;
}
Post volatile call_post = &OwnedPost;
Rtpc volatile call_rtpc = &OwnedRtpc;
EventAction volatile call_event_action = &OwnedEventAction;
std::int32_t (WINAPI* volatile call_register)(U64) = &OwnedRegister;
U32 observed_ship_motion = 0;
U64 observed_ship_object = 0;
bool replace_during_motion = false;
bool overflow_during_motion = false;
unsigned motion_native_calls = 0;
bool motion_native_active = false;
unsigned motion_cancels = 0;
unsigned failed_cancels = 0;
bool CancelObservedMotion(U64) noexcept {
  ++motion_cancels;
  if (failed_cancels) { --failed_cancels; return false; }
  motion_native_active = false;
  return true;
}
edtrace::ShipMotionResult ObserveShipMotion(U32 event, U64 object) noexcept {
  ++motion_native_calls;
  motion_native_active = true;
  observed_ship_motion = event;
  observed_ship_object = object;
  if (replace_during_motion) {
    edtrace::OnFootInput input{edtrace::OnFootInputKind::Unregister,object,fixture_time};
    input.result = 1;
    input.ship = edtrace::BeginShipWeaponInput(input.kind,object,0);
    edtrace::QueueOnFootInput(input);
    input.kind = edtrace::OnFootInputKind::Register;
    input.ship = edtrace::BeginShipWeaponInput(input.kind,object,0);
    edtrace::QueueOnFootInput(input);
  }
  if (overflow_during_motion) {
    for (unsigned i = 0; i < 2050; ++i) {
      edtrace::OnFootInput input{edtrace::OnFootInputKind::Post,object,fixture_time,252839122u,0,31337};
      input.ship = edtrace::BeginShipWeaponInput(input.kind,object,input.id);
      edtrace::QueueOnFootInput(input);
    }
  }
  return edtrace::ShipMotionResult::Posted;
}
edtrace::ShipWeaponAdmission AdmitShip(edtrace::OnFootInputKind kind, U64 object, U32 id) noexcept {
  auto admission = edtrace::BeginShipWeaponInput(kind,object,id);
  if (admission.tracked && kind == edtrace::OnFootInputKind::Parameter) ++ship_admissions;
  if (change_focus_during_post && admission.tracked) prior_activity = admission.activity;
  if (use_prior_activity && admission.tracked) admission.activity = prior_activity;
  SetLastError(0x8888);
  return admission;
}
void Consume(const edtrace::OnFootInput& input) noexcept {
  ++consumed;
  if (input.kind == edtrace::OnFootInputKind::Parameter) ++consumed_parameters;
  if (input.result == 1 && input.ship.tracked && input.kind == edtrace::OnFootInputKind::Parameter)
    ++shipped_results;
  auto timed = input;
  timed.milliseconds = fixture_time;
  edtrace::QueueOnFootInput(timed);
  SetLastError(0x9999);
}
void WINAPI CallbackToken(std::int32_t,void*) {}
template <typename T> Address Bits(T pointer) {
  Address value = 0; static_assert(sizeof(value) == sizeof(pointer)); std::memcpy(&value,&pointer,sizeof(value)); return value;
}
void PostEvent(U32 event, U64 object = 42) {
  std::array<U64,2> opaque{{0x1122334455667788ULL,0x9988776655443322ULL}};
  const auto original = opaque; SetLastError(0x1212);
  const U32 result = call_post(event,object,9,&CallbackToken,opaque.data()+1,opaque.data(),901);
  Check(result == (event == 1851848329u ? 0u : 31337u) && GetLastError() == 0x2323,"Post result/outgoing LastError changed");
  Check(post_observed.event == event && post_observed.object == object && post_observed.flags == 9 &&
        post_observed.callback == &CallbackToken && post_observed.cookie == opaque.data()+1 &&
        post_observed.external == opaque.data() && post_observed.playing == 901 && post_observed.error == 0x1212 && opaque == original,
        "Post arguments, opaque memory or incoming LastError changed");
}
void Parameter(U32 parameter,float value,U64 object = 42) {
  SetLastError(0x1313); const auto result = call_rtpc(parameter,value,object,-25,7,true);
  Check(result == (parameter == 2581359525u ? -17 : 1) && GetLastError() == 0x3434,"RTPC result/outgoing LastError changed");
  Check(rtpc_observed.parameter == parameter && rtpc_observed.value == value && rtpc_observed.object == object &&
        rtpc_observed.duration == -25 && rtpc_observed.curve == 7 && rtpc_observed.bypass && rtpc_observed.error == 0x1313,
        "RTPC arguments or incoming LastError changed");
}
void BeamAction(U32 action,U32 playing,std::int32_t milliseconds) {
  SetLastError(0x1414);
  const auto result = call_event_action(4054225129u,action,42,milliseconds,4,playing);
  Check(result == (milliseconds < 0 ? 2 : 1) && GetLastError() == 0x4545,
        "event action result or outgoing LastError changed");
  const auto& observed = event_action_observed;
  Check(observed.event == 4054225129u && observed.action == action && observed.object == 42 &&
        observed.transition == milliseconds && observed.curve == 4 && observed.playing == playing &&
        observed.error == 0x1414,"event action arguments or incoming LastError changed");
}
std::string ReadRecording(const wchar_t* directory) {
  WIN32_FIND_DATAW found{}; const std::wstring pattern = std::wstring(directory)+L"\\onfoot-events-*.jsonl";
  HANDLE search = FindFirstFileW(pattern.c_str(),&found); Check(search != INVALID_HANDLE_VALUE,"recording missing");
  FindClose(search);
  const std::wstring path = std::wstring(directory)+L"\\"+found.cFileName;
  FILE* file = _wfopen(path.c_str(),L"rb"); Check(file != nullptr,"recording unreadable");
  std::string text; std::array<char,4096> buffer{};
  for (;;) { const auto count = std::fread(buffer.data(),1,buffer.size(),file); text.append(buffer.data(),count); if (count < buffer.size()) break; }
  std::fclose(file); return text;
}
std::string EventLine(const std::string& text,U32 event) {
  const auto at = text.find("\"event_id\":"+std::to_string(event)+","); Check(at != std::string::npos,"recorded event missing");
  return text.substr(at,text.find('\n',at)-at);
}
}
int wmain(int argc,wchar_t** argv) {
  if (argc != 3) return 2;
  verification_result_path = argv[2];
  try {
    VerifySingleUseRequest(argv[1]);
    const Address image = Bits(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(image+dos->e_lfanew);
    edtrace::OnFootEventTraceFixtureTarget target{}; target.image = image; target.image_size = nt->OptionalHeader.SizeOfImage;
    target.addresses = {{Bits(&OwnedPost),Bits(&OwnedRtpc),Bits(&OwnedSwitch),Bits(&OwnedRegister),Bits(&OwnedUnregister)}};
    for (std::size_t i = 0; i < target.addresses.size(); ++i) std::memcpy(target.signatures[i].data(),reinterpret_cast<const void*>(target.addresses[i]),32);
    target.event_action = Bits(&OwnedEventAction);
    std::memcpy(target.event_action_signature.data(),reinterpret_cast<const void*>(target.event_action),32);
    target.observation_clock = &Clock; target.max_capture_ms = 600001;
    Check(edtrace::StartOnFootEventTraceControlledFixture(target,argv[1]) == ERROR_INVALID_PARAMETER,"longer-than-ten-minute recording accepted");
    target.max_capture_ms = 600000;
    const auto missing_directory = std::wstring(argv[1]) + L"\\nonexistent";
    Check(edtrace::StartOnFootEventTraceControlledFixture(target,missing_directory.c_str()) == ERROR_PATH_NOT_FOUND,
          "initial recorder accepted an unavailable log directory");
    edtrace::StartOnFootFeedback(0,true,1,false,&ObserveShipMotion,&CancelObservedMotion);
    Check(edtrace::StartOnFootEventSourcesControlledFixture(target,&Consume,&AdmitShip) == ERROR_SUCCESS,
          "the initial file error blocked a fresh start");
    Check(edtrace::StartOnFootEventRecording(missing_directory.c_str()) != ERROR_SUCCESS &&
          edtrace::GetOnFootEventTraceStats().hook_mask == 63,"failed recorder setup disabled numeric hooks");
    Parameter(1326349511u,75.5f,std::numeric_limits<U64>::max());
    Check(!edtrace::GetOnFootEventTraceStats().capture_open && consumed_parameters == 0,"new RTPC enabled recording or an effect input");
    Check(edtrace::StartOnFootEventRecording(argv[1]) == ERROR_SUCCESS,"manual recording failed");
    Parameter(1326349511u,75.5f,std::numeric_limits<U64>::max()); Parameter(2581359525u,88.0f);
    Parameter(1951144523u,12.25f); Parameter(4274346379u,.5f); Parameter(123456789u,.5f);
    Parameter(1497268741u,1.0f);
    Check(attach_forwarded_after_admission && shipped_results == 1,"ship admission spans the original parameter call");
    Check(consumed_parameters == 1,"native strength parameters leaked into the on-foot effect consumer");
    for (const U32 event : {685855539u,1357267772u,285258775u,123456789u,1851848329u}) PostEvent(event);
    clock_ms.store(600999); PostEvent(2879602485u);
    Check(!edtrace::GetOnFootEventTraceStats().time_limit_reached,"recording stopped before its deadline");
    clock_ms.store(601000);
    const ULONGLONG deadline = GetTickCount64()+5000;
    while (edtrace::GetOnFootEventTraceStats().capture_open && GetTickCount64() < deadline) Sleep(1);
    Check(edtrace::GetOnFootEventTraceStats().time_limit_reached && !edtrace::GetOnFootEventTraceStats().capture_open,"idle recording did not stop at ten minutes");
    PostEvent(111); Parameter(1326349511u,50.0f);
    Check(consumed == 8 && consumed_parameters == 1,"deadline changed the live consumer or its parameter filter");
    Check(edtrace::StopOnFootEventTrace(),"recording drain failed");
    const auto stats = edtrace::GetOnFootEventTraceStats();
    Check(stats.written == 11 && stats.post_calls == 6 && stats.rtpc_calls == 5 && !stats.queue_drops && !stats.storage_drops && !stats.logging_error,
          "recording changed call counts or lost records");
    Check(stats.hook_mask == 63 && !stats.capture_open,"recording deadline changed live hooks or restarted itself");
    const auto text = ReadRecording(argv[1]);
    Check(text.find("\"max_capture_ms\":600000") != std::string::npos && text.find("\"native_motion_route_schema\":1") != std::string::npos,"recording metadata missing");
    for (const char* name : {"shield_strength_self","hull_strength_self","collision_velocity","friction_velocity"})
      Check(text.find(std::string("\"parameter_name\":\"")+name+"\"") != std::string::npos,"named strength parameter missing");
    Check(text.find("\"game_object_id\":18446744073709551615,\"result\":1,\"parameter_id\":1326349511") != std::string::npos,"global strength request discarded");
    Check(text.find("\"parameter_id\":123456789") == std::string::npos,"unknown parameter captured");
    Check(EventLine(text,1357267772u).find("\"play_source_ids\":[15548333],\"stop_source_ids\":[942997056]") != std::string::npos,"native end/tail annotation missing");
    Check(EventLine(text,2879602485u).find("\"phase\":\"one_shot\"") != std::string::npos,"collision misclassified as a held effect");
    for (const U32 event : {285258775u,123456789u}) Check(EventLine(text,event).find("\"native_motion_route\":null") != std::string::npos,"audio-only/unknown event falsely classified");
    const auto rejected = EventLine(text,1851848329u);
    Check(rejected.find("\"returned_playing_id\":0") != std::string::npos && rejected.find("\"source_execution_observed\":false") != std::string::npos &&
          rejected.find("\"local_player\":\"unknown\"") != std::string::npos,"failed post acquired execution or ownership proof");

    for (int i = 0; i < 2200; ++i) PostEvent(123456789u);
    Check(edtrace::GetOnFootRuntimeStats().dropped == 0,"unmapped audio cannot exhaust the ship-only queue");
    fixture_time = 100;
    Check(call_register(42) == 1,"owned ship hardpoint registration failed");
    Parameter(1497268741u,1.0f);
    edtrace::TickOnFootFeedback(false,fixture_time,{},true);
    fixture_time = 150;
    change_focus_during_post = true;
    PostEvent(4054225129u);
    change_focus_during_post = false;
    edtrace::TickOnFootFeedback(false,160,{},true);
    Check(!edtrace::HapticNonzero(output),"late post from before focus loss reactivated ship output");
    fixture_time = 200;
    PostEvent(4054225129u);
    edtrace::TickOnFootFeedback(false,fixture_time,{},true);
    Check(output.low == .15f && output.high == .25f,"fresh owned post did not reach the real ship policy");
    fixture_time = 240;
    use_prior_activity = true;
    PostEvent(3037970380u);
    use_prior_activity = false;
    edtrace::TickOnFootFeedback(false,fixture_time,{},true);
    Check(!edtrace::HapticNonzero(output),"a successful release must clear a matching hold across focus epochs");
    fixture_time = 300;
    PostEvent(4054225129u);
    edtrace::TickOnFootFeedback(false,fixture_time,{},true);
    Check(edtrace::HapticNonzero(output),"beam starts before direct action checks");
    fixture_time = 420;
    BeamAction(1,31337,3500);
    BeamAction(0,31338,3500);
    BeamAction(0,31337,-1);
    edtrace::TickOnFootFeedback(false,fixture_time,{},true);
    Check(edtrace::HapticNonzero(output),"other instances, pauses, and rejected actions leave the beam active");
    fixture_time = 460;
    BeamAction(0,31337,3500);
    edtrace::TickOnFootFeedback(false,fixture_time,{},true);
    Check(!edtrace::HapticNonzero(output),"a direct Stop clears beam feedback immediately even with a long audio fade");
    fixture_time = 500;
    PostEvent(252839122u);
    edtrace::TickOnFootFeedback(false,525,{},true);
    const auto ship_motion = edtrace::GetOnFootRuntimeStats();
    Check(ship_motion.ship_motion_posts == 1 && observed_ship_motion == 1472761002u &&
          observed_ship_object == 42 && !edtrace::HapticNonzero(output),
          "captured railgun cue dispatches only authored Motion through the gameplay worker");
    edtrace::SetShipWeaponFeedbackStrength(0);
    fixture_time = 600;
    PostEvent(2249258127u);
    edtrace::TickOnFootFeedback(false,625,{},true);
    edtrace::SetShipWeaponFeedbackStrength(1);
    edtrace::TickOnFootFeedback(false,645,{},true);
    Check(edtrace::GetOnFootRuntimeStats().ship_motion_posts == 1,
          "strength zero discards the cue without dispatching on re-enable");
    edtrace::SetShipWeaponFeedbackStrength(0);
    edtrace::OnFootInput delayed{edtrace::OnFootInputKind::Post,42,700,252839122u,0,31337};
    delayed.ship = edtrace::BeginShipWeaponInput(delayed.kind,42,delayed.id);
    edtrace::SetShipWeaponFeedbackStrength(1);
    edtrace::QueueOnFootInput(delayed);
    edtrace::TickOnFootFeedback(false,725,{},true);
    Check(edtrace::GetOnFootRuntimeStats().ship_motion_posts == 1,
          "a firing result admitted at zero strength cannot revive on re-enable");
    replace_during_motion = true;
    fixture_time = 800;
    PostEvent(252839122u);
    edtrace::TickOnFootFeedback(false,825,{},true);
    const auto reentered = edtrace::GetOnFootRuntimeStats();
    Check(reentered.ship_motion_posts == 1 && reentered.ship_motion_rejected == 1 &&
          !motion_native_active,
          "native post reentrancy can publish lifecycle changes and cancels its own stale correction");
    replace_during_motion = false;
    fixture_time = 1000;
    call_register(42);
    Parameter(1497268741u,1);
    call_register(43);
    Parameter(1497268741u,1,43);
    edtrace::TickOnFootFeedback(false,1000,{},true);
    fixture_time = 1100;
    PostEvent(252839122u);
    fixture_time = 1101;
    PostEvent(2249258127u,43);
    const auto before_native_calls = motion_native_calls;
    overflow_during_motion = true;
    edtrace::TickOnFootFeedback(false,1125,{},true);
    const auto overflowed = edtrace::GetOnFootRuntimeStats();
    Check(overflowed.faulted && overflowed.ship_motion_posts == 1 &&
          overflowed.ship_motion_rejected == 3 && motion_native_calls == before_native_calls + 1 &&
          !motion_native_active && !edtrace::HapticNonzero(output),
          "publication failure cancels the accepted post and prevents the next native dispatch");
    failed_cancels = 1;
    Check(!edtrace::StopOnFootFeedback(), "shutdown reports native stops that still need service");
    Check(edtrace::StopOnFootFeedback(), "a subsequent shutdown service can complete native cancellation");
    edtrace::ClearOnFootEventConsumer(); Check(edtrace::StopOnFootEventTrace(),"final hook shutdown failed");
    Check(VerificationResult("PASS"),"result file failed"); std::puts("PASS bounded native impact recording and forwarding"); return 0;
  } catch (const std::exception& error) {
    edtrace::ClearOnFootEventConsumer(); edtrace::StopOnFootEventTrace();
    VerificationResult(std::string("FAIL ")+error.what()); std::fprintf(stderr,"FAIL %s\n",error.what()); return 1;
  }
}
