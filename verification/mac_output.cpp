#include <winsock2.h>
#include "edtrace/mac_output.hpp"
#include "edtrace/mac_haptics.hpp"
#include "result_file.hpp"
#include <cstdio>
#include <cstdlib>
#include <cwchar>
namespace mb=edtrace::macbridge;
static std::uint64_t instant=100;
static std::uint64_t Clock() noexcept { return instant; }
static void Check(bool value,const char* name) { if(!value) { std::fprintf(stderr,"FAIL %s\n",name);std::exit(1); } }
int wmain(int argc,wchar_t** argv) {
  if(argc==5&&std::wcscmp(argv[1],L"launch")==0) {
    verification_result_path=argv[4];
    Check(edtrace::MacOutputStartControlled(&Clock)==0,"launch fixture initializes its own session");
    Check(edtrace::MacOutputLaunchControlled(argv[2],argv[3])==0,"Wine starts an owned native Unix launcher without a shell");
    edtrace::MacOutputStop();Check(VerificationResult("PASS"),"launch result saved");return 0;
  }
  if(argc==2) verification_result_path=argv[1];
  const auto native=edtrace::ParseMacOutputPreference(L"MaCoS");
  const auto automatic=edtrace::ParseMacOutputPreference(L"auto");
  const auto fallback=edtrace::ParseMacOutputPreference(L"xinput");
  Check(native.native&&!native.gameinput,"macos mode enables the native companion without GameInput");
  Check(automatic.native&&automatic.gameinput,"auto keeps the existing native and GameInput output paths");
  Check(!fallback.native&&!fallback.gameinput,"xinput skips both optional output paths");
  Check(edtrace::MacOutputStartControlled(&Clock)==0,"owned relay starts on an ephemeral loopback port");
  const auto info=edtrace::MacOutputGetStats();Check(info.listening&&info.port>0,"owned relay reports its private port");
  SOCKET client=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);Check(client!=INVALID_SOCKET,"synthetic native client socket created");
  DWORD timeout=500;setsockopt(client,SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<const char*>(&timeout),sizeof(timeout));
  sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);address.sin_port=htons(static_cast<u_short>(info.port));
  mb::Message request{};request.token.fill(0x5a);request.active=true;request.controllers=1;request.motors=15;request.device_epoch=1;request.zeroed=true;
  auto foreign=request;foreign.token.fill(0x6b);foreign.sequence=1;const auto unauthorized=mb::Encode(foreign);
  Check(sendto(client,reinterpret_cast<const char*>(unauthorized.data()),unauthorized.size(),0,reinterpret_cast<const sockaddr*>(&address),sizeof(address))==static_cast<int>(unauthorized.size()),"foreign-session packet sent");
  edtrace::MacOutputPoll();u_long nonblocking=1;ioctlsocket(client,FIONBIO,&nonblocking);char rejected[81]{};
  Check(recv(client,rejected,sizeof(rejected),0)==SOCKET_ERROR&&WSAGetLastError()==WSAEWOULDBLOCK&&edtrace::MacOutputGetStats().polls==0,
        "foreign-session packet receives no reply or native lease");
  nonblocking=0;ioctlsocket(client,FIONBIO,&nonblocking);
  auto exchange=[&]() {
    ++request.sequence;const auto bytes=mb::Encode(request);
    Check(sendto(client,reinterpret_cast<const char*>(bytes.data()),bytes.size(),0,reinterpret_cast<const sockaddr*>(&address),sizeof(address))==static_cast<int>(bytes.size()),"synthetic capability poll sent");
    edtrace::MacOutputPoll();unsigned char received[mb::kPacketSize+1]{};
    const int count=recv(client,reinterpret_cast<char*>(received),sizeof(received),0);mb::Message reply{};
    Check(count>0&&mb::Decode(received,static_cast<std::size_t>(count),reply)&&reply.sequence==request.sequence&&reply.token==request.token,"authenticated reply matches its poll");
    request.generation=reply.generation;return reply;
  };
  Check(!exchange().active,"startup native output is quiet");
  Check(!edtrace::MacOutputSelect(true)&&edtrace::MacOutputBlocksFallback(),"acquisition blocks fallback before any native frame");
  instant=550;exchange();instant=600;
  Check(edtrace::MacOutputSelect(true),"ready native companion acquires after quiet fence");
  Check(edtrace::MacOutputSubmit({.2f,.3f,.4f,.5f}),"four-channel mix is accepted without socket work on caller");
  instant=610;const auto frame=exchange();
  Check(frame.active&&frame.frame.low==.2f&&frame.frame.high==.3f&&frame.frame.left==.4f&&frame.frame.right==.5f,"real relay carries each motor amplitude independently");
  instant=700;const auto stale=exchange();
  Check(stale.active&&!edtrace::HapticNonzero(stale.frame),"expired audio frame becomes zero while lease remains active");
  instant=720;Check(!edtrace::MacOutputSelect(false)&&edtrace::MacOutputBlocksFallback(),"focus loss stops native and fences fallback");
  Check(!exchange().active,"inactive native reply contains no output");
  request.zeroed=true;instant=730;exchange();edtrace::MacOutputSelect(false);
  Check(!edtrace::MacOutputBlocksFallback(),"native zero acknowledgement releases fallback");
  instant=800;exchange();edtrace::MacOutputSelect(true);
  instant=1250;exchange();instant=1300;Check(edtrace::MacOutputSelect(true),"native route can reacquire after focus return");
  instant=1410;Check(!edtrace::MacOutputSelect(true)&&edtrace::MacOutputBlocksFallback(),"lost native heartbeat shuts down output");
  instant=1910;edtrace::MacOutputSelect(false);Check(!edtrace::MacOutputBlocksFallback(),"lost helper eventually returns to working fallback");
  closesocket(client);edtrace::MacOutputStop();
  Check(VerificationResult("PASS"),"fixture result saved");
  std::puts("PASS native Mac loopback relay, separate channels, source expiry, focus stop and lost-helper fallback");return 0;
}
