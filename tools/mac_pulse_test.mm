// Pulses each controller actuator in turn (left handle, right handle, left
// trigger, right trigger) through Core Haptics, the API the ED companion app
// uses, speaking each name first. Proves the Mac can physically drive all four
// motors, independent of the game and the mod. Run via tools/mac_pulse_test.sh.
#import <Foundation/Foundation.h>
#import <GameController/GameController.h>
#import <CoreHaptics/CoreHaptics.h>
#import <AppKit/AppKit.h>

static void Say(NSString* text) {
  NSTask* task = [NSTask launchedTaskWithLaunchPath:@"/usr/bin/say" arguments:@[ text ]];
  [task waitUntilExit];
}

int main() {
  @autoreleasepool {
    [NSApplication sharedApplication];
    GCController.shouldMonitorBackgroundEvents = YES;
    NSDate* deadline = [NSDate dateWithTimeIntervalSinceNow:8];
    while (GCController.controllers.count == 0 && [deadline timeIntervalSinceNow] > 0)
      [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.2]];
    if (GCController.controllers.count == 0) { printf("no controller found\n"); return 1; }
    GCController* controller = GCController.controllers.firstObject;
    printf("controller: %s\n", controller.vendorName.UTF8String ?: "?");
    GCDeviceHaptics* haptics = controller.haptics;
    if (!haptics) { printf("controller reports no haptics\n"); return 1; }
    printf("localities:");
    for (GCHapticsLocality l in haptics.supportedLocalities) printf(" %s", l.UTF8String);
    printf("\n");
    NSArray* order = @[ GCHapticsLocalityLeftHandle, GCHapticsLocalityRightHandle,
                        GCHapticsLocalityLeftTrigger, GCHapticsLocalityRightTrigger ];
    NSArray* names = @[ @"left handle", @"right handle", @"left trigger", @"right trigger" ];
    for (NSUInteger i = 0; i < order.count; ++i) {
      for (int rep = 0; rep < 2; ++rep) {
        Say(rep ? @"again" : names[i]);
        NSError* error = nil;
        CHHapticEngine* engine = [haptics createEngineWithLocality:order[i]];
        if (!engine || ![engine startAndReturnError:&error]) {
          printf("%s: engine failed %s\n", [names[i] UTF8String], error.localizedDescription.UTF8String ?: "");
          break;
        }
        CHHapticEvent* event = [[CHHapticEvent alloc]
            initWithEventType:CHHapticEventTypeHapticContinuous
                   parameters:@[ [[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticIntensity value:1] ]
                 relativeTime:0 duration:0.8];
        CHHapticPattern* pattern = [[CHHapticPattern alloc] initWithEvents:@[ event ] parameters:@[] error:&error];
        id<CHHapticPatternPlayer> player = [engine createPlayerWithPattern:pattern error:&error];
        BOOL started = player && [player startAtTime:CHHapticTimeImmediate error:&error];
        printf("%s pulse %d: %s %s\n", [names[i] UTF8String], rep + 1, started ? "played" : "FAILED",
               error.localizedDescription.UTF8String ?: "");
        [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:1.2]];
        [engine stopWithCompletionHandler:nil];
        [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.5]];
      }
    }
    Say(@"done");
  }
  return 0;
}
