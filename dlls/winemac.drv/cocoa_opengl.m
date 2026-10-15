/*
 * MACDRV Cocoa OpenGL code
 *
 * Copyright 2012, 2013 Ken Thomases for CodeWeavers Inc.
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

#import "cocoa_opengl.h"

#include "cocoa_app.h"
#include "cocoa_event.h"

#pragma GCC diagnostic ignored "-Wdeclaration-after-statement"


@interface WineOpenGLContext ()
@property (retain, nonatomic) NSView* latentView;
@property (nullable, weak) NSView *view;    /* redeclare this to avoid NSOpenGLContext's deprecation warnings */

    + (NSView*) dummyView;
    - (void) wine_updateBackingSize:(const CGSize*)size;
    - (void) wine_recreateSurfaceForView:(NSView*)view;
    - (void) wine_superClearDrawable;

@end


@implementation WineOpenGLContext
@dynamic view;
@synthesize latentView, needsUpdate, needsReattach;

    - (void) dealloc
    {
        [[self view] release];
        [latentView release];
        [super dealloc];
    }

    + (NSView*) dummyView
    {
        static NSWindow* dummyWindow;
        static NSView* dummyWindowContentView;
        static dispatch_once_t once;

        dispatch_once(&once, ^{
            OnMainThread(^{
                dummyWindow = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 100, 100)
                                                          styleMask:NSWindowStyleMaskBorderless
                                                            backing:NSBackingStoreBuffered
                                                              defer:NO];
                dummyWindowContentView = dummyWindow.contentView;
            });
        });

        return dummyWindowContentView;
    }

    // Normally, we take care that disconnecting a context from a view doesn't
    // destroy that view's GL surface (see -clearDrawableLeavingSurfaceOnScreen).
    // However, if we're using a surface backing size and that size changes, we
    // need to destroy and recreate the surface or we get weird behavior.
    - (void) resetSurfaceIfBackingSizeChanged
    {
        int view_backing[2];
        if (macdrv_get_view_backing_size((WineContentView *)self.view, view_backing) &&
            (view_backing[0] != backing_size[0] || view_backing[1] != backing_size[1]))
        {
            view_backing[0] = backing_size[0];
            view_backing[1] = backing_size[1];
            macdrv_set_view_backing_size((WineContentView *)self.view, view_backing);

            NSView* save = self.view;
            if ([NSThread isMainThread])
                [self wine_recreateSurfaceForView:save];
            else OnMainThread(^{
                [self wine_recreateSurfaceForView:save];
            });
        }
    }

    /* A view has one GL surface, which all contexts attached to it draw to.
       Clearing the drawable of an attached context destroys that surface,
       and setting the view again creates a new one, but the other contexts
       attached to the view still refer to the destroyed surface's window.
       When one of them flushes, SkyLight looks that window up and
       dereferences the result without checking it.  Keep the other contexts
       locked while the surface changes, so that none of them flushes in
       between, and point them at the new surface. */
    - (void) wine_recreateSurfaceForView:(NSView*)view
    {
        NSArray* contexts = [(NSArray*)macdrv_copy_view_opengl_contexts((WineContentView *)view) autorelease];
        NSMutableArray* others = [NSMutableArray array];

        for (WineOpenGLContext* context in contexts)
        {
            if (context != self && [context view] == view && ![others containsObject:context])
                [others addObject:context];
        }

        for (WineOpenGLContext* context in others)
            CGLLockContext(context.CGLContextObj);
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        [super clearDrawable];
        [super setView:view];
#pragma clang diagnostic pop
        for (WineOpenGLContext* context in others)
        {
            [context update];
            CGLUnlockContext(context.CGLContextObj);
        }
    }

    /* This is a no-op if either dimension of the size is zero. */
    - (void) wine_updateBackingSize:(const CGSize*)size
    {
        GLint enabled;

        if (size)
        {
            if (size->width == 0 || size->height == 0)
                return;

            if (CGLIsEnabled(self.CGLContextObj, kCGLCESurfaceBackingSize, &enabled) != kCGLNoError)
                enabled = 0;

            if (!enabled || backing_size[0] != size->width || backing_size[1] != size->height)
            {
                backing_size[0] = size->width;
                backing_size[1] = size->height;
                CGLSetParameter(self.CGLContextObj, kCGLCPSurfaceBackingSize, backing_size);
            }

            if (!enabled)
                CGLEnable(self.CGLContextObj, kCGLCESurfaceBackingSize);

            [self resetSurfaceIfBackingSizeChanged];
        }
        else
        {
            backing_size[0] = 0;
            backing_size[1] = 0;

            if (CGLIsEnabled(self.CGLContextObj, kCGLCESurfaceBackingSize, &enabled) == kCGLNoError && enabled)
               CGLDisable(self.CGLContextObj, kCGLCESurfaceBackingSize);
        }
    }

    - (void) setView:(NSView*)newView
    {
        NSView* oldView = [self view];
        if ([NSThread isMainThread])
            [super setView:newView];
        else OnMainThread(^{
            [super setView:newView];
        });
        [newView retain];
        [oldView release];
    }

    - (void) wine_superClearDrawable
    {
        [super clearDrawable];
    }

    - (void) clearDrawable
    {
        NSView* oldView = [self view];
        if ([NSThread isMainThread])
            [super clearDrawable];
        else
        {
            /* -[NSOpenGLContext dealloc] calls this method.  A block that
               captured self would be released on the main thread only after
               OnMainThread() has returned and the context has been freed, and
               that release would reach the freed context.  Capture it without
               a retain. */
            __block WineOpenGLContext* context = self;
            OnMainThread(^{
                [context wine_superClearDrawable];
            });
        }
        [oldView release];

        [self wine_updateBackingSize:NULL];
    }

    /* On at least some versions of Mac OS X, -[NSOpenGLContext clearDrawable] has the
       undesirable side effect of ordering the view's GL surface off-screen.  This isn't
       done when just changing the context's view to a different view (which I would
       think would be analogous, since the old view and surface end up without a
       context attached).  So, we finesse things by first setting the context's view to
       a different view (the content view of an off-screen window) and then letting the
       original implementation proceed. */
    - (void) clearDrawableLeavingSurfaceOnScreen
    {
        [self setView:[[self class] dummyView]];
        [self clearDrawable];
    }

    - (void) removeFromViews:(BOOL)removeViews
    {
        if ([self view])
        {
            macdrv_remove_view_opengl_context((WineContentView *)[self view], self);
            if (removeViews)
                [self clearDrawableLeavingSurfaceOnScreen];
        }
        if ([self latentView])
        {
            macdrv_remove_view_opengl_context((WineContentView *)[self latentView], self);
            if (removeViews)
                [self setLatentView:nil];
        }
        needsUpdate = FALSE;
        needsReattach = FALSE;
    }

@end


/***********************************************************************
 *              macdrv_create_opengl_context
 *
 * Returns a Cocoa OpenGL context created from a CoreGL context.  The
 * caller is responsible for calling macdrv_dispose_opengl_context()
 * when done with the context object.
 */
WineOpenGLContext *macdrv_create_opengl_context(void* cglctx)
{
@autoreleasepool
{
    return [[WineOpenGLContext alloc] initWithCGLContextObj:cglctx];
}
}

/***********************************************************************
 *              macdrv_dispose_opengl_context
 *
 * Destroys a Cocoa OpenGL context previously created by
 * macdrv_create_opengl_context();
 */
void macdrv_dispose_opengl_context(WineOpenGLContext *context)
{
@autoreleasepool
{
    [context removeFromViews:YES];
    [context release];
}
}

/***********************************************************************
 *              macdrv_make_context_current
 */
void macdrv_make_context_current(WineOpenGLContext *context, WineContentView *view, CGRect r)
{
@autoreleasepool
{
    if (context && view)
    {
        if (view == (WineContentView *)[context view] || view == (WineContentView *)[context latentView])
        {
            [context wine_updateBackingSize:&r.size];
            macdrv_update_opengl_context(context);
        }
        else
        {
            [context removeFromViews:NO];
            macdrv_add_view_opengl_context(view, context);

            if (context.needsUpdate)
            {
                context.needsUpdate = FALSE;
                context.needsReattach = FALSE;
                if (context.view)
                    [context setView:[[context class] dummyView]];
                [context wine_updateBackingSize:&r.size];
                [context setView:(NSView *)view];
                [context setLatentView:nil];
                [context resetSurfaceIfBackingSizeChanged];
            }
            else
            {
                if ([context view])
                    [context clearDrawableLeavingSurfaceOnScreen];
                [context wine_updateBackingSize:&r.size];
                [context setLatentView:(NSView *)view];
            }
        }

        [context makeCurrentContext];
    }
    else
    {
        WineOpenGLContext* currentContext = (WineOpenGLContext*)[WineOpenGLContext currentContext];

        if ([currentContext isKindOfClass:[WineOpenGLContext class]])
        {
            [WineOpenGLContext clearCurrentContext];
            if (currentContext != context)
                [currentContext removeFromViews:YES];
        }

        if (context)
            [context removeFromViews:YES];
    }
}
}

/***********************************************************************
 *              macdrv_update_opengl_context
 */
void macdrv_update_opengl_context(WineOpenGLContext *context)
{
@autoreleasepool
{
    if (context.needsUpdate)
    {
        BOOL reattach = context.needsReattach;
        context.needsUpdate = FALSE;
        context.needsReattach = FALSE;
        if (context.latentView)
        {
            [context setView:context.latentView];
            context.latentView = nil;

            [context resetSurfaceIfBackingSizeChanged];
        }
        else
        {
            if (reattach)
            {
                NSView* view = [[context.view retain] autorelease];
                [context clearDrawableLeavingSurfaceOnScreen];
                context.view = view;
            }
            else OnMainThread(^{
                [context update];
            });
            [context resetSurfaceIfBackingSizeChanged];
        }
    }
}
}

/***********************************************************************
 *              macdrv_flush_opengl_context
 *
 * Performs an implicit glFlush() and then swaps the back buffer to the
 * front (if the context is double-buffered).
 */
void macdrv_flush_opengl_context(WineOpenGLContext *context)
{
@autoreleasepool
{
    macdrv_update_opengl_context(context);
    [context flushBuffer];
}
}
