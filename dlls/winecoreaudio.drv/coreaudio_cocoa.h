/*
 * CoreAudio Cocoa interface declarations
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

#ifndef __WINE_COREAUDIO_COCOA_H
#define __WINE_COREAUDIO_COCOA_H

/* The values of AVAuthorizationStatus. */
enum capture_authorization
{
    CAPTURE_AUTHORIZATION_NOT_DETERMINED = 0,
    CAPTURE_AUTHORIZATION_RESTRICTED     = 1,
    CAPTURE_AUTHORIZATION_DENIED         = 2,
    CAPTURE_AUTHORIZATION_AUTHORIZED     = 3,
};

extern int CoreAudio_get_capture_authorization_status(void);
extern int CoreAudio_request_capture_authorization(void);

#endif /* __WINE_COREAUDIO_COCOA_H */
