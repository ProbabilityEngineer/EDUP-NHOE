#include "edtrace/mac_haptics.hpp"
#include <cstdio>
#include <cstdlib>
namespace mb=edtrace::macbridge;
static void Check(bool value,const char* name) { if(!value) { std::fprintf(stderr,"FAIL %s\n",name);std::exit(1); } }
int main() {
  Check(mb::Eligible(1,true,15,true),"one fully usable Xbox can be selected");
  Check(!mb::Eligible(0,true,15,true)&&!mb::Eligible(2,true,15,true)&&!mb::Eligible(1,false,15,true)&&
    !mb::Eligible(1,true,3,true)&&!mb::Eligible(1,true,15,false),"missing, ambiguous or unsupported native routes remain fallback");
  std::array<float,4> delivered{};
  Check(mb::Apply({.2f,.3f,.4f,.5f},&delivered,[](void* context,unsigned index,float value) {
    (*static_cast<std::array<float,4>*>(context))[index]=value;return true;
  }),"actuator boundary accepts four independent amplitudes");
  Check(delivered[0]==.2f&&delivered[1]==.3f&&delivered[2]==.4f&&delivered[3]==.5f,"main and trigger channels remain in their own actuators");
  mb::Message poll{};poll.token.fill(7);poll.sequence=3;poll.active=true;poll.controllers=1;poll.motors=15;poll.device_epoch=1;
  auto bytes=mb::Encode(poll);mb::Message decoded{};
  Check(bytes[0]=='E'&&bytes[3]=='H'&&bytes[5]==1&&bytes[24]==3&&bytes[40]==1&&bytes[44]==15&&bytes[52]==1,
        "poll has the literal versioned wire layout");
  Check(mb::Decode(bytes.data(),bytes.size(),decoded)&&decoded.controllers==1&&decoded.motors==15,"valid native inventory survives transport");
  bytes[79]=1;Check(!mb::Decode(bytes.data(),bytes.size(),decoded),"unknown wire fields rejected");
  bytes=mb::Encode(poll);bytes[59]=0x7f;bytes[58]=0xc0;
  Check(!mb::Decode(bytes.data(),bytes.size(),decoded),"nonfinite and nonzero poll amplitudes rejected");
  mb::Route route;route.Observe(poll,100);
  Check(!route.Select(true,100)&&route.BlocksFallback(),"fresh companion first quiesces the previous provider");
  route.Observe(poll,550);
  Check(!route.Select(true,599)&&route.BlocksFallback(),"native acquisition waits for the prior provider fence");
  Check(route.Select(true,600)&&route.Active(),"fresh ready companion acquires after provider fence");
  Check(!route.Select(false,610)&&route.BlocksFallback(),"release waits for native output stop");
  Check(!route.Select(false,1109)&&route.BlocksFallback(),"fallback cannot race a late finite native pulse");
  Check(!route.Select(false,1110)&&!route.BlocksFallback(),"bounded stop fence eventually allows fallback");
  route.Observe(poll,1200);Check(!route.Select(true,1200),"reacquisition starts quiet");
  route.Observe(poll,1650);Check(route.Select(true,1700),"fresh companion resumes after acquisition fence");
  Check(!route.Select(true,1810)&&route.BlocksFallback(),"expired companion heartbeat revokes native output");
  poll.zeroed=true;poll.generation=route.Generation();route.Observe(poll,1811);
  Check(!route.Select(false,1811)&&!route.BlocksFallback(),"confirmed stop releases fallback without extra delay");
  route.Observe(poll,1900);Check(!route.Select(true,1900),"acknowledged route begins acquisition");
  route.Observe(poll,2350);Check(route.Select(true,2400),"native output reacquires after acknowledged stop");
  ++poll.device_epoch;route.Observe(poll,2410);
  Check(!route.Active()&&route.BlocksFallback(),"controller replacement cancels the old native lease");
  mb::Message response{};response.kind=mb::Kind::Frame;response.token=poll.token;response.sequence=6;response.generation=5;response.active=true;
  response.frame={.2f,.3f,.4f,.5f};bytes=mb::Encode(response);
  Check(mb::Decode(bytes.data(),bytes.size(),decoded)&&decoded.frame.left==.4f&&decoded.frame.right==.5f,"trigger frame survives transport separately");
  mb::Receiver receiver;receiver.Begin(6,1000);
  Check(!receiver.Accept(decoded,mb::Token{},1001),"foreign session rejected");
  Check(!receiver.Accept(decoded,poll.token,1080),"late reply cannot acquire a fresh lifetime");
  receiver.Begin(6,1100);Check(receiver.Accept(decoded,poll.token,1110),"current timely reply accepted");
  Check(!receiver.Accept(decoded,poll.token,1111),"reply replay rejected");
  receiver.Begin(7,1200);decoded.sequence=7;decoded.generation=4;
  Check(!receiver.Accept(decoded,poll.token,1201),"old route generation cannot reactivate output");
  receiver.Cancel();decoded.generation=6;
  Check(!receiver.Accept(decoded,poll.token,1202),"device replacement invalidates an outstanding reply");
  Check(!mb::Apply({},nullptr,nullptr),"missing actuator boundary fails quietly");
  mb::Relaunch relaunch;
  Check(!relaunch.Due(100000),"no relaunch before the first launch");
  relaunch.Launched(0);
  Check(!relaunch.Due(100000),"a helper that never answered is never relaunched");
  relaunch.Seen(20000);
  Check(!relaunch.Due(22499)&&relaunch.Due(22500),"a helper silent for 2.5 s after answering is due");
  relaunch.Seen(22600);Check(!relaunch.Due(23000),"a fresh poll cancels a pending relaunch");
  relaunch.Attempted(30000);
  Check(relaunch.Attempts()==1&&!relaunch.Due(100000),"a relaunched helper must answer before another attempt");
  relaunch.Seen(30100);
  Check(!relaunch.Due(39999)&&relaunch.Due(40000),"relaunches are spaced at least 10 s apart");
  for(std::uint64_t now=40000;relaunch.Attempts()<mb::kMaxRelaunches;now+=20000) { relaunch.Attempted(now);relaunch.Seen(now+1); }
  Check(!relaunch.Due(10000000),"relaunches stop at the cap");
  std::puts("PASS native Mac capability selection, independent channels, stop fence, stale/replayed packet guards and bounded helper relaunch");
}
