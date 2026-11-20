#include <winsock2.h>
#include <bcrypt.h>
#include "edtrace/mac_output.hpp"
#include "edtrace/mac_haptics.hpp"
#include <array>
#include <atomic>
#include <cstring>

namespace edtrace { namespace {
SRWLOCK lock=SRWLOCK_INIT;
SOCKET channel=INVALID_SOCKET;
bool winsock=false,closing=false;
macbridge::Token token{};
macbridge::Route route;
HapticFrame latest{};
bool latest_known=false,peer_known=false;
std::uint64_t latest_at=0,last_sequence=0;
sockaddr_in peer{};
MacOutputStats stats;
std::atomic<bool> blocked{false};
macbridge::Relaunch relaunch;
std::wstring launch_directory,launch_status_directory;
std::uint64_t Now() noexcept { return GetTickCount64(); }
std::uint64_t (*clock_now)() noexcept=&Now;

std::wstring Quote(const std::wstring& argument) {
  std::wstring result=L"\"";unsigned slashes=0;
  for(const auto ch:argument) {
    if(ch==L'\\') { ++slashes;continue; }
    result.append(ch==L'\"'?slashes*2+1:slashes,L'\\');slashes=0;result+=ch;
  }
  result.append(slashes*2,L'\\');return result+L"\"";
}
std::wstring UnixName(const std::wstring& path) {
  using Convert=char* (__cdecl*)(const wchar_t*);
  const auto raw=GetProcAddress(GetModuleHandleW(L"kernel32.dll"),"wine_get_unix_file_name");
  Convert convert=nullptr;static_assert(sizeof(convert)==sizeof(raw));std::memcpy(&convert,&raw,sizeof(raw));
  if(!convert) return {};
  char* name=convert(path.c_str());if(!name) return {};
  const int size=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,name,-1,nullptr,0);
  std::wstring result;
  if(size>1) { result.resize(static_cast<std::size_t>(size));MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,name,-1,result.data(),size);result.pop_back(); }
  HeapFree(GetProcessHeap(),0,name);return result;
}
DWORD Launch(const std::wstring& directory,const std::wstring& status_directory,const macbridge::Token& key,unsigned port,
             const std::wstring& opener=L"\\\\?\\unix/usr/bin/open") {
  const auto application=UnixName(directory+L"\\ed-macos-haptics.app");
  const auto output=UnixName(status_directory);
  if(application.empty()||output.empty()) return ERROR_FILE_NOT_FOUND;
  constexpr wchar_t hex[]=L"0123456789abcdef";
  std::wstring text;for(const auto b:key) { text+=hex[b>>4];text+=hex[b&15]; }
  // Wine's CreateProcess supports non-PE Unix executables. Use the absolute
  // system launcher directly; there is no shell, PATH search or activation.
  std::wstring command=Quote(opener)+L" -g -n -a "+Quote(application)+L" --args --port "+std::to_wstring(port)+
    L" --token "+text+L" --status "+Quote(output+L"/macos-haptics-status.json");
  STARTUPINFOW startup{};startup.cb=sizeof(startup);PROCESS_INFORMATION process{};
  if(!CreateProcessW(opener.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,directory.c_str(),&startup,&process)) return GetLastError();
  // A native Unix child need not have Windows process handles. Its bounded
  // server heartbeat governs lifetime; never terminate an unrelated process.
  if(process.hThread) CloseHandle(process.hThread);
  if(process.hProcess) CloseHandle(process.hProcess);
  return ERROR_SUCCESS;
}
DWORD StartChannel() noexcept {
  if(channel!=INVALID_SOCKET) return ERROR_ALREADY_INITIALIZED;
  WSADATA data{};const int startup=WSAStartup(MAKEWORD(2,2),&data);
  if(startup) return static_cast<DWORD>(startup);
  winsock=true;channel=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
  if(channel!=INVALID_SOCKET) {
    sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    BOOL exclusive=TRUE;u_long nonblocking=1;int count=sizeof(address);
    if(setsockopt(channel,SOL_SOCKET,SO_EXCLUSIVEADDRUSE,reinterpret_cast<const char*>(&exclusive),sizeof(exclusive))==0&&
       bind(channel,reinterpret_cast<const sockaddr*>(&address),sizeof(address))==0&&
       getsockname(channel,reinterpret_cast<sockaddr*>(&address),&count)==0&&
       ioctlsocket(channel,FIONBIO,&nonblocking)==0) {
      stats.port=ntohs(address.sin_port);stats.listening=true;return ERROR_SUCCESS;
    }
  }
  const DWORD failure=WSAGetLastError();
  if(channel!=INVALID_SOCKET) closesocket(channel);
  channel=INVALID_SOCKET;WSACleanup();winsock=false;return failure;
}
void SnapshotRoute() noexcept {
  stats.active=route.Active();stats.blocks_fallback=route.BlocksFallback();stats.generation=route.Generation();
  stats.acquiring=route.Acquiring();
  blocked.store(stats.blocks_fallback,std::memory_order_release);
}
void PollLocked() noexcept {
  if(channel==INVALID_SOCKET) return;
  for(unsigned i=0;i<8;++i) {
    unsigned char bytes[macbridge::kPacketSize+1]{};sockaddr_in source{};int size=sizeof(source);
    const int count=recvfrom(channel,reinterpret_cast<char*>(bytes),sizeof(bytes),0,reinterpret_cast<sockaddr*>(&source),&size);
    if(count==SOCKET_ERROR) break;
    macbridge::Message message{};
    if(size!=sizeof(source)||source.sin_family!=AF_INET||source.sin_addr.s_addr!=htonl(INADDR_LOOPBACK)||
       !macbridge::Decode(bytes,static_cast<std::size_t>(count),message)||message.kind!=macbridge::Kind::Poll||message.token!=token||
       (peer_known&&source.sin_port!=peer.sin_port)||message.sequence<=last_sequence) continue;
    peer=source;peer_known=true;last_sequence=message.sequence;++stats.polls;relaunch.Seen(clock_now());
    stats.controllers=message.controllers;stats.motors=message.motors;stats.client_error=message.error;
    const auto now=clock_now();route.Observe(message,now);SnapshotRoute();
    macbridge::Message reply{};reply.kind=closing?macbridge::Kind::Stop:macbridge::Kind::Frame;
    reply.token=token;reply.sequence=message.sequence;reply.generation=route.Generation();reply.active=!closing&&route.Active();
    if(reply.active&&latest_known&&now>=latest_at&&now-latest_at<macbridge::kSourceLeaseMs) reply.frame=latest;
    const auto packet=macbridge::Encode(reply);
    if(sendto(channel,reinterpret_cast<const char*>(packet.data()),packet.size(),0,reinterpret_cast<const sockaddr*>(&peer),sizeof(peer))!=static_cast<int>(packet.size()))
      stats.error=WSAGetLastError();
  }
}
}}

namespace edtrace {
DWORD MacOutputStart(const std::wstring& directory,const std::wstring& status_directory,bool enabled) noexcept {
  const DWORD incoming=GetLastError();DWORD result=ERROR_NOT_SUPPORTED;
  try {
    // This profile also runs in owned Wine fixtures on Linux. Only a real Mac
    // host may launch Apple's system executable.
    if(enabled&&GetFileAttributesW(L"\\\\?\\unix/System/Library/CoreServices/SystemVersion.plist")!=INVALID_FILE_ATTRIBUTES) {
      AcquireSRWLockExclusive(&lock);
      if(BCryptGenRandom(nullptr,token.data(),static_cast<ULONG>(token.size()),BCRYPT_USE_SYSTEM_PREFERRED_RNG)<0) result=ERROR_GEN_FAILURE;
      else result=StartChannel();
      stats.error=result;ReleaseSRWLockExclusive(&lock);
      if(!result) {
        const auto launch=Launch(directory,status_directory,token,stats.port);
        AcquireSRWLockExclusive(&lock);stats.launch_error=launch;stats.launched=launch==ERROR_SUCCESS;
        if(stats.launched) { launch_directory=directory;launch_status_directory=status_directory;relaunch.Launched(clock_now()); }
        ReleaseSRWLockExclusive(&lock);
      }
    }
  } catch(...) { result=ERROR_NOT_ENOUGH_MEMORY; }
  if(result) {
    AcquireSRWLockExclusive(&lock);stats.error=result;ReleaseSRWLockExclusive(&lock);
  }
  SetLastError(incoming);return result;
}
void MacOutputPoll() noexcept {
  const DWORD incoming=GetLastError();AcquireSRWLockExclusive(&lock);PollLocked();
  // A helper that answered and then went quiet has exited (it gives up after
  // two seconds without a reply) or hung. Relaunch it under a fresh token:
  // a late packet from the old helper fails the token check and gets no reply,
  // so that helper still exits by itself and never shares the controller.
  bool relaunching=false;macbridge::Token key{};unsigned port=0;
  std::wstring directory,status_directory;
  const auto now=clock_now();
  if(!closing&&channel!=INVALID_SOCKET&&relaunch.Due(now)) {
    relaunch.Attempted(now);++stats.relaunches;
    try {
      directory=launch_directory;status_directory=launch_status_directory;
      if(BCryptGenRandom(nullptr,token.data(),static_cast<ULONG>(token.size()),BCRYPT_USE_SYSTEM_PREFERRED_RNG)>=0) {
        peer_known=false;last_sequence=0;key=token;port=stats.port;relaunching=true;
      }
    } catch(...) {}
  }
  ReleaseSRWLockExclusive(&lock);
  if(relaunching) {
    DWORD launch=ERROR_NOT_ENOUGH_MEMORY;
    try { launch=Launch(directory,status_directory,key,port); } catch(...) {}
    AcquireSRWLockExclusive(&lock);stats.launch_error=launch;ReleaseSRWLockExclusive(&lock);
  }
  SetLastError(incoming);
}
bool MacOutputSelect(bool allowed) noexcept {
  const DWORD incoming=GetLastError();AcquireSRWLockExclusive(&lock);
  const auto before=route.Generation();const bool selected=route.Select(allowed&&stats.listening&&!closing,clock_now());
  if(before!=route.Generation()) { latest={};latest_known=false; }
  SnapshotRoute();ReleaseSRWLockExclusive(&lock);SetLastError(incoming);return selected;
}
bool MacOutputBlocksFallback() noexcept { return blocked.load(std::memory_order_acquire); }
bool MacOutputSubmit(HapticFrame frame) noexcept {
  const DWORD incoming=GetLastError();
  if(!TryAcquireSRWLockExclusive(&lock)) { SetLastError(incoming);return blocked.load(std::memory_order_acquire); }
  const bool handled=route.BlocksFallback();
  if(route.Active()) { latest=HapticScale(frame,1);latest_at=clock_now();latest_known=true;++stats.frames;if(latest.left>0||latest.right>0) ++stats.trigger_frames; }
  ReleaseSRWLockExclusive(&lock);SetLastError(incoming);return handled;
}
MacOutputStats MacOutputGetStats() noexcept {
  AcquireSRWLockShared(&lock);const auto result=stats;ReleaseSRWLockShared(&lock);return result;
}
void MacOutputStop() noexcept {
  const DWORD incoming=GetLastError();
  AcquireSRWLockExclusive(&lock);closing=true;route.Select(false,clock_now());latest={};latest_known=false;SnapshotRoute();ReleaseSRWLockExclusive(&lock);
  // Give the helper a bounded opportunity to receive Stop; lost packets and
  // crashes still expire its finite pulses and two-second server heartbeat.
  for(unsigned i=0;i<6&&channel!=INVALID_SOCKET;++i) { MacOutputPoll();Sleep(10); }
  AcquireSRWLockExclusive(&lock);
  if(channel!=INVALID_SOCKET) closesocket(channel);
  channel=INVALID_SOCKET;if(winsock) WSACleanup();winsock=false;stats.listening=false;stats.active=false;
  ReleaseSRWLockExclusive(&lock);SetLastError(incoming);
}
#ifdef ED_MAC_OUTPUT_FIXTURE
DWORD MacOutputStartControlled(std::uint64_t (*clock)() noexcept) noexcept {
  if(!clock) return ERROR_INVALID_PARAMETER;
  AcquireSRWLockExclusive(&lock);clock_now=clock;token.fill(0x5a);stats.error=StartChannel();const auto result=stats.error;ReleaseSRWLockExclusive(&lock);return result;
}
DWORD MacOutputLaunchControlled(const std::wstring& opener,const std::wstring& directory) noexcept {
  try { return Launch(directory,directory,token,stats.port,opener); } catch(...) { return ERROR_NOT_ENOUGH_MEMORY; }
}
#endif
} // namespace edtrace
