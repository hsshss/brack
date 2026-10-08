#include "util/main_thread.h"

#import <AppKit/AppKit.h>

#include <thread>

namespace brack {

// NSApplication's loop, not only the run loop: plugin editors get their events through it.
int runWithMainLoop(const std::function<int()>& body) {
    int result = 0;
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
        // A plugin host shows no window and plays no audio of its own, which App Nap would slow down.
        id<NSObject> activity = [[NSProcessInfo processInfo]
            beginActivityWithOptions:NSActivityLatencyCritical | NSActivityUserInitiatedAllowingIdleSystemSleep
                              reason:@"Brack runs audio plugins"];
        std::thread worker([&] {
            result = body();
            dispatch_async(dispatch_get_main_queue(), ^{
                [NSApp stop:nil];
                // stop: takes effect after the next event: send one.
                [NSApp postEvent:[NSEvent otherEventWithType:NSEventTypeApplicationDefined location:NSZeroPoint
                                               modifierFlags:0 timestamp:0 windowNumber:0 context:nil
                                                     subtype:0 data1:0 data2:0]
                         atStart:YES];
            });
        });
        [NSApp run];
        worker.join();
        [[NSProcessInfo processInfo] endActivity:activity];
    }
    return result;
}

}  // namespace brack
