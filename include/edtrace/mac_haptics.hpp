#pragma once
#include "edtrace/haptic_frame.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace edtrace::macbridge {
constexpr std::size_t kPacketSize = 80;
constexpr std::uint64_t kPollDeadlineMs = 80, kClientLeaseMs = 160;
constexpr std::uint64_t kSourceLeaseMs = 100, kFallbackFenceMs = 500;
constexpr std::uint64_t kAcquireFenceMs = 500;
constexpr double kPulseSeconds = .12;
using Token = std::array<unsigned char,16>;
using Packet = std::array<unsigned char,kPacketSize>;
enum class Kind : unsigned char { Poll=1, Frame=2, Stop=3 };
struct Message {
  Kind kind = Kind::Poll;
  bool active = false, zeroed = false;
  Token token{};
  std::uint64_t sequence = 0, generation = 0;
  std::uint32_t controllers = 0, motors = 0, error = 0, device_epoch = 0;
  HapticFrame frame{};
};
inline void Put(Packet& p,std::size_t offset,std::uint64_t value,unsigned count) noexcept {
  for(unsigned i=0;i<count;++i) p[offset+i]=static_cast<unsigned char>(value>>(i*8));
}
inline std::uint64_t Get(const Packet& p,std::size_t offset,unsigned count) noexcept {
  std::uint64_t value=0;
  for(unsigned i=0;i<count;++i) value|=std::uint64_t(p[offset+i])<<(i*8);
  return value;
}
inline Packet Encode(const Message& message) noexcept {
  Packet p{}; p[0]='E';p[1]='D';p[2]='M';p[3]='H';p[4]=1;
  p[5]=static_cast<unsigned char>(message.kind);p[6]=message.active?1:0;p[7]=message.zeroed?1:0;
  std::memcpy(p.data()+8,message.token.data(),16);
  Put(p,24,message.sequence,8);Put(p,32,message.generation,8);
  Put(p,40,message.controllers,4);Put(p,44,message.motors,4);Put(p,48,message.error,4);
  Put(p,52,message.device_epoch,4);
  const auto f=message.kind==Kind::Frame&&message.active?HapticScale(message.frame,1):HapticFrame{};
  const float values[]{f.low,f.high,f.left,f.right};
  for(unsigned i=0;i<4;++i) { std::uint32_t bits=0;std::memcpy(&bits,&values[i],4);Put(p,56+i*4,bits,4); }
  return p;
}
inline bool Decode(const unsigned char* data,std::size_t count,Message& output) noexcept {
  output={};
  if(!data||count!=kPacketSize) return false;
  Packet p{};std::memcpy(p.data(),data,count);
  if(p[0]!='E'||p[1]!='D'||p[2]!='M'||p[3]!='H'||p[4]!=1||p[5]<1||p[5]>3||p[6]>1||p[7]>1) return false;
  for(unsigned i=72;i<80;++i) if(p[i]) return false;
  Message m{};m.kind=static_cast<Kind>(p[5]);m.active=p[6]!=0;m.zeroed=p[7]!=0;
  std::memcpy(m.token.data(),p.data()+8,16);
  if(m.token==Token{}) return false;
  m.sequence=Get(p,24,8);m.generation=Get(p,32,8);
  m.controllers=static_cast<std::uint32_t>(Get(p,40,4));m.motors=static_cast<std::uint32_t>(Get(p,44,4));m.error=static_cast<std::uint32_t>(Get(p,48,4));
  m.device_epoch=static_cast<std::uint32_t>(Get(p,52,4));
  if(!m.sequence||m.motors>15) return false;
  float values[4]{};
  for(unsigned i=0;i<4;++i) {
    const auto bits=static_cast<std::uint32_t>(Get(p,56+i*4,4));std::memcpy(&values[i],&bits,4);
    if(!std::isfinite(values[i])||values[i]<0||values[i]>1||((m.kind!=Kind::Frame||!m.active)&&values[i]!=0)) return false;
  }
  if(m.kind!=Kind::Poll&&(m.controllers||m.motors||m.error||m.device_epoch||m.zeroed||!m.generation)) return false;
  if(m.kind==Kind::Stop&&m.active) return false;
  m.frame={values[0],values[1],values[2],values[3]};output=m;return true;
}
inline bool Eligible(unsigned controllers,bool xbox,unsigned motors,bool engines) noexcept {
  return controllers==1&&xbox&&motors==15&&engines;
}
inline bool Apply(HapticFrame frame,void* context,bool (*actuator)(void*,unsigned,float)) noexcept {
  if(!actuator) return false;
  frame=HapticScale(frame,1);const float values[]{frame.low,frame.high,frame.left,frame.right};bool result=true;
  for(unsigned i=0;i<4;++i) if(!actuator(context,i,values[i])) result=false;
  return result;
}
inline bool Matches(const Message& message,const Token& token,std::uint64_t sequence,std::uint64_t elapsed) noexcept {
  return (message.kind==Kind::Frame||message.kind==Kind::Stop)&&message.token==token&&message.sequence==sequence&&elapsed<kPollDeadlineMs;
}
// All clocks are supplied by the boundary. A finite native pulse plus the poll
// deadline fits inside the fallback fence, even when a final reply is delayed.
class Route {
 public:
  void Observe(const Message& m,std::uint64_t now) noexcept {
    if(seen_&&epoch_!=m.device_epoch&&(active_||acquiring_)) {
      if(active_) { draining_=true;released_at_=now; }
      active_=false;acquiring_=false;++generation_;
    }
    epoch_=m.device_epoch;
    seen_=true;seen_at_=now;candidate_=m.active&&m.controllers==1&&m.motors==15&&!m.error;
    if(!active_&&draining_&&m.zeroed&&m.generation==generation_) draining_=false;
  }
  bool Select(bool allowed,std::uint64_t now) noexcept {
    const bool fresh=seen_&&now>=seen_at_&&now-seen_at_<kClientLeaseMs;
    if(draining_&&now>=released_at_&&now-released_at_>=kFallbackFenceMs) draining_=false;
    const bool wanted=allowed&&fresh&&candidate_;
    if(!wanted&&(active_||acquiring_)) {
      if(active_) { draining_=true;released_at_=now; }
      active_=false;acquiring_=false;++generation_;
    }
    if(wanted&&!active_&&!draining_&&!acquiring_) { acquiring_=true;acquire_at_=now;++generation_; }
    // Quiesce the previous GameInput/XInput provider before activating native
    // engines. The asynchronous Microsoft provider has no stop acknowledgement.
    if(acquiring_&&now>=acquire_at_&&now-acquire_at_>=kAcquireFenceMs) { acquiring_=false;active_=true;++generation_; }
    return active_;
  }
  bool Active() const noexcept { return active_; }
  bool BlocksFallback() const noexcept { return active_||draining_||acquiring_; }
  bool Acquiring() const noexcept { return acquiring_; }
  std::uint64_t Generation() const noexcept { return generation_; }
 private:
  bool seen_=false,candidate_=false,active_=false,draining_=false,acquiring_=false;
  std::uint64_t seen_at_=0,released_at_=0,generation_=1,acquire_at_=0;
  std::uint32_t epoch_=0;
};
// The helper exits after two seconds without a reply (a stalled game worker,
// a lost controller, a crash). Relaunch it only after it has answered at least
// once since its last launch: a helper that never answers (blocked by
// Gatekeeper, missing) is never retried. Attempts are spaced and capped, so a
// helper that keeps dying cannot become a launch loop.
constexpr std::uint64_t kHelperSilentMs = 2500, kRelaunchSpacingMs = 10000;
constexpr unsigned kMaxRelaunches = 20;
class Relaunch {
 public:
  void Launched(std::uint64_t now) noexcept { launched_=true;seen_=false;launched_at_=now; }
  void Seen(std::uint64_t now) noexcept { if(launched_) { seen_=true;heard_at_=now; } }
  bool Due(std::uint64_t now) const noexcept {
    return launched_&&seen_&&attempts_<kMaxRelaunches&&now>=heard_at_&&now-heard_at_>=kHelperSilentMs&&
           now>=launched_at_&&now-launched_at_>=kRelaunchSpacingMs;
  }
  // Counts the attempt even if the launch itself fails, so Due stays false.
  void Attempted(std::uint64_t now) noexcept { ++attempts_;Launched(now); }
  unsigned Attempts() const noexcept { return attempts_; }
 private:
  bool launched_=false,seen_=false;
  std::uint64_t launched_at_=0,heard_at_=0;
  unsigned attempts_=0;
};
class Receiver {
 public:
  void Begin(std::uint64_t sequence,std::uint64_t now) noexcept { sequence_=sequence;sent_at_=now;pending_=true; }
  bool Accept(const Message& m,const Token& token,std::uint64_t now) noexcept {
    if(!pending_||now<sent_at_||!Matches(m,token,sequence_,now-sent_at_)||m.generation<generation_) return false;
    pending_=false;generation_=m.generation;return true;
  }
  bool Pending(std::uint64_t now) const noexcept { return pending_&&now>=sent_at_&&now-sent_at_<kPollDeadlineMs; }
  std::uint64_t Generation() const noexcept { return generation_; }
  void Cancel() noexcept { pending_=false; }
 private:
  bool pending_=false;
  std::uint64_t sequence_=0,sent_at_=0,generation_=0;
};
} // namespace edtrace::macbridge
