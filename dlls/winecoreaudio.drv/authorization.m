/*
 * Microphone access for CoreAudio capture
 *
 * Copyright 2020 Brendan Shanks for CodeWeavers
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

#if 0
#pragma makedep unix
#endif

#include "config.h"

#import <AVFoundation/AVFoundation.h>

#include "coreaudio_cocoa.h"

/* Posted in this process around the system prompt; winemac.drv observes
 * them so that the prompt taking the focus is not taken for the user
 * switching away from the application. */
static NSString * const WineWillShowPermissionDialogNotification = @"WineWillShowPermissionDialogNotification";
static NSString * const WineDidShowPermissionDialogNotification = @"WineDidShowPermissionDialogNotification";

/***********************************************************************
 *              CoreAudio_get_capture_authorization_status
 *
 * Return the AVAuthorizationStatus of the microphone for this process.
 */
int CoreAudio_get_capture_authorization_status(void)
{
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    int status = [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio];

    [pool release];
    return status;
}

/***********************************************************************
 *              CoreAudio_request_capture_authorization
 *
 * Ask the user for access to the microphone and wait for the answer.
 * Return 1 if access was granted, 0 if not.
 */
int CoreAudio_request_capture_authorization(void)
{
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    NSNotificationCenter *center = [NSNotificationCenter defaultCenter];
    dispatch_semaphore_t answered = dispatch_semaphore_create(0);
    __block BOOL granted = NO;

    [center postNotificationName:WineWillShowPermissionDialogNotification object:nil];
    [AVCaptureDevice requestAccessForMediaType:AVMediaTypeAudio completionHandler:^(BOOL ok) {
        granted = ok;
        dispatch_semaphore_signal(answered);
    }];
    dispatch_semaphore_wait(answered, DISPATCH_TIME_FOREVER);
    dispatch_release(answered);
    [center postNotificationName:WineDidShowPermissionDialogNotification object:nil];

    [pool release];
    return granted;
}
