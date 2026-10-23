/*
 * MACDRV Cocoa initialization code
 *
 * Copyright 2011, 2012, 2013 Ken Thomases for CodeWeavers Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#import "config.h"
#import "macdrv.h"

#import <AppKit/AppKit.h>
#include <mach/mach.h>
#include <mach/mach_time.h>

#import "cocoa_app.h"

#pragma GCC diagnostic ignored "-Wdeclaration-after-statement"


/* Condition values for an NSConditionLock. Used to signal between run_cocoa_app
   and macdrv_start_cocoa_app so the latter knows when the former is running
   the application event loop. */
enum {
    COCOA_APP_NOT_RUNNING,
    COCOA_APP_RUNNING,
};


/* Shared by macdrv_start_cocoa_app and run_cocoa_app, which can run after the
   former gave up waiting for it.  It is reference counted: one reference for
   macdrv_start_cocoa_app and one for the run loop source whose perform
   callback is run_cocoa_app.  The fields after lock are guarded by it. */
struct cocoa_app_startup_info {
    NSConditionLock*    lock;
    unsigned long long  tickcount;
    uint64_t            uptime_ns;
    BOOL                success;
    BOOL                abandoned;
    int                 refcount;
};


static const void* retain_startup_info(const void* info)
{
    struct cocoa_app_startup_info* startup_info = (struct cocoa_app_startup_info*)info;
    __atomic_add_fetch(&startup_info->refcount, 1, __ATOMIC_RELAXED);
    return info;
}


static void release_startup_info(const void* info)
{
    struct cocoa_app_startup_info* startup_info = (struct cocoa_app_startup_info*)info;
    if (__atomic_sub_fetch(&startup_info->refcount, 1, __ATOMIC_ACQ_REL)) return;
    [startup_info->lock release];
    free(startup_info);
}


/***********************************************************************
 *              run_cocoa_app
 *
 * Transforms the main thread from merely idling in its run loop to
 * being a Cocoa application running its event loop.
 *
 * This will be the perform callback of a custom run loop source that
 * will be scheduled in the main thread's run loop from a secondary
 * thread by macdrv_start_cocoa_app.  This function communicates that
 * it has successfully started the application by changing the condition
 * of a shared NSConditionLock, passed in via the info parameter.
 *
 * This function never returns, unless macdrv_start_cocoa_app gave up
 * waiting for it.  It's the new permanent home of the main thread.
 */
static void run_cocoa_app(void* info)
{
    struct cocoa_app_startup_info* startup_info = info;
    NSConditionLock* lock = startup_info->lock;
    BOOL created_app = FALSE;
    BOOL success = FALSE;

    /* The lock is held while the application is set up, so that
       macdrv_start_cocoa_app, if it times out meanwhile, waits for the
       result instead of giving up on an application that is starting. */
    [lock lock];
    if (startup_info->abandoned)
    {
        /* macdrv_start_cocoa_app has failed already; the process goes on
           without the Mac driver, so don't start an application for it. */
        [lock unlock];
        return;
    }

    /* If the setup raises an exception, macdrv_start_cocoa_app still gets
       the lock back, with success unset, instead of waiting for it forever. */
    @try
    {
        @autoreleasepool
        {
            if (!NSApp)
            {
                [WineApplication sharedApplication];
                created_app = TRUE;
            }

            /* Without a value for this default, AppKit looks up the version it
               was linked against with NSVersionOfRunTimeLibrary(), which walks
               the loaded images and can crash if a Wine thread unloads a
               library at the same time. */
            [[NSUserDefaults standardUserDefaults] registerDefaults:@{ @"NSUseActiveDisplayForMainScreen": @YES }];

            if ([NSApp respondsToSelector:@selector(setWineController:)])
            {
                WineApplicationController* controller = [WineApplicationController sharedController];
                [NSApp setWineController:controller];
                [controller computeEventTimeAdjustmentFromTicks:startup_info->tickcount uptime:startup_info->uptime_ns];
                startup_info->success = TRUE;
            }
        }
    }
    @finally
    {
        success = startup_info->success;
        [lock unlockWithCondition:COCOA_APP_RUNNING];
    }

    if (created_app && success)
    {
        @autoreleasepool
        {
            /* Never returns */
            [NSApp run];
        }
    }
}


/***********************************************************************
 *              macdrv_start_cocoa_app
 *
 * Tells the main thread to transform itself into a Cocoa application.
 *
 * Returns 0 on success, non-zero on failure.
 */
int macdrv_start_cocoa_app(unsigned long long tickcount)
{
@autoreleasepool
{
    int ret = -1;
    CFRunLoopSourceRef source = NULL;
    struct cocoa_app_startup_info* startup_info;
    uint64_t uptime_mach = mach_absolute_time();
    mach_timebase_info_data_t mach_timebase;
    NSDate* timeLimit;
    CFRunLoopSourceContext source_context = { 0 };

    /* Make sure Cocoa is in multi-threading mode by detaching a
       do-nothing thread. */
    [NSThread detachNewThreadSelector:@selector(self)
                             toTarget:[NSThread class]
                           withObject:nil];

    if (!(startup_info = calloc(1, sizeof(*startup_info))))
        return -1;
    startup_info->refcount = 1;
    startup_info->lock = [[NSConditionLock alloc] initWithCondition:COCOA_APP_NOT_RUNNING];
    startup_info->tickcount = tickcount;

    mach_timebase_info(&mach_timebase);
    startup_info->uptime_ns = uptime_mach * mach_timebase.numer / mach_timebase.denom;

    timeLimit = [NSDate dateWithTimeIntervalSinceNow:5];

    /* The source holds its own reference to startup_info, so run_cocoa_app
       can still use it when it runs after this function gave up. */
    source_context.info = startup_info;
    source_context.retain = retain_startup_info;
    source_context.release = release_startup_info;
    source_context.perform = run_cocoa_app;
    if (startup_info->lock)
        source = CFRunLoopSourceCreate(NULL, 0, &source_context);

    if (source && timeLimit)
    {
        CFRunLoopAddSource(CFRunLoopGetMain(), source, kCFRunLoopCommonModes);
        CFRunLoopSourceSignal(source);
        CFRunLoopWakeUp(CFRunLoopGetMain());

        if ([startup_info->lock lockWhenCondition:COCOA_APP_RUNNING beforeDate:timeLimit])
        {
            ret = !startup_info->success;
            [startup_info->lock unlock];
        }
        else
        {
            /* The main thread has not set up the application in time.  If it
               is doing so right now, this waits for it to finish; otherwise
               it must not start an application later. */
            [startup_info->lock lock];
            if ([startup_info->lock condition] == COCOA_APP_RUNNING)
                ret = !startup_info->success;
            else
                startup_info->abandoned = TRUE;
            [startup_info->lock unlock];
            if (startup_info->abandoned)
                CFRunLoopSourceInvalidate(source);
        }
    }

    if (source)
        CFRelease(source);
    release_startup_info(startup_info);
    return ret;
}
}
