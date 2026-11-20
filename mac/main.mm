#import <AppKit/AppKit.h>
#import <CoreHaptics/CoreHaptics.h>
#import <GameController/GameController.h>
#include "edtrace/mac_controller_check.hpp"
#include "edtrace/mac_haptics.hpp"
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace mb=edtrace::macbridge;
using edtrace::HapticFrame;
static std::uint64_t Now() {
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

@interface EDMotor : NSObject
@property(nonatomic,strong) CHHapticEngine* engine;
@property(nonatomic,strong) id<CHHapticPatternPlayer> player;
@property(nonatomic) BOOL healthy;
@property(nonatomic) float intensity;
@property(nonatomic) NSInteger errorCode;
@property(nonatomic) NSInteger stoppedReason;
-(instancetype)initWithHaptics:(GCDeviceHaptics*)haptics locality:(GCHapticsLocality)locality;
-(BOOL)apply:(float)value;
-(void)close;
@end
@implementation EDMotor
-(instancetype)initWithHaptics:(GCDeviceHaptics*)haptics locality:(GCHapticsLocality)locality {
  self=[super init];if(!self) return nil;
  self.engine=[haptics createEngineWithLocality:locality];
  if(!self.engine) { self.errorCode=-1;return self; }
  self.engine.playsHapticsOnly=YES;self.engine.autoShutdownEnabled=NO;
  __weak EDMotor* weakSelf=self;
  self.engine.stoppedHandler=^(CHHapticEngineStoppedReason reason) {
    dispatch_async(dispatch_get_main_queue(),^{ EDMotor* motor=weakSelf;if(motor) { motor.healthy=NO;motor.stoppedReason=reason; } });
  };
  self.engine.resetHandler=^{
    dispatch_async(dispatch_get_main_queue(),^{ EDMotor* motor=weakSelf;if(motor) { motor.healthy=NO;motor.errorCode=-2; } });
  };
  NSError* error=nil;
  if(![self.engine startAndReturnError:&error]) { self.errorCode=error.code;[self close];return self; }
  CHHapticEventParameter* parameter=[[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticIntensity value:1];
  CHHapticEvent* event=[[CHHapticEvent alloc] initWithEventType:CHHapticEventTypeHapticContinuous parameters:@[parameter] relativeTime:0 duration:mb::kPulseSeconds];
  CHHapticPattern* pattern=[[CHHapticPattern alloc] initWithEvents:@[event] parameters:@[] error:&error];
  if(pattern) self.player=[self.engine createPlayerWithPattern:pattern error:&error];
  if(!self.player||error) { self.errorCode=error.code;[self close];return self; }
  self.healthy=YES;return self;
}
// Core Haptics raises an Objective-C exception, rather than returning an
// error, when a player's engine has already stopped (for example after the
// controller disconnects). Uncaught, that aborts the companion, so every
// player call treats an exception as an unhealthy motor.
static constexpr NSInteger kEngineException=-3;
-(BOOL)apply:(float)value {
  if(!self.healthy) return NO;
  value=edtrace::HapticUnit(value);NSError* error=nil;
  @try {
    if(value==0) {
      if(self.intensity>0&&![self.player stopAtTime:CHHapticTimeImmediate error:&error]) { self.healthy=NO;self.errorCode=error.code;return NO; }
    } else {
      CHHapticDynamicParameter* parameter=[[CHHapticDynamicParameter alloc] initWithParameterID:CHHapticDynamicParameterIDHapticIntensityControl value:value relativeTime:0];
      if(![self.player sendParameters:@[parameter] atTime:CHHapticTimeImmediate error:&error]||
         ![self.player startAtTime:CHHapticTimeImmediate error:&error]) { self.healthy=NO;self.errorCode=error.code;return NO; }
      // Restarting an already-playing player restarts its finite pattern. A
      // helper crash cannot leave an infinite controller request behind.
    }
  } @catch(NSException*) { self.healthy=NO;self.errorCode=kEngineException;return NO; }
  self.intensity=value;return YES;
}
-(void)close {
  self.engine.stoppedHandler=^(CHHapticEngineStoppedReason reason) { (void)reason; };self.engine.resetHandler=^{};
  @try { [self.player stopAtTime:CHHapticTimeImmediate error:nil]; } @catch(NSException*) {}
  @try { [self.engine stopWithCompletionHandler:nil]; } @catch(NSException*) {}
  self.player=nil;self.engine=nil;self.healthy=NO;
}
@end

@interface EDDevice : NSObject
@property(nonatomic,strong) GCController* controller;
@property(nonatomic,strong) NSArray<EDMotor*>* motors;
@property(nonatomic) unsigned count;
@property(nonatomic) unsigned mask;
@property(nonatomic) unsigned status;
@property(nonatomic) unsigned epoch;
@property(nonatomic) unsigned failedActuator;
@property(nonatomic) unsigned playFailures;
@property(nonatomic) BOOL disabled;
@property(nonatomic) std::uint64_t nextDiscovery;
@property(nonatomic) std::uint64_t uncertainUntil;
@property(nonatomic,copy) NSString* label;
@property(nonatomic) NSInteger engineError,stoppedReason;
-(void)refresh:(std::uint64_t)now;
-(void)refresh:(std::uint64_t)now controllers:(NSArray<GCController*>*)connected;
-(BOOL)ready;
-(BOOL)quiet:(std::uint64_t)now;
-(BOOL)render:(HapticFrame)frame now:(std::uint64_t)now;
-(void)zero:(std::uint64_t)now;
-(void)close:(std::uint64_t)now;
@end
@implementation EDDevice
-(instancetype)init {
  self=[super init];if(!self) return nil;self.epoch=1;self.label=@"";
  [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(changed:) name:GCControllerDidConnectNotification object:nil];
  [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(changed:) name:GCControllerDidDisconnectNotification object:nil];
  return self;
}
-(void)changed:(NSNotification*)notification {
  self.nextDiscovery=0;self.playFailures=0;self.disabled=NO;
  if([notification.name isEqualToString:GCControllerDidDisconnectNotification]&&notification.object==self.controller) [self close:Now()];
}
-(void)dealloc { [[NSNotificationCenter defaultCenter] removeObserver:self]; }
-(BOOL)ready {
  if(self.disabled||self.status||self.motors.count!=4) return NO;
  for(EDMotor* motor in self.motors) if(!motor.healthy) { self.status=5;self.engineError=motor.errorCode;self.stoppedReason=motor.stoppedReason;return NO; }
  return YES;
}
-(void)refresh:(std::uint64_t)now {
  if(now<self.nextDiscovery) return;
  [self refresh:now controllers:[GCController controllers]];
}
-(void)refresh:(std::uint64_t)now controllers:(NSArray<GCController*>*)connected {
  self.nextDiscovery=now+1000;
  self.count=static_cast<unsigned>(connected.count);GCController* candidate=connected.count==1?connected.firstObject:nil;
  const bool xbox=candidate&&[candidate.extendedGamepad isKindOfClass:[GCXboxGamepad class]];
  NSArray<GCHapticsLocality>* localities=@[GCHapticsLocalityLeftHandle,GCHapticsLocalityRightHandle,GCHapticsLocalityLeftTrigger,GCHapticsLocalityRightTrigger];
  unsigned mask=0;
  for(unsigned i=0;i<4;++i) if([candidate.haptics.supportedLocalities containsObject:localities[i]]) mask|=1u<<i;
  self.mask=mask;
  if(candidate!=self.controller) { self.disabled=NO;self.playFailures=0; }
  if(candidate==self.controller&&self.disabled) return;
  if(candidate==self.controller&&mb::Eligible(self.count,xbox,mask,[self ready])) return;
  [self close:now];self.controller=candidate;NSString* name=candidate.vendorName;self.label=name?name:@"";
  if(self.label.length>128) self.label=[self.label substringToIndex:128];
  self.status=!self.count?1:self.count!=1?2:!xbox?3:mask!=15?4:5;
  if(!mb::Eligible(self.count,xbox,mask,true)) return;
  NSMutableArray<EDMotor*>* built=[NSMutableArray arrayWithCapacity:4];
  for(GCHapticsLocality locality in localities) {
    EDMotor* motor=[[EDMotor alloc] initWithHaptics:candidate.haptics locality:locality];
    if(!motor||!motor.healthy) {
      self.failedActuator=static_cast<unsigned>(built.count)+1;self.engineError=motor.errorCode;
      [motor close];for(EDMotor* previous in built) [previous close];return;
    }
    [built addObject:motor];
  }
  self.motors=built;self.status=0;self.failedActuator=0;
}
-(void)zero:(std::uint64_t)now {
  for(EDMotor* motor in self.motors) if(![motor apply:0]) {
    self.uncertainUntil=now+150;self.engineError=motor.errorCode;self.stoppedReason=motor.stoppedReason;
  }
}
-(BOOL)quiet:(std::uint64_t)now {
  if(now<self.uncertainUntil) return NO;
  for(EDMotor* motor in self.motors) if(motor.intensity>0) return NO;
  return YES;
}
-(BOOL)render:(HapticFrame)frame now:(std::uint64_t)now {
  if(![self ready]) { [self zero:now];return NO; }
  const BOOL success=mb::Apply(frame,(__bridge void*)self,[](void* context,unsigned index,float value) {
    EDDevice* device=(__bridge EDDevice*)context;return [device.motors[index] apply:value]!=NO;
  });
  if(!success) {
    self.status=5;[self zero:now];++self.playFailures;self.disabled=self.playFailures>=3;
    self.nextDiscovery=now+10000;
  }
  return success;
}
-(void)close:(std::uint64_t)now {
  [self zero:now];for(EDMotor* motor in self.motors) [motor close];
  self.motors=nil;self.controller=nil;++self.epoch;
}
@end

static bool ParseToken(const char* text,mb::Token& token) {
  if(std::strlen(text)!=32) return false;
  auto digit=[](char ch)->int { if(ch>='0'&&ch<='9') return ch-'0';if(ch>='a'&&ch<='f') return ch-'a'+10;return -1; };
  for(unsigned i=0;i<16;++i) { const int a=digit(text[i*2]),b=digit(text[i*2+1]);if(a<0||b<0) return false;token[i]=static_cast<unsigned char>(a*16+b); }
  return token!=mb::Token{};
}
static void Status(NSString* path,EDDevice* device,bool active,std::uint64_t frames) {
  if(!path.length) return;
  NSDictionary* object=@{@"schema":@1,@"backend":@"macos-corehaptics",@"controllers":@(device.count),
    @"motor_mask":@(device.mask),@"ready":@([device ready]),@"active":@(active),@"status":@(device.status),
    @"device_epoch":@(device.epoch),@"controller":device.label?device.label:@"",@"engine_error":@(device.engineError),
    @"stopped_reason":@(device.stoppedReason),@"failed_actuator":@(device.failedActuator),@"frames":@(frames),@"physical_output_verified":@NO,
    @"playback_failures":@(device.playFailures),@"native_disabled":@(device.disabled),
    @"selection_policy":@"sole-native-xbox",@"transport":@"not-queried",@"input_values_read":@NO};
  NSData* data=[NSJSONSerialization dataWithJSONObject:object options:NSJSONWritingPrettyPrinted error:nil];
  if(data) [data writeToFile:path options:NSDataWritingAtomic error:nil];
}
static int SelfTest() {
  if(mb::Eligible(2,true,15,true)||mb::Eligible(1,false,15,true)||mb::Eligible(1,true,3,true)||!mb::Eligible(1,true,15,true)) return 1;
  mb::Message response{};response.kind=mb::Kind::Frame;response.token.fill(7);response.sequence=3;response.generation=2;response.active=true;response.frame={.2f,.3f,.4f,.5f};
  auto bytes=mb::Encode(response);mb::Message decoded{};
  if(!mb::Decode(bytes.data(),bytes.size(),decoded)||decoded.frame.low!=.2f||decoded.frame.high!=.3f||decoded.frame.left!=.4f||decoded.frame.right!=.5f) return 2;
  std::array<float,4> delivered{};
  if(!mb::Apply(decoded.frame,&delivered,[](void* context,unsigned index,float value) { (*static_cast<std::array<float,4>*>(context))[index]=value;return true; })||
    delivered[0]!=.2f||delivered[1]!=.3f||delivered[2]!=.4f||delivered[3]!=.5f) return 4;
  mb::Receiver receiver;receiver.Begin(3,100);
  if(!receiver.Accept(decoded,response.token,120)||receiver.Accept(decoded,response.token,121)) return 3;
  std::puts("PASS native companion protocol/capability self-test; no controller APIs called");return 0;
}
static NSString* CapabilityStatus(EDDevice* device) {
  switch(device.status) {
    case 0: return @"All four actuator engines are ready.";
    case 1: return @"GameController has yet to report a connected controller.";
    case 2: return @"GameController reports multiple controllers. Native output requires a single controller.";
    case 3: return @"The native bridge requires an Xbox controller profile.";
    case 4: return @"The device reports fewer than four separate actuator localities.";
    default: return [NSString stringWithFormat:@"Core Haptics engine setup failed (error %ld).",
                     static_cast<long>(device.engineError)];
  }
}
static int ShowCapabilityCheck(EDDevice* device) {
  [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
  bool launched=false;
  for (;;) {
    @autoreleasepool {
      NSArray<GCController*>* connected=mb::CheckConnectedControllers([&] {
        if(!launched) { [NSApp finishLaunching];launched=true; }
        [NSApp activateIgnoringOtherApps:YES];
      }, [](double seconds) {
        [[NSRunLoop mainRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:seconds]];
      }, [] {
        return [GCController controllers];
      });
      [device refresh:Now() controllers:connected];
      [device ready];  // Include an engine failure reported during discovery.
      NSString* controller=device.label.length ? device.label : @"—";
      NSAlert* alert=[[NSAlert alloc] init];
      alert.messageText=@"EDUP native haptics capability check";
      alert.informativeText=[NSString stringWithFormat:
        @"Connected controllers: %u\nDetected device: %@\nActuator mask: %u (15 includes all four)\n\n%@\n\nThis check leaves vibration idle.",
        device.count,controller,device.mask,CapabilityStatus(device)];
      [alert addButtonWithTitle:@"Done"];
      [alert addButtonWithTitle:@"Retry"];
      const auto result=[alert runModal];
      [device close:Now()];
      if(result!=NSAlertSecondButtonReturn) return 0;
    }
  }
}
int main(int argc,char** argv) {
  @autoreleasepool {
    if(argc==2&&std::strcmp(argv[1],"--self-test")==0) return SelfTest();
    [NSApplication sharedApplication];
    EDDevice* device=[[EDDevice alloc] init];
    if(argc==1) return ShowCapabilityCheck(device);
    [NSApp setActivationPolicy:NSApplicationActivationPolicyProhibited];
    [NSApp finishLaunching];
    unsigned port=0;mb::Token token{};NSString* statusPath=nil;
    if(argc!=7) return 64;
    for(int i=1;i<argc;i+=2) {
      if(!std::strcmp(argv[i],"--port")) { char* end=nullptr;const auto value=std::strtoul(argv[i+1],&end,10);if(!*argv[i+1]||*end||value<1||value>65535) return 64;port=static_cast<unsigned>(value); }
      else if(!std::strcmp(argv[i],"--token")) { if(!ParseToken(argv[i+1],token)) return 64; }
      else if(!std::strcmp(argv[i],"--status")) statusPath=[NSString stringWithUTF8String:argv[i+1]];
      else return 64;
    }
    if(!port||token==mb::Token{}||!statusPath.length) return 64;
    const int channel=socket(AF_INET,SOCK_DGRAM,0);if(channel<0) return 70;
    sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);address.sin_port=htons(static_cast<std::uint16_t>(port));
    if(connect(channel,reinterpret_cast<const sockaddr*>(&address),sizeof(address))||fcntl(channel,F_SETFL,O_NONBLOCK)==-1) { close(channel);return 70; }
    mb::Receiver receiver;std::uint64_t sequence=0,frames=0,lastServer=Now(),lastStatus=0;
    unsigned epoch=0;bool active=false,done=false,sentOnce=false;
    while(!done) {
      @autoreleasepool {
        [device refresh:Now()];auto now=Now();
        if(epoch!=device.epoch) { receiver.Cancel();epoch=device.epoch; }
        for(unsigned i=0;i<8;++i) {
          unsigned char bytes[mb::kPacketSize+1]{};const auto size=recv(channel,bytes,sizeof(bytes),0);if(size<0) break;
          mb::Message response{};
          now=Now();
          if(!mb::Decode(bytes,static_cast<std::size_t>(size),response)||!receiver.Accept(response,token,now)) continue;
          lastServer=now;
          active=response.active&&[device ready]&&response.kind==mb::Kind::Frame;
          if(active) { if(![device render:response.frame now:now]) active=false;++frames; }
          else [device zero:now];
          if(response.kind==mb::Kind::Stop) { done=true;break; }
        }
        now=Now();
        if(sentOnce&&now-lastServer>=mb::kClientLeaseMs) { [device zero:now];active=false; }
        if(sentOnce&&now-lastServer>=2000) done=true;
        if(!done&&!receiver.Pending(now)) {
          mb::Message poll{};poll.token=token;poll.sequence=++sequence;poll.generation=receiver.Generation();
          poll.active=[device ready];poll.zeroed=[device quiet:now];poll.controllers=device.count;poll.motors=device.mask;poll.error=device.status;poll.device_epoch=device.epoch;
          const auto packet=mb::Encode(poll);
          if(send(channel,packet.data(),packet.size(),0)==static_cast<ssize_t>(packet.size())) {
            const auto sent=Now();receiver.Begin(sequence,sent);if(!sentOnce) lastServer=sent;sentOnce=true;
          } else if(!sentOnce&&now-lastServer>=2000) done=true;
        }
        if(now-lastStatus>=1000||done) { Status(statusPath,device,active,frames);lastStatus=now; }
        [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:[device ready]?.005:.02]];
      }
    }
    [device close:Now()];Status(statusPath,device,false,frames);close(channel);return 0;
  }
}
