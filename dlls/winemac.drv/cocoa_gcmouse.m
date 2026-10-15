/*
 * MACDRV GameController mouse support
 *
 * Copyright 2026 Elvin Hayatov
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

#import <GameController/GameController.h>
#include <os/lock.h>

#import "cocoa_app.h"
#import "cocoa_event.h"
#import "cocoa_window.h"

#pragma GCC diagnostic ignored "-Wdeclaration-after-statement"

WINE_DEFAULT_DEBUG_CHANNEL(event);


/* The movement a mouse reports to GameController is the raw input for it,
   unaccelerated device counts as Windows reports them, while the movement of
   its NSEvents is the cursor's.  An NSEvent move counts as covered by
   GameController when a GameController mouse moved within this many
   milliseconds of it.  Movement of a device GameController does not report
   keeps its raw input from the cursor movement. */
#define GCMOUSE_COVER_MS 100

/* Written by the GameController handler queue and the main thread. */
static os_unfair_lock gcmouse_lock = OS_UNFAIR_LOCK_INIT;
static double gcmouse_accum_x, gcmouse_accum_y;  /* not yet posted, incl. fractions */
static BOOL gcmouse_post_pending;
/* Event time of the last GameController movement, 0 for none yet. */
static uint32_t gcmouse_last_move;

/* Main thread only. */
static BOOL gcmouse_started;
static dispatch_queue_t gcmouse_queue;
static NSMutableSet* gcmouse_mice;


/***********************************************************************
 *              gcmouse_post_movement
 *
 * Run on the main thread: post the whole counts of the movement collected
 * since the last call as a MOUSE_MOVED_RAW event, and keep the fractions.
 */
static void gcmouse_post_movement(void)
{
    WineApplicationController* controller = [WineApplicationController sharedController];
    WineWindow* window;
    macdrv_event* event;
    int x, y;

    os_unfair_lock_lock(&gcmouse_lock);
    x = gcmouse_accum_x;
    y = gcmouse_accum_y;
    gcmouse_accum_x -= x;
    gcmouse_accum_y -= y;
    gcmouse_post_pending = FALSE;
    os_unfair_lock_unlock(&gcmouse_lock);

    if (!x && !y) return;
    if (![NSApp isActive] || [NSApp isHidden]) return;
    if (!(window = [controller rawMouseTargetWindow])) return;

    event = macdrv_create_event(MOUSE_MOVED_RAW, window);
    event->mouse_moved.x = x;
    event->mouse_moved.y = y;
    event->mouse_moved.time_ms = __atomic_load_n(&gcmouse_last_move, __ATOMIC_ACQUIRE);
    [window.queue postEvent:event];
    macdrv_release_event(event);
}


/***********************************************************************
 *              gcmouse_moved
 *
 * Run on gcmouse_queue for every movement a GameController mouse reports.
 * Only collects it; the main thread picks the window and posts it.
 */
static void gcmouse_moved(float delta_x, float delta_y)
{
    WineApplicationController* controller = [WineApplicationController sharedController];
    uint32_t time = [controller ticksForEventTime:[[NSProcessInfo processInfo] systemUptime]];
    BOOL post;

    if (!delta_x && !delta_y) return;

    os_unfair_lock_lock(&gcmouse_lock);
    gcmouse_accum_x += delta_x;
    gcmouse_accum_y -= delta_y;  /* GameController's y axis points up */
    post = !gcmouse_post_pending;
    gcmouse_post_pending = TRUE;
    os_unfair_lock_unlock(&gcmouse_lock);

    __atomic_store_n(&gcmouse_last_move, time ? time : 1, __ATOMIC_RELEASE);

    if (post)
        OnMainThreadAsync(^{ gcmouse_post_movement(); });
}


static void gcmouse_add(GCMouse* mouse)
{
    GCMouseInput* input = [mouse mouseInput];

    if (!input || [gcmouse_mice containsObject:mouse]) return;

    TRACE("adding %s\n", [[mouse description] UTF8String]);
    [gcmouse_mice addObject:mouse];
    [mouse setHandlerQueue:gcmouse_queue];
    [input setMouseMovedHandler:^(GCMouseInput* moved_input, float delta_x, float delta_y){
        gcmouse_moved(delta_x, delta_y);
    }];
}


static void gcmouse_remove(GCMouse* mouse)
{
    if (![gcmouse_mice containsObject:mouse]) return;

    TRACE("removing %s\n", [[mouse vendorName] UTF8String]);
    [[mouse mouseInput] setMouseMovedHandler:nil];
    [gcmouse_mice removeObject:mouse];
}


/***********************************************************************
 *              macdrv_start_gcmouse
 *
 * Run on the main thread when the application first becomes active: watch
 * the mice GameController knows, which are connected and announced
 * asynchronously.  Does nothing unless compatdb sets gcmouse to true.
 */
void macdrv_start_gcmouse(void)
{
    NSNotificationCenter* center;
    NSOperationQueue* mainQueue;
    dispatch_queue_attr_t attr;

    if (!use_gcmouse || gcmouse_started) return;
    gcmouse_started = TRUE;

    center = [NSNotificationCenter defaultCenter];
    mainQueue = [NSOperationQueue mainQueue];

    attr = dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INTERACTIVE, 0);
    gcmouse_queue = dispatch_queue_create("org.winehq.wine.winemac.gcmouse", attr);
    gcmouse_mice = [[NSMutableSet alloc] init];

    [center addObserverForName:GCMouseDidConnectNotification object:nil queue:mainQueue
                    usingBlock:^(NSNotification* note){
        gcmouse_add([note object]);
    }];
    [center addObserverForName:GCMouseDidDisconnectNotification object:nil queue:mainQueue
                    usingBlock:^(NSNotification* note){
        gcmouse_remove([note object]);
    }];

    for (GCMouse* mouse in [GCMouse mice])
        gcmouse_add(mouse);
}


/***********************************************************************
 *              macdrv_gcmouse_covers_movement
 *
 * Whether a GameController mouse reports the movement of a mouse event
 * from the given time, so that its raw input comes from MOUSE_MOVED_RAW.
 * Called on Wine threads.
 */
bool macdrv_gcmouse_covers_movement(unsigned long time)
{
    uint32_t last = __atomic_load_n(&gcmouse_last_move, __ATOMIC_ACQUIRE);

    /* A move from before the last GameController movement counts too. */
    return last && (int32_t)((uint32_t)time - last) < GCMOUSE_COVER_MS;
}
