//
//  JadeCrashException.m
//  Jade
//
//  Objective-C exception handler. Swift cannot pass a closure where
//  NSSetUncaughtExceptionHandler wants a C function pointer, so the
//  registration and the handler both live here.
//

#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>

#import "JadeCrash.h"

static void JadeHandleUncaughtException(NSException *exception) {
    @autoreleasepool {
        NSMutableString *report = [NSMutableString stringWithCapacity:4096];

        [report appendString:@"--- uncaught exception ---\n"];
        [report appendFormat:@"name: %@\n", exception.name ?: @"(null)"];
        [report appendFormat:@"reason: %@\n", exception.reason ?: @"(null)"];
        [report appendFormat:@"class: %@\n", NSStringFromClass([exception class])];

        NSDictionary *userInfo = exception.userInfo;
        if (userInfo.count > 0) {
            [report appendFormat:@"userInfo: %@\n", userInfo];
        }

        NSArray<NSString *> *exceptionStack = exception.callStackSymbols;
        if (exceptionStack.count > 0) {
            [report appendString:@"\n--- exception call stack ---\n"];
            for (NSString *line in exceptionStack) {
                [report appendFormat:@"%@\n", line];
            }
        }

        NSArray<NSString *> *threadStack = [NSThread callStackSymbols];
        if (threadStack.count > 0) {
            [report appendString:@"\n--- current thread call stack ---\n"];
            for (NSString *line in threadStack) {
                [report appendFormat:@"%@\n", line];
            }
        }

        JadeCrashWriteReport("exception", report.UTF8String);
    }
}

void JadeCrashInstallExceptionHandler(void) {
    NSSetUncaughtExceptionHandler(&JadeHandleUncaughtException);
}
