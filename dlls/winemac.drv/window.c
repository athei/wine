/*
 * MACDRV windowing driver
 *
 * Copyright 1993, 1994, 1995, 1996, 2001 Alexandre Julliard
 * Copyright 1993 David Metcalfe
 * Copyright 1995, 1996 Alex Korobka
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

#if 0
#pragma makedep unix
#endif

#include "config.h"
#include "macdrv.h"

#include <IOKit/pwr_mgt/IOPMLib.h>

WINE_DEFAULT_DEBUG_CHANNEL(macdrv);


static pthread_mutex_t win_data_mutex;
static CFMutableDictionaryRef win_datas;

static unsigned int activate_on_focus_time;


/* per-monitor DPI aware NtUserSetWindowPos call */
static BOOL set_window_pos(HWND hwnd, HWND after, INT x, INT y, INT cx, INT cy, UINT flags)
{
    UINT context = NtUserSetThreadDpiAwarenessContext(NTUSER_DPI_PER_MONITOR_AWARE_V2);
    BOOL ret = NtUserSetWindowPos(hwnd, after, x, y, cx, cy, flags);
    NtUserSetThreadDpiAwarenessContext(context);
    return ret;
}


static struct macdrv_window_features get_window_features_for_style(DWORD style, DWORD ex_style, BOOL shaped)
{
    struct macdrv_window_features wf = {0};

    if (ex_style & WS_EX_NOACTIVATE) wf.prevents_app_activation = TRUE;

    if ((style & WS_CAPTION) == WS_CAPTION && !(ex_style & WS_EX_LAYERED))
    {
        wf.shadow = TRUE;
        if (!shaped)
        {
            wf.title_bar = TRUE;
            if (style & WS_SYSMENU) wf.close_button = TRUE;
            if (style & WS_MINIMIZEBOX) wf.minimize_button = TRUE;
            if (style & WS_MAXIMIZEBOX) wf.maximize_button = TRUE;
            if (ex_style & WS_EX_TOOLWINDOW) wf.utility = TRUE;
        }
    }
    if (style & WS_THICKFRAME)
    {
        wf.shadow = TRUE;
        if (!shaped) wf.resizable = TRUE;
    }
    else if (ex_style & WS_EX_DLGMODALFRAME) wf.shadow = TRUE;
    else if ((style & (WS_DLGFRAME|WS_BORDER)) == WS_DLGFRAME) wf.shadow = TRUE;

    return wf;
}

/***********************************************************************
 *              get_cocoa_window_features
 */
static struct macdrv_window_features get_cocoa_window_features(struct macdrv_win_data *data, DWORD style, DWORD ex_style)
{
    struct macdrv_window_features wf = {0};

    if (ex_style & WS_EX_NOACTIVATE) wf.prevents_app_activation = TRUE;
    if (EqualRect(&data->rects.window, &data->rects.visible)) return wf;

    return get_window_features_for_style(style, ex_style, data->shaped);
}


/*******************************************************************
 *              can_window_become_foreground
 *
 * Check if the specified window can become the foreground/key
 * window.
 */
static inline BOOL can_window_become_foreground(HWND hwnd)
{
    LONG style = NtUserGetWindowLongW(hwnd, GWL_STYLE);

    if (!(style & WS_VISIBLE)) return FALSE;
    if ((style & (WS_POPUP|WS_CHILD)) == WS_CHILD) return FALSE;
    if (hwnd == NtUserGetDesktopWindow()) return FALSE;
    return !(style & WS_DISABLED);
}


/***********************************************************************
 *              get_cocoa_window_state
 */
static void get_cocoa_window_state(struct macdrv_win_data *data,
                                   DWORD style, DWORD ex_style,
                                   struct macdrv_window_state* state)
{
    memset(state, 0, sizeof(*state));
    state->disabled = (style & WS_DISABLED) != 0;
    state->no_foreground = !can_window_become_foreground(data->hwnd);
    state->floating = (ex_style & WS_EX_TOPMOST) != 0;
    state->excluded_by_expose = state->excluded_by_cycle =
        (!(ex_style & WS_EX_APPWINDOW) &&
         (NtUserGetWindowRelative(data->hwnd, GW_OWNER) || (ex_style & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE))));
    if (IsRectEmpty(&data->rects.window))
        state->excluded_by_expose = TRUE;
    state->minimized = (style & WS_MINIMIZE) != 0;
    state->minimized_valid = state->minimized != data->minimized;
    state->maximized = (style & WS_MAXIMIZE) != 0;
}


/***********************************************************************
 *              constrain_window_frame
 *
 * Alter a window frame rectangle to fit within a) Cocoa's documented
 * limits, and b) sane sizes, like twice the desktop rect.
 */
static void constrain_window_frame(CGPoint* origin, CGSize* size)
{
    CGRect desktop_rect = macdrv_get_desktop_rect();
    int max_width, max_height;

    max_width = min(32000, 2 * CGRectGetWidth(desktop_rect));
    max_height = min(32000, 2 * CGRectGetHeight(desktop_rect));

    if (origin)
    {
        if (origin->x < -16000) origin->x = -16000;
        if (origin->y < -16000) origin->y = -16000;
        if (origin->x > 16000) origin->x = 16000;
        if (origin->y > 16000) origin->y = 16000;
    }
    if (size)
    {
        if (size->width > max_width) size->width = max_width;
        if (size->height > max_height) size->height = max_height;
    }
}


/***********************************************************************
 *              alloc_win_data
 */
static struct macdrv_win_data *alloc_win_data(HWND hwnd)
{
    struct macdrv_win_data *data;

    if ((data = calloc(1, sizeof(*data))))
    {
        data->hwnd = hwnd;
        pthread_mutex_lock(&win_data_mutex);
        if (!win_datas)
            win_datas = CFDictionaryCreateMutable(NULL, 0, NULL, NULL);
        CFDictionarySetValue(win_datas, hwnd, data);
    }
    return data;
}


/***********************************************************************
 *              get_win_data
 *
 * Lock and return the data structure associated with a window.
 */
struct macdrv_win_data *get_win_data(HWND hwnd)
{
    struct macdrv_win_data *data;

    if (!hwnd) return NULL;
    pthread_mutex_lock(&win_data_mutex);
    if (win_datas && (data = (struct macdrv_win_data*)CFDictionaryGetValue(win_datas, hwnd)))
        return data;
    pthread_mutex_unlock(&win_data_mutex);
    return NULL;
}


/***********************************************************************
 *              release_win_data
 *
 * Release the data returned by get_win_data.
 */
void release_win_data(struct macdrv_win_data *data)
{
    if (data) pthread_mutex_unlock(&win_data_mutex);
}


/***********************************************************************
 *              macdrv_get_cocoa_window
 *
 * Return the Mac window associated with the full area of a window
 */
WineWindow *macdrv_get_cocoa_window(HWND hwnd, bool require_on_screen)
{
    struct macdrv_win_data *data = get_win_data(hwnd);
    WineWindow *ret = NULL;
    if (data && (data->on_screen || !require_on_screen))
        ret = data->cocoa_window;
    release_win_data(data);
    return ret;
}


/***********************************************************************
 *              set_cocoa_window_properties
 *
 * Set the window properties for a Cocoa window based on its Windows
 * properties.
 */
static void set_cocoa_window_properties(struct macdrv_win_data *data)
{
    DWORD style, ex_style;
    HWND owner;
    WineWindow *owner_win;
    struct macdrv_window_features wf;
    struct macdrv_window_state state;

    style = NtUserGetWindowLongW(data->hwnd, GWL_STYLE);
    ex_style = NtUserGetWindowLongW(data->hwnd, GWL_EXSTYLE);

    owner = NtUserGetWindowRelative(data->hwnd, GW_OWNER);
    if (owner)
        owner = NtUserGetAncestor(owner, GA_ROOT);
    owner_win = macdrv_get_cocoa_window(owner, TRUE);
    macdrv_set_cocoa_parent_window(data->cocoa_window, owner_win);

    wf = get_cocoa_window_features(data, style, ex_style);
    macdrv_set_cocoa_window_features(data->cocoa_window, &wf);

    get_cocoa_window_state(data, style, ex_style, &state);
    macdrv_set_cocoa_window_state(data->cocoa_window, &state);
    if (state.minimized_valid)
        data->minimized = state.minimized;
}


/***********************************************************************
 *              sync_window_region
 *
 * Update the window region.
 */
static void sync_window_region(struct macdrv_win_data *data, HRGN win_region)
{
    if (!data->cocoa_window) return;

    if (IsRectEmpty(&data->rects.window))  /* set an empty shape */
    {
        data->shaped = FALSE;
        TRACE("win %p/%p setting empty shape for zero-sized window\n", data->hwnd, data->cocoa_window);
        macdrv_set_window_shape(data->cocoa_window, &CGRectZero, 1);
        return;
    }

    /* use surface shape instead */
    macdrv_set_window_shape(data->cocoa_window, NULL, 0);
}


/***********************************************************************
 *              set_shape_pending
 *
 * A window region reaches the Cocoa window as a shape image with the next
 * flush, which can come after the window is shown.  Tell the Cocoa window
 * about the region before that, so that it counts as shaped meanwhile.
 */
void set_shape_pending(struct macdrv_win_data *data, bool pending)
{
    if (!data->cocoa_window || !data->shape_pending == !pending) return;
    macdrv_window_set_shape_pending(data->cocoa_window, pending);
    data->shape_pending = pending;
}


/***********************************************************************
 *              sync_window_opacity
 */
static void sync_window_opacity(struct macdrv_win_data *data, BYTE alpha,
                                BOOL per_pixel_alpha, DWORD flags)
{
    CGFloat opacity = 1.0;

    if (flags & LWA_ALPHA) opacity = alpha / 255.0;

    TRACE("setting window %p/%p alpha to %g\n", data->hwnd, data->cocoa_window, opacity);
    macdrv_set_window_alpha(data->cocoa_window, opacity);

    if (!data->per_pixel_alpha != !per_pixel_alpha)
    {
        TRACE("setting window %p/%p per-pixel-alpha to %d\n", data->hwnd, data->cocoa_window, per_pixel_alpha);
        macdrv_window_use_per_pixel_alpha(data->cocoa_window, per_pixel_alpha);
        data->per_pixel_alpha = per_pixel_alpha;
    }
}


/***********************************************************************
 *              sync_window_min_max_info
 */
static void sync_window_min_max_info(HWND hwnd)
{
    LONG style = NtUserGetWindowLongW(hwnd, GWL_STYLE);
    LONG exstyle = NtUserGetWindowLongW(hwnd, GWL_EXSTYLE);
    UINT dpi = NtUserGetWinMonitorDpi(hwnd, MDT_RAW_DPI);
    RECT win_rect, primary_monitor_rect;
    MINMAXINFO minmax;
    LONG adjustedStyle;
    INT xinc, yinc;
    WINDOWPLACEMENT wpl;
    HMONITOR monitor;
    struct macdrv_win_data *data;
    BOOL menu;

    TRACE("win %p\n", hwnd);

    if (!macdrv_get_cocoa_window(hwnd, FALSE)) return;

    NtUserGetWindowRect(hwnd, &win_rect, dpi);
    minmax.ptReserved.x = win_rect.left;
    minmax.ptReserved.y = win_rect.top;

    if ((style & WS_CAPTION) == WS_CAPTION)
        adjustedStyle = style & ~WS_BORDER; /* WS_CAPTION = WS_DLGFRAME | WS_BORDER */
    else
        adjustedStyle = style;

    primary_monitor_rect.left = primary_monitor_rect.top = 0;
    primary_monitor_rect.right = NtUserGetSystemMetrics(SM_CXSCREEN);
    primary_monitor_rect.bottom = NtUserGetSystemMetrics(SM_CYSCREEN);
    menu = ((style & WS_POPUP) && NtUserGetWindowLongPtrW(hwnd, GWLP_ID));
    NtUserAdjustWindowRect(&primary_monitor_rect, adjustedStyle, menu, exstyle, dpi);

    xinc = -primary_monitor_rect.left;
    yinc = -primary_monitor_rect.top;

    minmax.ptMaxSize.x = primary_monitor_rect.right - primary_monitor_rect.left;
    minmax.ptMaxSize.y = primary_monitor_rect.bottom - primary_monitor_rect.top;
    minmax.ptMaxPosition.x = -xinc;
    minmax.ptMaxPosition.y = -yinc;
    if (style & (WS_DLGFRAME | WS_BORDER))
    {
        minmax.ptMinTrackSize.x = NtUserGetSystemMetrics(SM_CXMINTRACK);
        minmax.ptMinTrackSize.y = NtUserGetSystemMetrics(SM_CYMINTRACK);
    }
    else
    {
        minmax.ptMinTrackSize.x = 2 * xinc;
        minmax.ptMinTrackSize.y = 2 * yinc;
    }
    minmax.ptMaxTrackSize.x = NtUserGetSystemMetrics(SM_CXMAXTRACK);
    minmax.ptMaxTrackSize.y = NtUserGetSystemMetrics(SM_CYMAXTRACK);

    wpl.length = sizeof(wpl);
    if (NtUserGetWindowPlacement(hwnd, &wpl) && (wpl.ptMaxPosition.x != -1 || wpl.ptMaxPosition.y != -1))
    {
        minmax.ptMaxPosition = wpl.ptMaxPosition;

        /* Convert from GetWindowPlacement's workspace coordinates to screen coordinates. */
        minmax.ptMaxPosition.x -= wpl.rcNormalPosition.left - win_rect.left;
        minmax.ptMaxPosition.y -= wpl.rcNormalPosition.top - win_rect.top;
    }

    TRACE("initial ptMaxSize %s ptMaxPosition %s ptMinTrackSize %s ptMaxTrackSize %s\n", wine_dbgstr_point(&minmax.ptMaxSize),
          wine_dbgstr_point(&minmax.ptMaxPosition), wine_dbgstr_point(&minmax.ptMinTrackSize), wine_dbgstr_point(&minmax.ptMaxTrackSize));

    send_message(hwnd, WM_GETMINMAXINFO, 0, (LPARAM)&minmax);

    TRACE("app's ptMaxSize %s ptMaxPosition %s ptMinTrackSize %s ptMaxTrackSize %s\n", wine_dbgstr_point(&minmax.ptMaxSize),
          wine_dbgstr_point(&minmax.ptMaxPosition), wine_dbgstr_point(&minmax.ptMinTrackSize), wine_dbgstr_point(&minmax.ptMaxTrackSize));

    /* if the app didn't change the values, adapt them for the window's monitor */
    if ((monitor = NtUserMonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY)))
    {
        MONITORINFO mon_info;
        RECT monitor_rect;

        mon_info.cbSize = sizeof(mon_info);
        NtUserGetMonitorInfo(monitor, &mon_info);

        if ((style & WS_MAXIMIZEBOX) && ((style & WS_CAPTION) == WS_CAPTION || !(style & WS_POPUP)))
            monitor_rect = mon_info.rcWork;
        else
            monitor_rect = mon_info.rcMonitor;

        if (minmax.ptMaxSize.x == primary_monitor_rect.right - primary_monitor_rect.left &&
            minmax.ptMaxSize.y == primary_monitor_rect.bottom - primary_monitor_rect.top)
        {
            minmax.ptMaxSize.x = (monitor_rect.right - monitor_rect.left) + 2 * xinc;
            minmax.ptMaxSize.y = (monitor_rect.bottom - monitor_rect.top) + 2 * yinc;
        }
        if (minmax.ptMaxPosition.x == -xinc && minmax.ptMaxPosition.y == -yinc)
        {
            minmax.ptMaxPosition.x = monitor_rect.left - xinc;
            minmax.ptMaxPosition.y = monitor_rect.top - yinc;
        }
    }

    minmax.ptMaxTrackSize.x = max(minmax.ptMaxTrackSize.x, minmax.ptMinTrackSize.x);
    minmax.ptMaxTrackSize.y = max(minmax.ptMaxTrackSize.y, minmax.ptMinTrackSize.y);

    TRACE("adjusted ptMaxSize %s ptMaxPosition %s ptMinTrackSize %s ptMaxTrackSize %s\n", wine_dbgstr_point(&minmax.ptMaxSize),
          wine_dbgstr_point(&minmax.ptMaxPosition), wine_dbgstr_point(&minmax.ptMinTrackSize), wine_dbgstr_point(&minmax.ptMaxTrackSize));

    if ((data = get_win_data(hwnd)) && data->cocoa_window)
    {
        RECT min_rect, max_rect;
        CGSize min_size, max_size;

        SetRect(&min_rect, 0, 0, minmax.ptMinTrackSize.x, minmax.ptMinTrackSize.y);
        min_rect = visible_rect_from_window(&data->rects, min_rect);
        min_size = CGSizeMake(min_rect.right - min_rect.left, min_rect.bottom - min_rect.top);

        if (minmax.ptMaxTrackSize.x == NtUserGetSystemMetrics(SM_CXMAXTRACK) &&
            minmax.ptMaxTrackSize.y == NtUserGetSystemMetrics(SM_CYMAXTRACK))
            max_size = CGSizeMake(CGFLOAT_MAX, CGFLOAT_MAX);
        else
        {
            SetRect(&max_rect, 0, 0, minmax.ptMaxTrackSize.x, minmax.ptMaxTrackSize.y);
            max_rect = visible_rect_from_window(&data->rects, max_rect);
            max_size = CGSizeMake(max_rect.right - max_rect.left, max_rect.bottom - max_rect.top);
        }

        constrain_window_frame(NULL, &max_size);

        TRACE("min_size (%g,%g) max_size (%g,%g)\n", min_size.width, min_size.height, max_size.width, max_size.height);
        macdrv_set_window_min_max_sizes(data->cocoa_window, min_size, max_size);
    }

    release_win_data(data);
}


/**********************************************************************
 *              create_cocoa_window
 *
 * Create the whole Mac window for a given window
 */
static void create_cocoa_window(struct macdrv_win_data *data)
{
    struct macdrv_thread_data *thread_data = macdrv_init_thread_data();
    WCHAR text[1024];
    struct macdrv_window_features wf;
    CGRect frame;
    DWORD style, ex_style;
    HRGN win_rgn;
    COLORREF key;
    BYTE alpha;
    DWORD layered_flags;

    if ((win_rgn = NtGdiCreateRectRgn(0, 0, 0, 0)) &&
        NtUserGetWindowRgnEx(data->hwnd, win_rgn, 0) == ERROR)
    {
        NtGdiDeleteObjectApp(win_rgn);
        win_rgn = 0;
    }
    data->shaped = (win_rgn != 0);

    style = NtUserGetWindowLongW(data->hwnd, GWL_STYLE);
    ex_style = NtUserGetWindowLongW(data->hwnd, GWL_EXSTYLE);

    wf = get_cocoa_window_features(data, style, ex_style);

    frame = cgrect_from_rect(data->rects.visible);
    constrain_window_frame(&frame.origin, &frame.size);
    if (frame.size.width < 1 || frame.size.height < 1)
        frame.size.width = frame.size.height = 1;

    TRACE("creating %p window %s whole %s client %s\n", data->hwnd, wine_dbgstr_rect(&data->rects.window),
          wine_dbgstr_rect(&data->rects.visible), wine_dbgstr_rect(&data->rects.client));

    data->cocoa_window = macdrv_create_cocoa_window(&wf, frame, data->hwnd, thread_data->queue);
    if (!data->cocoa_window) goto done;
    if (data->shaped) set_shape_pending(data, TRUE);

    set_cocoa_window_properties(data);

    /* set the window text */
    if (!NtUserInternalGetWindowText(data->hwnd, text, ARRAY_SIZE(text))) text[0] = 0;
    macdrv_set_cocoa_window_title(data->cocoa_window, text, wcslen(text));

    /* set the window region */
    if (win_rgn || IsRectEmpty(&data->rects.window)) sync_window_region(data, win_rgn);

    /* set the window opacity */
    if (!NtUserGetLayeredWindowAttributes(data->hwnd, &key, &alpha, &layered_flags)) layered_flags = 0;
    sync_window_opacity(data, alpha, FALSE, layered_flags);

done:
    if (win_rgn) NtGdiDeleteObjectApp(win_rgn);
}


/**********************************************************************
 *              destroy_cocoa_window
 *
 * Destroy the whole Mac window for a given window.
 */
static void destroy_cocoa_window(struct macdrv_win_data *data)
{
    if (!data->cocoa_window) return;

    TRACE("win %p Cocoa win %p\n", data->hwnd, data->cocoa_window);

    macdrv_destroy_cocoa_window(data->cocoa_window);
    data->cocoa_window = 0;
    data->on_screen = FALSE;
    data->shape_pending = FALSE;
}


/***********************************************************************
 *              macdrv_create_win_data
 *
 * Create a Mac data window structure for an existing window.
 */
static struct macdrv_win_data *macdrv_create_win_data(HWND hwnd, const struct window_rects *rects)
{
    struct macdrv_win_data *data;
    HWND parent;

    if (NtUserGetWindowThread(hwnd, NULL) != GetCurrentThreadId()) return NULL;

    if (!(parent = NtUserGetAncestor(hwnd, GA_PARENT)))  /* desktop */
    {
        macdrv_init_thread_data();
        return NULL;
    }

    if (!(data = alloc_win_data(hwnd))) return NULL;
    data->rects = *rects;

    if (parent == NtUserGetDesktopWindow())
    {
        create_cocoa_window(data);
        TRACE("win %p/%p window %s whole %s client %s\n",
               hwnd, data->cocoa_window, wine_dbgstr_rect(&data->rects.window),
               wine_dbgstr_rect(&data->rects.visible), wine_dbgstr_rect(&data->rects.client));
    }

    TRACE("win %p/%p window %s whole %s client %s\n",
           hwnd, data->cocoa_window, wine_dbgstr_rect(&data->rects.window),
           wine_dbgstr_rect(&data->rects.visible), wine_dbgstr_rect(&data->rects.client));
    return data;
}


/**********************************************************************
 *              is_owned_by
 */
static BOOL is_owned_by(HWND hwnd, HWND maybe_owner)
{
    while (1)
    {
        HWND hwnd2 = NtUserGetWindowRelative(hwnd, GW_OWNER);
        if (!hwnd2)
            hwnd2 = NtUserGetAncestor(hwnd, GA_ROOT);
        if (!hwnd2 || hwnd2 == hwnd)
            break;
        if (hwnd2 == maybe_owner)
            return TRUE;
        hwnd = hwnd2;
    }

    return FALSE;
}


/**********************************************************************
 *              is_all_the_way_front
 */
static BOOL is_all_the_way_front(HWND hwnd)
{
    BOOL topmost = (NtUserGetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
    HWND prev = hwnd;

    while ((prev = NtUserGetWindowRelative(prev, GW_HWNDPREV)))
    {
        if (!topmost && (NtUserGetWindowLongW(prev, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0)
            return TRUE;
        if (!is_owned_by(prev, hwnd))
            return FALSE;
    }

    return TRUE;
}


/***********************************************************************
 *              set_focus
 */
static void set_focus(HWND hwnd, BOOL raise)
{
    struct macdrv_win_data *data;

    if (!(hwnd = NtUserGetAncestor(hwnd, GA_ROOT))) return;

    if (raise && hwnd == NtUserGetForegroundWindow() && hwnd != NtUserGetDesktopWindow() && !is_all_the_way_front(hwnd))
        NtUserSetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);

    if (!(data = get_win_data(hwnd))) return;

    if (data->cocoa_window && data->on_screen)
    {
        BOOL activate = activate_on_focus_time && (NtGetTickCount() - activate_on_focus_time < 2000);
        /* Set Mac focus */
        macdrv_give_cocoa_window_focus(data->cocoa_window, activate);
        activate_on_focus_time = 0;
    }

    release_win_data(data);
}

/***********************************************************************
 *              show_window
 */
static void show_window(struct macdrv_win_data *data)
{
    HWND prev = NULL;
    HWND next = NULL;
    WineWindow *prev_window = NULL;
    WineWindow *next_window = NULL;
    BOOL activate = FALSE;
    GUITHREADINFO info;

    /* find window that this one must be after */
    prev = NtUserGetWindowRelative(data->hwnd, GW_HWNDPREV);
    while (prev && !((NtUserGetWindowLongW(prev, GWL_STYLE) & (WS_VISIBLE | WS_MINIMIZE)) == WS_VISIBLE &&
                     (prev_window = macdrv_get_cocoa_window(prev, TRUE))))
        prev = NtUserGetWindowRelative(prev, GW_HWNDPREV);
    if (!prev_window)
    {
        /* find window that this one must be before */
        next = NtUserGetWindowRelative(data->hwnd, GW_HWNDNEXT);
        while (next && !((NtUserGetWindowLongW(next, GWL_STYLE) & (WS_VISIBLE | WS_MINIMIZE)) == WS_VISIBLE &&
                         (next_window = macdrv_get_cocoa_window(next, TRUE))))
            next = NtUserGetWindowRelative(next, GW_HWNDNEXT);
    }

    TRACE("win %p/%p below %p/%p above %p/%p\n",
          data->hwnd, data->cocoa_window, prev, prev_window, next, next_window);

    if (!prev_window)
        activate = activate_on_focus_time && (NtGetTickCount() - activate_on_focus_time < 2000);
    macdrv_order_cocoa_window(data->cocoa_window, prev_window, next_window, activate);
    data->on_screen = TRUE;

    info.cbSize = sizeof(info);
    if (NtUserGetGUIThreadInfo(NtUserGetWindowThread(data->hwnd, NULL), &info) && info.hwndFocus &&
        (data->hwnd == info.hwndFocus || NtUserIsChild(data->hwnd, info.hwndFocus)))
        set_focus(info.hwndFocus, FALSE);
    if (activate)
        activate_on_focus_time = 0;
}


/***********************************************************************
 *              hide_window
 */
static void hide_window(struct macdrv_win_data *data)
{
    TRACE("win %p/%p\n", data->hwnd, data->cocoa_window);

    if (data->cocoa_window)
        macdrv_hide_cocoa_window(data->cocoa_window);
    data->on_screen = FALSE;
}


/***********************************************************************
 *              get_region_data
 *
 * Calls GetRegionData on the given region and converts the rectangle
 * array to CGRect format. The returned buffer must be freed by caller.
 * If hdc_lptodp is not 0, the rectangles are converted through LPtoDP.
 */
RGNDATA *get_region_data(HRGN hrgn, HDC hdc_lptodp)
{
    RGNDATA *data;
    DWORD size;
    int i;
    RECT *rect;
    CGRect *cgrect;

    if (!hrgn || !(size = NtGdiGetRegionData(hrgn, 0, NULL))) return NULL;
    if (sizeof(CGRect) > sizeof(RECT))
    {
        /* add extra size for CGRect array */
        int count = (size - sizeof(RGNDATAHEADER)) / sizeof(RECT);
        size += count * (sizeof(CGRect) - sizeof(RECT));
    }
    if (!(data = malloc(size))) return NULL;
    if (!NtGdiGetRegionData(hrgn, size, data))
    {
        free(data);
        return NULL;
    }

    rect = (RECT *)data->Buffer;
    cgrect = (CGRect *)data->Buffer;
    if (hdc_lptodp)  /* map to device coordinates */
    {
        NtGdiTransformPoints(hdc_lptodp, (POINT *)rect, (POINT *)rect,
                             data->rdh.nCount * 2, NtGdiLPtoDP);
        for (i = 0; i < data->rdh.nCount; i++)
        {
            if (rect[i].right < rect[i].left)
            {
                INT tmp = rect[i].right;
                rect[i].right = rect[i].left;
                rect[i].left = tmp;
            }
            if (rect[i].bottom < rect[i].top)
            {
                INT tmp = rect[i].bottom;
                rect[i].bottom = rect[i].top;
                rect[i].top = tmp;
            }
        }
    }

    if (sizeof(CGRect) > sizeof(RECT))
    {
        /* need to start from the end */
        for (i = data->rdh.nCount-1; i >= 0; i--)
            cgrect[i] = cgrect_from_rect(rect[i]);
    }
    else
    {
        for (i = 0; i < data->rdh.nCount; i++)
            cgrect[i] = cgrect_from_rect(rect[i]);
    }
    return data;
}


/***********************************************************************
 *              sync_window_position
 *
 * Synchronize the Mac window position with the Windows one
 */
static void sync_window_position(struct macdrv_win_data *data, UINT swp_flags, const struct window_rects *old_rects)
{
    CGRect frame = cgrect_from_rect(data->rects.visible);

    if (data->cocoa_window)
    {
        if (data->minimized) return;

        constrain_window_frame(&frame.origin, &frame.size);
        if (frame.size.width < 1 || frame.size.height < 1)
            frame.size.width = frame.size.height = 1;

        macdrv_set_cocoa_window_frame(data->cocoa_window, &frame);
    }

    if (old_rects &&
        (IsRectEmpty(&old_rects->window) != IsRectEmpty(&data->rects.window) ||
         old_rects->window.left - old_rects->visible.left != data->rects.window.left - data->rects.visible.left ||
         old_rects->window.top - old_rects->visible.top != data->rects.window.top - data->rects.visible.top))
        sync_window_region(data, (HRGN)1);

    TRACE("win %p/%p whole_rect %s frame %s\n", data->hwnd, data->cocoa_window,
          wine_dbgstr_rect(&data->rects.visible), wine_dbgstr_cgrect(frame));

    if (data->cocoa_window && data->on_screen && (!(swp_flags & SWP_NOZORDER) || (swp_flags & SWP_SHOWWINDOW)))
        show_window(data);
}


/**********************************************************************
 *              activate_on_following_focus
 */
void activate_on_following_focus(void)
{
    activate_on_focus_time = NtGetTickCount();
    if (!activate_on_focus_time) activate_on_focus_time = 1;
}


/***********************************************************************
 *              set_app_icon
 */
static void set_app_icon(void)
{
    CFArrayRef images = create_app_icon_images();
    if (images)
    {
        macdrv_set_application_icon(images);
        CFRelease(images);
    }
}


/**********************************************************************
 *		        set_capture_window_for_move
 */
static BOOL set_capture_window_for_move(HWND hwnd)
{
    HWND previous = 0;
    BOOL ret;

    SERVER_START_REQ(set_capture_window)
    {
        req->handle = wine_server_user_handle(hwnd);
        req->flags  = CAPTURE_MOVESIZE;
        if ((ret = !wine_server_call_err(req)))
        {
            previous = wine_server_ptr_handle(reply->previous);
            hwnd = wine_server_ptr_handle(reply->full_handle);
        }
    }
    SERVER_END_REQ;

    if (ret)
    {
        macdrv_SetCapture(NtUserGetAncestor(hwnd, GA_ROOT), GUI_INMOVESIZE, NtUserGetAncestor(previous, GA_ROOT));

        if (previous && previous != hwnd)
            send_message(previous, WM_CAPTURECHANGED, 0, (LPARAM)hwnd);
    }
    return ret;
}


static HMONITOR monitor_from_point(POINT pt, UINT flags)
{
    RECT rect;

    SetRect(&rect, pt.x, pt.y, pt.x + 1, pt.y + 1);
    return NtUserMonitorFromRect(&rect, flags);
}


/***********************************************************************
 *              move_window
 *
 * Based on user32's WINPOS_SysCommandSizeMove() specialized just for
 * moving top-level windows and enforcing Mac-style constraints like
 * keeping the top of the window within the work area.
 */
static LRESULT move_window(HWND hwnd, WPARAM wparam)
{
    UINT dpi = NtUserGetWinMonitorDpi(hwnd, MDT_DEFAULT);
    MSG msg;
    RECT origRect, movedRect, desktopRect;
    int hittest = (int)(wparam & 0x0f);
    POINT capturePoint;
    LONG style = NtUserGetWindowLongW(hwnd, GWL_STYLE);
    BOOL moved = FALSE;
    DWORD dwPoint = NtUserGetMessagePos();
    INT captionHeight;
    HMONITOR mon = 0;
    MONITORINFO info;

    if ((style & (WS_MINIMIZE | WS_MAXIMIZE)) || !IsWindowVisible(hwnd)) return -1;
    if (hittest && hittest != HTCAPTION) return -1;

    capturePoint.x = (short)LOWORD(dwPoint);
    capturePoint.y = (short)HIWORD(dwPoint);
    NtUserClipCursor(NULL);

    TRACE("hwnd %p hittest %d, pos %d,%d\n", hwnd, hittest, capturePoint.x, capturePoint.y);

    origRect.left = origRect.right = origRect.top = origRect.bottom = 0;
    if (NtUserAdjustWindowRect(&origRect, style, FALSE, NtUserGetWindowLongW(hwnd, GWL_EXSTYLE), dpi))
        captionHeight = -origRect.top;
    else
        captionHeight = 0;

    NtUserGetWindowRect(hwnd, &origRect, NtUserGetWinMonitorDpi(hwnd, MDT_DEFAULT));
    movedRect = origRect;

    if (!hittest)
    {
        /* Move pointer to the center of the caption */
        RECT rect = origRect;

        /* Note: to be exactly centered we should take the different types
         * of border into account, but it shouldn't make more than a few pixels
         * of difference so let's not bother with that */
        rect.top += NtUserGetSystemMetrics(SM_CYBORDER);
        if (style & WS_SYSMENU)
            rect.left += NtUserGetSystemMetrics(SM_CXSIZE) + 1;
        if (style & WS_MINIMIZEBOX)
            rect.right -= NtUserGetSystemMetrics(SM_CXSIZE) + 1;
        if (style & WS_MAXIMIZEBOX)
            rect.right -= NtUserGetSystemMetrics(SM_CXSIZE) + 1;
        capturePoint.x = (rect.right + rect.left) / 2;
        capturePoint.y = rect.top + NtUserGetSystemMetrics(SM_CYSIZE)/2;

        NtUserSetCursorPos(capturePoint.x, capturePoint.y);
        send_message(hwnd, WM_SETCURSOR, (WPARAM)hwnd, MAKELONG(HTCAPTION, WM_MOUSEMOVE));
    }

    desktopRect = rect_from_cgrect(macdrv_get_desktop_rect());
    mon = monitor_from_point(capturePoint, MONITOR_DEFAULTTONEAREST);
    info.cbSize = sizeof(info);
    if (mon && !NtUserGetMonitorInfo(mon, &info))
        mon = 0;

    /* repaint the window before moving it around */
    NtUserRedrawWindow(hwnd, NULL, 0, RDW_UPDATENOW | RDW_ALLCHILDREN);

    send_message(hwnd, WM_ENTERSIZEMOVE, 0, 0);
    set_capture_window_for_move(hwnd);

    while(1)
    {
        POINT pt;
        int dx = 0, dy = 0;
        HMONITOR newmon;

        if (!NtUserGetMessage(&msg, 0, 0, 0)) break;
        if (NtUserCallMsgFilter(&msg, MSGF_SIZE)) continue;

        /* Exit on button-up, Return, or Esc */
        if (msg.message == WM_LBUTTONUP ||
            (msg.message == WM_KEYDOWN && (msg.wParam == VK_RETURN || msg.wParam == VK_ESCAPE)))
            break;

        if (msg.message != WM_KEYDOWN && msg.message != WM_MOUSEMOVE)
        {
            NtUserTranslateMessage(&msg, 0);
            NtUserDispatchMessage(&msg);
            continue;  /* We are not interested in other messages */
        }

        pt = msg.pt;

        if (msg.message == WM_KEYDOWN) switch(msg.wParam)
        {
        case VK_UP:    pt.y -= 8; break;
        case VK_DOWN:  pt.y += 8; break;
        case VK_LEFT:  pt.x -= 8; break;
        case VK_RIGHT: pt.x += 8; break;
        }

        pt.x = max(pt.x, desktopRect.left);
        pt.x = min(pt.x, desktopRect.right - 1);
        pt.y = max(pt.y, desktopRect.top);
        pt.y = min(pt.y, desktopRect.bottom - 1);

        if ((newmon = monitor_from_point(pt, MONITOR_DEFAULTTONULL)) && newmon != mon)
        {
            if (NtUserGetMonitorInfo(newmon, &info))
                mon = newmon;
            else
                mon = 0;
        }

        if (mon)
        {
            /* wineserver clips the cursor position to the virtual desktop rect but,
               if the display configuration is non-rectangular, that could still
               leave the logical cursor position outside of any display.  The window
               could keep moving as you push the cursor against a display edge, even
               though the visible cursor doesn't keep moving. The following keeps
               the window movement in sync with the visible cursor. */
            pt.x = max(pt.x, info.rcMonitor.left);
            pt.x = min(pt.x, info.rcMonitor.right - 1);
            pt.y = max(pt.y, info.rcMonitor.top);
            pt.y = min(pt.y, info.rcMonitor.bottom - 1);

            /* Assuming that dx will be calculated below as pt.x - capturePoint.x,
               dy will be pt.y - capturePoint.y, and movedRect will be offset by those,
               we want to enforce these constraints:
                    movedRect.left + dx < info.rcWork.right
                    movedRect.right + dx > info.rcWork.left
                    movedRect.top + captionHeight + dy < info.rcWork.bottom
                    movedRect.bottom + dy > info.rcWork.top
                    movedRect.top + dy >= info.rcWork.top
               The first four keep at least one edge barely in the work area.
               The last keeps the top (i.e. the title bar) in the work area.
               The fourth is redundant with the last, so can be ignored.

               Substituting for dx and dy and rearranging gives us...
             */
            pt.x = min(pt.x, info.rcWork.right - 1 + capturePoint.x - movedRect.left);
            pt.x = max(pt.x, info.rcWork.left + 1 + capturePoint.x - movedRect.right);
            pt.y = min(pt.y, info.rcWork.bottom - 1 + capturePoint.y - movedRect.top - captionHeight);
            pt.y = max(pt.y, info.rcWork.top + capturePoint.y - movedRect.top);
        }

        dx = pt.x - capturePoint.x;
        dy = pt.y - capturePoint.y;

        if (dx || dy)
        {
            moved = TRUE;

            if (msg.message == WM_KEYDOWN) NtUserSetCursorPos(pt.x, pt.y);
            else
            {
                OffsetRect(&movedRect, dx, dy);
                capturePoint = pt;

                send_message(hwnd, WM_MOVING, 0, (LPARAM)&movedRect);
                set_window_pos(hwnd, 0, movedRect.left, movedRect.top, 0, 0, SWP_NOACTIVATE | SWP_NOSIZE | SWP_NOZORDER);
            }
        }
    }

    set_capture_window_for_move(0);

    send_message(hwnd, WM_EXITSIZEMOVE, 0, 0);
    send_message(hwnd, WM_SETVISIBLE, TRUE, 0L);

    /* if the move is canceled, restore the previous position */
    if (moved && msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE)
    {
        set_window_pos(hwnd, 0, origRect.left, origRect.top, 0, 0, SWP_NOACTIVATE | SWP_NOSIZE | SWP_NOZORDER);
    }

    return 0;
}


/***********************************************************************
 *              perform_window_command
 */
static void perform_window_command(HWND hwnd, unsigned int style_any, unsigned int style_none, WORD command, WORD hittest)
{
    unsigned int style;

    TRACE("win %p style_any 0x%08x style_none 0x%08x command 0x%04x hittest 0x%04x\n",
          hwnd, style_any, style_none, command, hittest);

    style = NtUserGetWindowLongW(hwnd, GWL_STYLE);
    if ((style_any && !(style & style_any)) || (style & (WS_DISABLED | style_none)))
    {
        TRACE("not changing win %p style 0x%08x\n", hwnd, style);
        return;
    }

    if (get_active_window() != hwnd)
    {
        LRESULT ma = send_message(hwnd, WM_MOUSEACTIVATE, (WPARAM)NtUserGetAncestor(hwnd, GA_ROOT),
                                  MAKELPARAM(hittest, WM_NCLBUTTONDOWN));
        switch (ma)
        {
            case MA_NOACTIVATEANDEAT:
            case MA_ACTIVATEANDEAT:
                TRACE("not changing win %p mouse-activate result %ld\n", hwnd, ma);
                return;
            case MA_NOACTIVATE:
                break;
            case MA_ACTIVATE:
            case 0:
                NtUserSetActiveWindow(hwnd);
                break;
            default:
                WARN("unknown WM_MOUSEACTIVATE code %ld\n", ma);
                break;
        }
    }

    TRACE("changing win %p\n", hwnd);
    NtUserPostMessage(hwnd, WM_SYSCOMMAND, command, 0);
}

static void macdrv_client_surface_destroy(struct client_surface *client)
{
    struct macdrv_client_surface *surface = impl_from_client_surface(client);

    TRACE("%s\n", debugstr_client_surface(client));

    if (surface->metal_swapchain) macdrv_destroy_swapchain(surface->metal_swapchain);
}

static void macdrv_client_surface_detach(struct client_surface *client)
{
    struct macdrv_client_surface *surface = impl_from_client_surface(client);

    TRACE("%s\n", debugstr_client_surface(client));

    if (surface->cocoa_view)
    {
        struct macdrv_win_data *data;

        if ((data = get_win_data(client->hwnd)))
        {
            if (data->client_view == surface->cocoa_view)
                data->client_view = NULL;
            release_win_data(data);
        }

        /* the surface is going away; WineMetalLayer and mtld3d read it from the view */
        macdrv_set_view_d3dmetal_client_surface(surface->cocoa_view, NULL);
        macdrv_dispose_view(surface->cocoa_view);
    }
}

struct client_view_list
{
    HWND         hwnd;
    unsigned int count;
    struct
    {
        HWND             hwnd;
        WineContentView *view;
    } *entries;
};

static void collect_client_view(const void *key, const void *value, void *context)
{
    const struct macdrv_win_data *data = value;
    struct client_view_list *list = context;

    /* skip toplevels, the only windows with a Cocoa window */
    if (!data->client_view || data->cocoa_window || data->hwnd == list->hwnd) return;
    list->entries[list->count].hwnd = data->hwnd;
    list->entries[list->count].view = data->client_view;
    list->count++;
}

/* Fill chain with hwnd and its ancestors, ending with its toplevel. */
static unsigned int get_window_ancestors(HWND hwnd, HWND *chain, unsigned int size)
{
    HWND desktop = NtUserGetDesktopWindow();
    unsigned int count = 0;

    while (hwnd && hwnd != desktop)
    {
        if (count == size) return 0;
        chain[count++] = hwnd;
        hwnd = NtUserGetAncestor(hwnd, GA_PARENT);
    }
    return count;
}

/* Whether a window and all its ancestors up to its toplevel are visible. */
static BOOL is_chain_visible(const HWND *chain, unsigned int count)
{
    while (count--)
        if (!(NtUserGetWindowLongW(chain[count], GWL_STYLE) & WS_VISIBLE)) return FALSE;
    return TRUE;
}

/* Whether hwnd is above its sibling, from one list of hwnd and the siblings below it. */
static BOOL is_sibling_above(HWND hwnd, HWND sibling)
{
    HWND buffer[64], *list = buffer;
    ULONG i, count = ARRAY_SIZE(buffer);
    NTSTATUS status;
    BOOL ret = FALSE;

    while ((status = NtUserBuildHwndList(0, hwnd, FALSE, FALSE, 0, count, list, &count)) == STATUS_BUFFER_TOO_SMALL)
    {
        if (list != buffer) free(list);
        if (!(list = malloc(count * sizeof(*list)))) return FALSE;
    }
    for (i = 0; !status && list[i] != HWND_BOTTOM; i++)
    {
        if (list[i] != sibling) continue;
        ret = TRUE;
        break;
    }
    if (list != buffer) free(list);
    return ret;
}

/* Whether a window is above another of the same toplevel, given the ancestor
 * chains of both: a child is above its ancestors, and windows with different
 * parents are ordered as their ancestors are where the chains diverge. */
static BOOL is_window_above(const HWND *chain, unsigned int count, const HWND *other, unsigned int other_count)
{
    while (count > 1 && other_count > 1 && chain[count - 2] == other[other_count - 2])
    {
        count--;
        other_count--;
    }
    if (count == 1) return FALSE;
    if (other_count == 1) return TRUE;
    return is_sibling_above(chain[count - 2], other[other_count - 2]);
}

/***********************************************************************
 *              find_client_view_above
 *
 * Find the view that the client view of a child window goes directly
 * below: the current client view of the lowest visible window of the same
 * toplevel that is above the window. The window data lock must not be held.
 */
static WineContentView *find_client_view_above(HWND hwnd, HWND toplevel, HWND *above_hwnd)
{
    struct client_view_list list = {.hwnd = hwnd};
    HWND chain[32], other[32], best[32];
    unsigned int i, count, other_count, best_count = 0;
    WineContentView *ret = NULL;

    pthread_mutex_lock(&win_data_mutex);
    if (win_datas && (list.entries = malloc(CFDictionaryGetCount(win_datas) * sizeof(*list.entries))))
        CFDictionaryApplyFunction(win_datas, collect_client_view, &list);
    pthread_mutex_unlock(&win_data_mutex);

    if (list.count && (count = get_window_ancestors(hwnd, chain, ARRAY_SIZE(chain))) &&
        chain[count - 1] == toplevel)
    {
        for (i = 0; i < list.count; i++)
        {
            if (!(other_count = get_window_ancestors(list.entries[i].hwnd, other, ARRAY_SIZE(other)))) continue;
            if (other[other_count - 1] != toplevel) continue;
            if (!is_chain_visible(other, other_count)) continue;
            if (is_window_above(chain, count, other, other_count)) continue;
            if (ret && is_window_above(other, other_count, best, best_count)) continue;

            memcpy(best, other, other_count * sizeof(*other));
            best_count = other_count;
            *above_hwnd = list.entries[i].hwnd;
            ret = list.entries[i].view;
        }
    }

    free(list.entries);
    return ret;
}

/* client surfaces created through macdrv_functions, guarded by the window data lock */
static unsigned int d3dmetal_surface_count;

/* one bit each in client_overlay.pending */
#define MAX_CLIENT_OVERLAY_RECTS 32

static struct macdrv_client_surface *get_d3dmetal_client_view_surface(const struct macdrv_win_data *data);

struct overlay_window
{
    HWND         chain[32];         /* the window and its ancestors */
    unsigned int count;
    DWORD        style;
    RECT         rect;              /* window rect in the toplevel's content view, clipped by the ancestors */
    POINT        origin;            /* top left of the window rect there, not clipped */
    RECT         view_rect;         /* frame of the client view, if the window has one */
    HRGN         region;            /* window region there, if the window has one */
    bool         client_view;
    bool         presenting;        /* is_d3dmetal_client_view_presenting */
};

struct overlay_window_list
{
    unsigned int           count;
    struct overlay_window *windows;
};

static void collect_overlay_window(const void *key, const void *value, void *context)
{
    struct macdrv_win_data *data = (struct macdrv_win_data *)value;
    struct overlay_window_list *list = context;
    struct overlay_window *window = &list->windows[list->count++];

    window->chain[0] = data->hwnd;
    window->client_view = !!data->client_view;
    if ((window->presenting = is_d3dmetal_client_view_presenting(data)))
        window->view_rect = get_d3dmetal_client_view_surface(data)->view_rect;
}

/* Get the window rect and the client rect of a window in its toplevel's
 * content view, the space of the client view frames, clipped by the client
 * rects of its ancestors, and the top left of its window rect there. The
 * caller holds the window data lock. */
static bool get_content_view_rects(const HWND *chain, unsigned int count, RECT *window, RECT *client,
                                   POINT *origin)
{
    const struct macdrv_win_data *data;
    unsigned int i;

    if (!(data = CFDictionaryGetValue(win_datas, chain[0]))) return false;
    *window = data->rects.window;
    *client = data->rects.client;
    origin->x = window->left;
    origin->y = window->top;
    for (i = 1; i < count; i++)
    {
        if (!(data = CFDictionaryGetValue(win_datas, chain[i]))) return false;
        OffsetRect(window, data->rects.client.left, data->rects.client.top);
        OffsetRect(client, data->rects.client.left, data->rects.client.top);
        origin->x += data->rects.client.left;
        origin->y += data->rects.client.top;
        intersect_rect(window, window, &data->rects.client);
        intersect_rect(client, client, &data->rects.client);
    }
    OffsetRect(window, -data->rects.visible.left, -data->rects.visible.top);
    OffsetRect(client, -data->rects.visible.left, -data->rects.visible.top);
    origin->x -= data->rects.visible.left;
    origin->y -= data->rects.visible.top;
    return true;
}

/* Get the window region of a window, if it has one, in its toplevel's
 * content view. The window data lock must not be held. */
static HRGN get_content_view_region(const struct overlay_window *window)
{
    HWND hwnd = window->chain[0];
    HRGN hrgn = NtGdiCreateRectRgn(0, 0, 0, 0), ret = 0;
    float scale = (float)NtUserGetWinMonitorDpi(hwnd, MDT_RAW_DPI) / NtUserGetDpiForWindow(hwnd);
    XFORM xform = {scale, 0, 0, scale, window->origin.x, window->origin.y};
    RGNDATA *data;
    DWORD size;

    /* the region is relative to the window rect, at the window's DPI */
    if (NtUserGetWindowRgnEx(hwnd, hrgn, 0) != ERROR && (size = NtGdiGetRegionData(hrgn, 0, NULL)) &&
        (data = malloc(size)))
    {
        if (NtGdiGetRegionData(hrgn, size, data)) ret = NtGdiExtCreateRegion(&xform, size, data);
        free(data);
    }
    NtGdiDeleteObjectApp(hrgn);
    return ret;
}

static struct overlay_window *find_overlay_window(struct overlay_window *windows, unsigned int count, HWND hwnd)
{
    unsigned int i;

    for (i = 0; i < count; i++)
        if (windows[i].count && windows[i].chain[0] == hwnd) return &windows[i];
    return NULL;
}

/* Pass the rects of the client overlay that no flush waits for to the Cocoa
 * window. The caller holds the window data. */
static void show_client_overlay(struct macdrv_win_data *data)
{
    struct client_overlay *overlay = &data->client_overlay;
    CGRect shown[MAX_CLIENT_OVERLAY_RECTS];
    unsigned int i, count = 0;

    for (i = 0; i < overlay->count; i++)
        if (!(overlay->pending & (1u << i))) shown[count++] = overlay->rects[i];
    macdrv_window_set_client_overlay(data->cocoa_window, shown, count);
}

/* Set the rects of the client overlay of a toplevel. A rect that lies
 * inside one that shows already shows at once. Any other waits for a flush
 * to paint it if it meets the area of a window that may not have painted
 * its new place yet, or a rect that still waits, so a child window that
 * was just shown does not show unpainted. A child that does not paint
 * after it is shown or moved keeps its rects waiting until a flush paints
 * into them. The caller holds the window data. */
static void set_client_overlay(struct macdrv_win_data *data, const CGRect *rects, unsigned int count)
{
    struct client_overlay *overlay = &data->client_overlay;
    CGRect *copy = NULL;
    unsigned int i, j, pending = 0;
    bool shown;

    if (count == overlay->count && (!count || !memcmp(rects, overlay->rects, count * sizeof(*rects))))
        return;
    if (count && !(copy = malloc(count * sizeof(*copy)))) return;

    for (i = 0; i < count; i++)
    {
        copy[i] = rects[i];
        for (j = 0, shown = false; j < overlay->count && !shown; j++)
            shown = !(overlay->pending & (1u << j)) && CGRectContainsRect(overlay->rects[j], rects[i]);
        if (shown) continue;
        if (CGRectIntersectsRect(rects[i], overlay->unpainted)) pending |= 1u << i;
        for (j = 0; j < overlay->count; j++)
            if ((overlay->pending & (1u << j)) && CGRectIntersectsRect(overlay->rects[j], rects[i]))
                pending |= 1u << i;
    }

    TRACE("%p: %u rects, pending %#x\n", data->hwnd, count, pending);
    for (i = 0; i < count; i++) TRACE("    %s\n", wine_dbgstr_cgrect(rects[i]));
    free(overlay->rects);
    overlay->rects = copy;
    overlay->count = count;
    overlay->pending = pending;
    show_client_overlay(data);
}

/***********************************************************************
 *              update_client_overlay
 *
 * Child windows without a client view of their own draw into the window
 * surface of their toplevel, which shows below every client view. On
 * Windows, the presents of a window with WS_CLIPCHILDREN leave out its
 * children, and those of a window with WS_CLIPSIBLINGS leave out its
 * siblings above it, and so do those of its descendants, as the server
 * clips visible regions: not for windows with WS_EX_TRANSPARENT, and only
 * within window regions. Find those parts of the client views of DXMT and
 * mtld3d, less any client view above them, for the window surface to show
 * there above the client views. Do it for the toplevel of hwnd, which may
 * not have painted its new place yet if unpainted is set. The window data
 * lock must not be held.
 */
static void update_client_overlay(HWND hwnd, bool unpainted)
{
    struct overlay_window_list list = {0};
    struct overlay_window *window, *view, *other, *ancestor;
    unsigned int i, j, k, surfaces, count, serial, rect_count = 0;
    struct macdrv_win_data *data;
    HRGN region, part, rgn;
    RGNDATA *region_data;
    const CGRect *rects = NULL;
    bool presenting = false;
    HWND toplevel, chain[32];
    unsigned int chain_count = 0;

    pthread_mutex_lock(&win_data_mutex);
    surfaces = d3dmetal_surface_count;
    pthread_mutex_unlock(&win_data_mutex);
    if (!surfaces || !(toplevel = NtUserGetAncestor(hwnd, GA_ROOT))) return;

    if (unpainted && (chain_count = get_window_ancestors(hwnd, chain, ARRAY_SIZE(chain))) &&
        (chain[chain_count - 1] != toplevel || !is_chain_visible(chain, chain_count)))
        chain_count = 0;

    if (!(data = get_win_data(toplevel))) return;
    /* updates can finish out of order: only the one that started last sets the rects */
    serial = ++data->client_overlay.serial;
    if (data->cocoa_window && (list.windows = malloc(CFDictionaryGetCount(win_datas) * sizeof(*list.windows))))
        CFDictionaryApplyFunction(win_datas, collect_overlay_window, &list);
    for (i = 0; i < list.count; i++) presenting |= list.windows[i].presenting;
    if (!presenting && !data->client_overlay.count)
    {
        /* no rects to set, so none to hold back for the earlier updates */
        data->client_overlay.unpainted = CGRectZero;
        list.count = 0;
    }
    /* Keep the place of the window for the update that sets the rects, which
     * may be another one, from the same moment as the serial. */
    if (list.count && chain_count)
    {
        RECT rect, client;
        POINT origin;

        if (get_content_view_rects(chain, chain_count, &rect, &client, &origin) && !IsRectEmpty(&rect))
        {
            CGRect moved = cgrect_from_rect(rect), *area = &data->client_overlay.unpainted;
            *area = CGRectIsEmpty(*area) ? moved : CGRectUnion(*area, moved);
        }
    }
    release_win_data(data);
    if (!list.count)
    {
        free(list.windows);
        return;
    }

    /* keep the visible windows of the toplevel */
    for (i = count = 0; i < list.count; i++)
    {
        window = &list.windows[count];
        *window = list.windows[i];
        if (!(window->count = get_window_ancestors(window->chain[0], window->chain, ARRAY_SIZE(window->chain))))
            continue;
        if (window->chain[window->count - 1] != toplevel || !is_chain_visible(window->chain, window->count))
            continue;
        window->style = NtUserGetWindowLongW(window->chain[0], GWL_STYLE);
        count++;
    }

    /* the client view of DXMT or mtld3d has the frame of its surface */
    pthread_mutex_lock(&win_data_mutex);
    for (i = 0; i < count; i++)
    {
        RECT client;

        window = &list.windows[i];
        if (!get_content_view_rects(window->chain, window->count, &window->rect, &client, &window->origin))
        {
            window->count = 0;
            continue;
        }
        if (!window->presenting) window->view_rect = client;
    }
    pthread_mutex_unlock(&win_data_mutex);

    for (i = 0; i < count; i++)
    {
        window = &list.windows[i];
        window->region = window->count > 1 && !window->client_view ? get_content_view_region(window) : 0;
    }

    region = NtGdiCreateRectRgn(0, 0, 0, 0);
    part = NtGdiCreateRectRgn(0, 0, 0, 0);
    rgn = NtGdiCreateRectRgn(0, 0, 0, 0);
    for (i = 0; i < count; i++)
    {
        bool found = false;

        window = &list.windows[i];
        if (window->count < 2 || window->client_view) continue;

        NtGdiSetRectRgn(part, 0, 0, 0, 0);
        for (j = 0; j < count; j++)
        {
            unsigned int depth = window->count, view_depth;
            RECT rect;

            view = &list.windows[j];
            if (!view->count || !view->presenting) continue;

            /* Find the window, or its ancestor, that is a child of the view's
             * window, or a sibling of it or of one of its ancestors. */
            view_depth = view->count;
            while (depth > 1 && view_depth > 1 && window->chain[depth - 2] == view->chain[view_depth - 2])
            {
                depth--;
                view_depth--;
            }
            if (depth == 1) continue; /* the window holds the view */
            if (view_depth == 1)
            {
                if (!(view->style & WS_CLIPCHILDREN)) continue;
            }
            else if (!(NtUserGetWindowLongW(view->chain[view_depth - 2], GWL_STYLE) & WS_CLIPSIBLINGS) ||
                     !is_sibling_above(window->chain[depth - 2], view->chain[view_depth - 2]))
                continue;
            if (NtUserGetWindowLongW(window->chain[depth - 2], GWL_EXSTYLE) & WS_EX_TRANSPARENT) continue;

            if (!intersect_rect(&rect, &window->rect, &view->view_rect)) continue;
            NtGdiSetRectRgn(rgn, rect.left, rect.top, rect.right, rect.bottom);
            /* the window regions of the window and of its ancestors up to there clip it */
            for (k = 0; k <= depth - 2; k++)
                if ((ancestor = find_overlay_window(list.windows, count, window->chain[k])) && ancestor->region)
                    NtGdiCombineRgn(rgn, rgn, ancestor->region, RGN_AND);
            NtGdiCombineRgn(part, part, rgn, RGN_OR);
            found = true;
        }
        if (!found) continue;

        for (j = 0; j < count; j++)
        {
            other = &list.windows[j];
            if (!other->count || !other->client_view || IsRectEmpty(&other->view_rect)) continue;
            if (!is_window_above(other->chain, other->count, window->chain, window->count)) continue;
            NtGdiSetRectRgn(rgn, other->view_rect.left, other->view_rect.top,
                            other->view_rect.right, other->view_rect.bottom);
            NtGdiCombineRgn(part, part, rgn, RGN_DIFF);
        }
        NtGdiCombineRgn(region, region, part, RGN_OR);
    }
    for (i = 0; i < count; i++)
        if (list.windows[i].region) NtGdiDeleteObjectApp(list.windows[i].region);
    free(list.windows);

    if ((region_data = get_region_data(region, 0)))
    {
        rects = (const CGRect *)region_data->Buffer;
        rect_count = region_data->rdh.nCount;
        if (rect_count > MAX_CLIENT_OVERLAY_RECTS)
        {
            static int once;
            if (!once++) WARN("client overlay of %p has %u rects, showing %u\n", toplevel, rect_count,
                              MAX_CLIENT_OVERLAY_RECTS);
            rect_count = MAX_CLIENT_OVERLAY_RECTS;
        }
    }
    NtGdiDeleteObjectApp(rgn);
    NtGdiDeleteObjectApp(part);
    NtGdiDeleteObjectApp(region);

    if ((data = get_win_data(toplevel)))
    {
        if (data->client_overlay.serial != serial)
            TRACE("%p: a later update sets the rects\n", toplevel);
        else if (data->cocoa_window)
        {
            set_client_overlay(data, rects, rect_count);
            data->client_overlay.unpainted = CGRectZero;
        }
        release_win_data(data);
    }
    free(region_data);
}

/***********************************************************************
 *              flush_client_overlay
 *
 * Show the rects of the client overlay that waited for a flush to paint
 * them once one has painted into them. A flush comes for each frame while a
 * child window above a client view of DXMT or mtld3d repaints, as for
 * an edit control that the game redraws, so this does nothing else. The
 * caller holds the window data.
 */
void flush_client_overlay(struct macdrv_win_data *data, const RECT *dirty)
{
    struct client_overlay *overlay = &data->client_overlay;
    unsigned int i, pending = overlay->pending;
    CGRect rect;

    if (!pending) return;

    rect = cgrect_from_rect(*dirty);
    for (i = 0; i < overlay->count; i++)
        if (CGRectIntersectsRect(rect, overlay->rects[i])) pending &= ~(1u << i);
    if (pending == overlay->pending) return;

    TRACE("%p: painted %s, pending %#x\n", data->hwnd, wine_dbgstr_rect(dirty), pending);
    overlay->pending = pending;
    show_client_overlay(data);
}

struct overlay_toplevels
{
    unsigned int count;
    HWND         hwnds[16];
};

static void collect_overlay_toplevel(const void *key, const void *value, void *context)
{
    const struct macdrv_win_data *data = value;
    struct overlay_toplevels *toplevels = context;

    if (data->client_overlay.count && toplevels->count < ARRAY_SIZE(toplevels->hwnds))
        toplevels->hwnds[toplevels->count++] = data->hwnd;
}

/* Update every client overlay, for a child window that went away without
 * being hidden first, whose toplevel is no longer known. The window data
 * lock must not be held. */
static void update_client_overlays(void)
{
    struct overlay_toplevels toplevels = {0};
    unsigned int i;

    pthread_mutex_lock(&win_data_mutex);
    if (d3dmetal_surface_count && win_datas)
        CFDictionaryApplyFunction(win_datas, collect_overlay_toplevel, &toplevels);
    pthread_mutex_unlock(&win_data_mutex);

    for (i = 0; i < toplevels.count; i++) update_client_overlay(toplevels.hwnds[i], false);
}

static void macdrv_client_surface_update(struct client_surface *client)
{
    struct macdrv_client_surface *surface = impl_from_client_surface(client);
    HWND hwnd = client->hwnd, toplevel = client->toplevel, above_hwnd = NULL;
    WineContentView *above_view = NULL;
    struct macdrv_win_data *data;
    bool moved;

    TRACE("%s\n", debugstr_client_surface(client));

    /* The client views of a toplevel's child windows share a superview, so
     * order them as their windows are ordered. Raising the view on every
     * update instead swaps the views of child windows that present in turn. */
    if (toplevel != hwnd) above_view = find_client_view_above(hwnd, toplevel, &above_hwnd);

    if (!(data = get_win_data(toplevel))) return;

    /* The view may have been replaced or detached since it was found. Keep
     * holding the window data until the view is passed on, so it stays alive. */
    if (above_view)
    {
        struct macdrv_win_data *above_data = get_win_data(above_hwnd);

        if (!above_data || above_data->client_view != above_view)
        {
            TRACE("client view %p of %p is gone\n", above_view, above_hwnd);
            above_view = NULL;
        }
        release_win_data(above_data);
    }

    /* DXMT updates its surface for every frame, so only a view that moves
     * moves the client overlay. */
    moved = surface->presented && !EqualRect(&surface->view_rect, &client->monitor_rect);
    surface->view_rect = client->monitor_rect;

    macdrv_set_view_frame(surface->cocoa_view, cgrect_from_rect(client->monitor_rect));
    macdrv_set_view_superview(surface->cocoa_view, toplevel == hwnd ? NULL : data->client_view, data->cocoa_window,
                              above_view, NULL);
    release_win_data(data);

    if (moved) update_client_overlay(toplevel, false);
}

/* Make the surface's cocoa view the one the window shows. Returns whether it
 * was not already. The caller holds the window data. */
static bool show_client_view(struct macdrv_client_surface *surface, struct macdrv_win_data *data)
{
    if (data->client_view == surface->cocoa_view) return false;
    if (data->client_view) macdrv_set_view_hidden(data->client_view, TRUE);
    macdrv_set_view_hidden(surface->cocoa_view, FALSE);
    data->client_view = surface->cocoa_view;
    return true;
}

static void macdrv_client_surface_present(struct client_surface *client, HDC hdc)
{
    struct macdrv_client_surface *surface = impl_from_client_surface(client);
    struct macdrv_win_data *data;
    bool shown, presented, update;

    TRACE("%s\n", debugstr_client_surface(client));

    if (!(data = get_win_data(surface->client.hwnd))) return;
    presented = surface->presented;
    /* mtld3d presents its surface itself: once when a device attaches, which
     * finds the view that creating the surface showed, and after that only
     * once that surface's device has drawn a frame, to show the view again or
     * to size it. So every present but the first comes with a frame, and the
     * first one counts only if it had to show the view. DXMT marks its frames
     * through CLIENT_SURFACE_PRESENTED instead. The window data lock guards
     * present_count and presented. */
    shown = show_client_view(surface, data);
    if (surface->present_count++ || shown) surface->presented = true;
    /* a view that starts to show frames goes into the client overlay */
    update = surface->presented && (shown || !presented);
    release_win_data(data);

    if (update) update_client_overlay(surface->client.hwnd, false);
}

static const struct client_surface_funcs macdrv_client_surface_funcs =
{
    .size = sizeof(struct macdrv_client_surface),
    .destroy = macdrv_client_surface_destroy,
    .detach = macdrv_client_surface_detach,
    .update = macdrv_client_surface_update,
    .present = macdrv_client_surface_present,
};

struct macdrv_client_surface *impl_from_client_surface(struct client_surface *client)
{
    assert(client->funcs == &macdrv_client_surface_funcs);
    return CONTAINING_RECORD(client, struct macdrv_client_surface, client);
}

struct client_surface *macdrv_CreateClientSurface(HWND hwnd, int pixel_format, BOOL raw)
{
    struct macdrv_client_surface *surface;

    surface = client_surface_create(&macdrv_client_surface_funcs, hwnd, pixel_format, raw);

    if (surface)
    {
        struct macdrv_win_data *data;

        surface->cocoa_view = macdrv_create_view(cgrect_from_rect(surface->client.monitor_rect));
        macdrv_set_view_hidden(surface->cocoa_view, TRUE);
        macdrv_client_surface_update(&surface->client);
        if ((data = get_win_data(hwnd)))
        {
            show_client_view(surface, data);
            release_win_data(data);
        }
    }

    return surface ? &surface->client : NULL;
}

bool macdrv_client_surface_acquire_metal_swapchain(struct macdrv_client_surface *surface)
{
    HWND hwnd = surface->client.hwnd;
    struct macdrv_win_data *data;

    if (surface->metal_swapchain) return true;

    if ((data = get_win_data(hwnd)))
    {
        release_win_data(data);
        surface->metal_swapchain = macdrv_create_view_swapchain(surface->cocoa_view);
    }
    else
    {
        RECT rect;

        if (NtUserGetAncestor(hwnd, GA_ROOT) != hwnd)
        {
            FIXME("Cross-process child window Metal swapchains are not implemented\n");
            return false;
        }

        if (!NtUserGetClientRect(hwnd, &rect, NtUserGetWinMonitorDpi(hwnd, MDT_RAW_DPI))) return false;
        surface->metal_swapchain = macdrv_create_offscreen_swapchain(hwnd, cgrect_from_rect(rect));
    }

    return surface->metal_swapchain != NULL;
}

/**********************************************************************
 *              SetDesktopWindow   (MACDRV.@)
 */
void macdrv_SetDesktopWindow(HWND hwnd)
{
    static pthread_once_t app_icon_once = PTHREAD_ONCE_INIT;
    unsigned int width, height;

    TRACE("%p\n", hwnd);

    /* retrieve the real size of the desktop */
    SERVER_START_REQ(get_window_rectangles)
    {
        req->handle = wine_server_user_handle(hwnd);
        req->relative = COORDS_CLIENT;
        wine_server_call(req);
        width  = reply->window.right;
        height = reply->window.bottom;
    }
    SERVER_END_REQ;

    if (!width && !height)  /* not initialized yet */
    {
        CGRect rect = macdrv_get_desktop_rect();

        SERVER_START_REQ(set_window_pos)
        {
            req->handle        = wine_server_user_handle(hwnd);
            req->previous      = 0;
            req->swp_flags     = SWP_NOZORDER;
            req->window.left   = CGRectGetMinX(rect);
            req->window.top    = CGRectGetMinY(rect);
            req->window.right  = CGRectGetMaxX(rect);
            req->window.bottom = CGRectGetMaxY(rect);
            req->client        = req->window;
            wine_server_call(req);
        }
        SERVER_END_REQ;
    }

    pthread_once(&app_icon_once, set_app_icon);
}

#define WM_WINE_NOTIFY_ACTIVITY WM_USER

LRESULT macdrv_DesktopWindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_WINE_NOTIFY_ACTIVITY:
    {
        /* This wakes from display sleep, but doesn't affect the screen saver. */
        static IOPMAssertionID assertion;
        IOPMAssertionDeclareUserActivity(CFSTR("Wine user input"), kIOPMUserActiveLocal, &assertion);

        /* This prevents the screen saver, but doesn't wake from display sleep. */
        /* It's deprecated, but there's no better alternative. */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        UpdateSystemActivity(UsrActivity);
#pragma clang diagnostic pop
        break;
    }
    }
    return NtUserMessageCall(hwnd, msg, wp, lp, 0, NtUserDefWindowProc, FALSE);
}

/***********************************************************************
 *              DestroyWindow   (MACDRV.@)
 */
void macdrv_DestroyWindow(HWND hwnd)
{
    struct macdrv_win_data *data;
    CFMutableArrayRef d3dmetal_client_surfaces;
    bool child;

    TRACE("%p\n", hwnd);

    if (!(data = get_win_data(hwnd))) return;

    if (data->drag_event) NtSetEvent(data->drag_event, NULL);

    child = !data->cocoa_window;
    destroy_cocoa_window(data);

    d3dmetal_client_surfaces = data->d3dmetal_client_surfaces;
    data->d3dmetal_client_surfaces = NULL;
    if (d3dmetal_client_surfaces) d3dmetal_surface_count -= CFArrayGetCount(d3dmetal_client_surfaces);

    CFDictionaryRemoveValue(win_datas, hwnd);
    release_win_data(data);
    free(data->client_overlay.rects);
    free(data);

    /* Releasing a client surface takes win32u's surface lock, and the other
     * holders of that lock (detach_client_surfaces, update_client_surfaces)
     * call back into this driver for the window data, so it must not happen
     * while the window data is held: two threads, one destroying its window
     * and one creating, moving or destroying its own, would deadlock. */
    if (d3dmetal_client_surfaces) CFRelease(d3dmetal_client_surfaces);

    /* win32u hides a window before it destroys it, but not the windows of
     * another thread that it destroys when that thread exits */
    if (child) update_client_overlays();
}


/*****************************************************************
 *              ActivateWindow   (MACDRV.@)
 *
 * Set the Mac active window.
 */
void macdrv_ActivateWindow(HWND hwnd, HWND previous)
{
    struct macdrv_thread_data *thread_data = macdrv_thread_data();

    TRACE("%p\n", hwnd);

    if (!thread_data) return;
    thread_data->dead_key_state = 0;
    set_focus(hwnd, TRUE);
}


/***********************************************************************
 *              SetLayeredWindowAttributes  (MACDRV.@)
 *
 * Set transparency attributes for a layered window.
 */
void macdrv_SetLayeredWindowAttributes(HWND hwnd, COLORREF key, BYTE alpha, DWORD flags)
{
    struct macdrv_win_data *data = get_win_data(hwnd);

    TRACE("hwnd %p key %#08x alpha %#02x flags %x\n", hwnd, key, alpha, flags);

    if (data)
    {
        data->layered = TRUE;
        data->ulw_layered = FALSE;
        if (data->cocoa_window)
        {
            sync_window_opacity(data, alpha, FALSE, flags);
            /* since layered attributes are now set, can now show the window */
            if ((NtUserGetWindowLongW(hwnd, GWL_STYLE) & WS_VISIBLE) && !data->on_screen)
                show_window(data);
        }
        release_win_data(data);
    }
    else
        FIXME("setting layered attributes on window %p of other process not supported\n", hwnd);
}


/*****************************************************************
 *              SetParent   (MACDRV.@)
 */
void macdrv_SetParent(HWND hwnd, HWND parent, HWND old_parent)
{
    struct macdrv_win_data *data;

    TRACE("%p, %p, %p\n", hwnd, parent, old_parent);

    if (parent == old_parent) return;
    if (!(data = get_win_data(hwnd))) return;

    if (parent != NtUserGetDesktopWindow()) /* a child window */
    {
        if (old_parent == NtUserGetDesktopWindow())
        {
            /* destroy the old Mac window */
            destroy_cocoa_window(data);
        }
    }
    else  /* new top level window */
    {
        create_cocoa_window(data);
    }
    release_win_data(data);
}


/***********************************************************************
 *              SetWindowRgn  (MACDRV.@)
 *
 * Assign specified region to window (for non-rectangular windows)
 */
void macdrv_SetWindowRgn(HWND hwnd, HRGN hrgn, BOOL redraw)
{
    struct macdrv_win_data *data;

    TRACE("%p, %p, %d\n", hwnd, hrgn, redraw);

    if ((data = get_win_data(hwnd)))
    {
        sync_window_region(data, hrgn);
        release_win_data(data);
    }
    else
    {
        DWORD procid;

        NtUserGetWindowThread(hwnd, &procid);
        if (procid != GetCurrentProcessId())
            send_message(hwnd, WM_MACDRV_SET_WIN_REGION, 0, 0);
    }
}


/***********************************************************************
 *              SetWindowStyle   (MACDRV.@)
 *
 * Update the state of the Cocoa window to reflect a style change
 */
void macdrv_SetWindowStyle(HWND hwnd, INT offset, STYLESTRUCT *style)
{
    struct macdrv_win_data *data;

    TRACE("hwnd %p offset %d styleOld 0x%08x styleNew 0x%08x\n", hwnd, offset, style->styleOld, style->styleNew);

    if (hwnd == NtUserGetDesktopWindow()) return;
    if (!(data = get_win_data(hwnd))) return;

    if (data->cocoa_window)
    {
        DWORD changed = style->styleNew ^ style->styleOld;

        set_cocoa_window_properties(data);

        if (offset == GWL_EXSTYLE && (changed & WS_EX_LAYERED)) /* changing WS_EX_LAYERED resets attributes */
        {
            data->layered = FALSE;
            data->ulw_layered = FALSE;
            sync_window_opacity(data, 0, FALSE, 0);
        }

        if (offset == GWL_EXSTYLE && (changed & WS_EX_LAYOUTRTL))
            sync_window_region(data, (HRGN)1);
    }

    release_win_data(data);
}


/*****************************************************************
 *              SetWindowText   (MACDRV.@)
 */
void macdrv_SetWindowText(HWND hwnd, LPCWSTR text)
{
    WineWindow *win;

    TRACE("%p, %s\n", hwnd, debugstr_w(text));

    if ((win = macdrv_get_cocoa_window(hwnd, FALSE)))
        macdrv_set_cocoa_window_title(win, text, wcslen(text));
}


/***********************************************************************
 *              ShowWindow   (MACDRV.@)
 */
UINT macdrv_ShowWindow(HWND hwnd, INT cmd, RECT *rect, UINT swp)
{
    struct macdrv_thread_data *thread_data = macdrv_thread_data();
    struct macdrv_win_data *data = get_win_data(hwnd);
    CGRect frame;

    TRACE("win %p/%p cmd %d at %s flags %08x\n",
          hwnd, data ? data->cocoa_window : NULL, cmd, wine_dbgstr_rect(rect), swp);

    if (!data || !data->cocoa_window) goto done;
    if (NtUserGetWindowLongW(hwnd, GWL_STYLE) & WS_MINIMIZE)
    {
        if (rect->left != -32000 || rect->top != -32000)
        {
            OffsetRect(rect, -32000 - rect->left, -32000 - rect->top);
            swp &= ~(SWP_NOMOVE | SWP_NOCLIENTMOVE);
        }
        goto done;
    }
    if (!data->on_screen) goto done;

    /* only fetch the new rectangle if the ShowWindow was a result of an external event */

    if (!thread_data->current_event || thread_data->current_event->window != data->cocoa_window)
        goto done;

    if (thread_data->current_event->type != WINDOW_FRAME_CHANGED &&
        thread_data->current_event->type != WINDOW_DID_UNMINIMIZE)
        goto done;

    if (!macdrv_get_cocoa_window_frame(data->cocoa_window, &frame)) goto done;
    *rect = rect_from_cgrect(frame);
    *rect = window_rect_from_visible(&data->rects, *rect);
    TRACE("rect %s -> %s\n", wine_dbgstr_cgrect(frame), wine_dbgstr_rect(rect));
    swp &= ~(SWP_NOMOVE | SWP_NOCLIENTMOVE | SWP_NOSIZE | SWP_NOCLIENTSIZE);

done:
    release_win_data(data);
    return swp;
}


/***********************************************************************
 *              SysCommand   (MACDRV.@)
 *
 * Perform WM_SYSCOMMAND handling.
 */
LRESULT macdrv_SysCommand(HWND hwnd, WPARAM wparam, LPARAM lparam, const POINT *pos)
{
    struct macdrv_win_data *data;
    LRESULT ret = -1;
    WPARAM command = wparam & 0xfff0;

    TRACE("%p, %x, %lx\n", hwnd, (unsigned)wparam, lparam);

    if (!(data = get_win_data(hwnd))) goto done;
    if (!data->cocoa_window || !data->on_screen) goto done;

    /* prevent a simple ALT press+release from activating the system menu,
       as that can get confusing */
    if (command == SC_KEYMENU && !(WCHAR)lparam &&
        !NtUserGetWindowLongPtrW(hwnd, GWLP_ID) &&
        (NtUserGetWindowLongW(hwnd, GWL_STYLE) & WS_SYSMENU))
    {
        TRACE("ignoring SC_KEYMENU wp %lx lp %lx\n", (unsigned long)wparam, lparam);
        ret = 0;
    }

    if (command == SC_MOVE)
    {
        release_win_data(data);
        return move_window(hwnd, wparam);
    }

done:
    release_win_data(data);
    return ret;
}


/***********************************************************************
 *              UpdateLayeredWindow   (MACDRV.@)
 */
void macdrv_UpdateLayeredWindow(HWND hwnd, BYTE alpha, UINT flags)
{
    struct macdrv_win_data *data;

    if ((data = get_win_data(hwnd)))
    {
        /* Since layered attributes are now set, can now show the window */
        if (data->cocoa_window && !data->on_screen && NtUserGetWindowLongW(hwnd, GWL_STYLE) & WS_VISIBLE)
            show_window(data);

        /* The ULW flags are a superset of the LWA flags. */
        sync_window_opacity(data, alpha, TRUE, flags);
        release_win_data(data);
    }
}


/**********************************************************************
 *              WindowMessage   (MACDRV.@)
 */
LRESULT macdrv_WindowMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    struct macdrv_win_data *data;

    TRACE("%p, %u, %u, %lu\n", hwnd, msg, (unsigned)wp, lp);

    switch(msg)
    {
    case WM_MACDRV_SET_WIN_REGION:
        if ((data = get_win_data(hwnd)))
        {
            sync_window_region(data, (HRGN)1);
            release_win_data(data);
        }
        return 0;
    case WM_MACDRV_ACTIVATE_ON_FOLLOWING_FOCUS:
        activate_on_following_focus();
        TRACE("WM_MACDRV_ACTIVATE_ON_FOLLOWING_FOCUS time %u\n", activate_on_focus_time);
        return 0;
    case WM_MACDRV_CREATE_REMOTE_LAYER:
        if ((data = get_win_data(hwnd)))
        {
            TRACE("WM_MACDRV_CREATE_REMOTE_LAYER context_id %u\n", (unsigned int)lp);
            if (data->cocoa_window) macdrv_window_create_ca_layer_host_view(data->cocoa_window, (unsigned int)lp);
            release_win_data(data);
        }
        return 0;
    case WM_MACDRV_RELEASE_REMOTE_LAYER:
        if ((data = get_win_data(hwnd)))
        {
            TRACE("WM_MACDRV_RELEASE_REMOTE_LAYER context_id %u\n", (unsigned int)lp);
            if (data->cocoa_window) macdrv_window_release_ca_layer_host_view(data->cocoa_window, (unsigned int)lp);
            release_win_data(data);
        }
        return 0;
    }

    FIXME("unrecognized window msg %x hwnd %p wp %lx lp %lx\n", msg, hwnd, (unsigned long)wp, lp);
    return 0;
}


void macdrv_create_remote_layer(void* hwnd_ptr, unsigned int context_id)
{
    NtUserPostMessage((HWND)hwnd_ptr, WM_MACDRV_CREATE_REMOTE_LAYER, 0, context_id);
}


void macdrv_release_remote_layer(void* hwnd_ptr, unsigned int context_id)
{
    NtUserPostMessage((HWND)hwnd_ptr, WM_MACDRV_RELEASE_REMOTE_LAYER, 0, context_id);
}


/***********************************************************************
 *              WindowPosChanging   (MACDRV.@)
 */
BOOL macdrv_WindowPosChanging(HWND hwnd, UINT swp_flags, BOOL shaped, const struct window_rects *rects)
{
    struct macdrv_win_data *data = get_win_data(hwnd);
    BOOL ret = FALSE;

    TRACE("hwnd %p, swp_flags %04x, shaped %u, rects %s\n", hwnd, swp_flags, shaped, debugstr_window_rects(rects));

    if (!data && !(data = macdrv_create_win_data(hwnd, rects))) return FALSE; /* use default surface */
    if (data->cocoa_window && shaped != data->shaped) set_shape_pending(data, shaped);
    data->shaped = shaped;

    ret = !!data->cocoa_window; /* use default surface if we don't have a window */
    release_win_data(data);

    return ret;
}


/***********************************************************************
 *      GetWindowStyleMasks   (X11DRV.@)
 */
BOOL macdrv_GetWindowStyleMasks(HWND hwnd, UINT style, UINT ex_style, UINT *style_mask, UINT *ex_style_mask)
{
    struct macdrv_window_features wf = get_window_features_for_style(style, ex_style, FALSE);

    *style_mask = *ex_style_mask = 0;
    if (wf.title_bar)
    {
        *style_mask |= WS_CAPTION;
        *ex_style_mask |= WS_EX_TOOLWINDOW;
    }
    if (wf.shadow)
    {
        *style_mask |= WS_DLGFRAME | WS_THICKFRAME;
        *ex_style_mask |= WS_EX_DLGMODALFRAME;
    }

    return TRUE;
}


/***********************************************************************
 *              get_fullscreen_content_rect
 *
 * A full-screen window keeps its aspect ratio in a display mode that
 * win32u emulates, and win32u widens its visible rect to the whole screen.
 * Only then does the visible rect extend past the window rect.  Get the
 * window rect relative to the visible rect, the part of the Cocoa window
 * that the window's content covers.
 */
bool get_fullscreen_content_rect(const struct window_rects *rects, RECT *rect)
{
    if (EqualRect(&rects->window, &rects->visible)) return false;
    /* The frame of a decorated window lies outside its visible rect, and the
     * visible rect win32u had before widening it is not known here: it is
     * the window rect less the parts of the frame the Cocoa window draws,
     * which is not the client rect when the window has a menu bar or a
     * border Cocoa does not draw.  Such a window gets no bars, and its
     * surface stays at the left edge of the screen. */
    if (rects->window.left < rects->visible.left || rects->window.top < rects->visible.top ||
        rects->window.right > rects->visible.right || rects->window.bottom > rects->visible.bottom)
        return false;

    *rect = rects->window;
    OffsetRect(rect, -rects->visible.left, -rects->visible.top);
    return true;
}


/***********************************************************************
 *              WindowPosChanged   (MACDRV.@)
 */
void macdrv_WindowPosChanged(HWND hwnd, HWND insert_after, HWND owner_hint, UINT swp_flags,
                             const struct window_rects *new_rects, struct window_surface *surface)
{
    BOOL fullscreen = !!(swp_flags & WINE_SWP_FULLSCREEN);
    struct macdrv_thread_data *thread_data;
    struct macdrv_win_data *data;
    unsigned int new_style = NtUserGetWindowLongW(hwnd, GWL_STYLE);
    struct window_rects old_rects;

    if (!(data = get_win_data(hwnd))) return;

    thread_data = macdrv_thread_data();

    old_rects = data->rects;
    data->rects = *new_rects;

    TRACE("win %p/%p new_rects %s style %08x flags %08x surface %p\n", hwnd, data->cocoa_window,
          debugstr_window_rects(new_rects), new_style, swp_flags, surface);

    if (!data->cocoa_window) goto done;

    if (data->on_screen)
    {
        if ((swp_flags & SWP_HIDEWINDOW) && !(new_style & WS_VISIBLE))
            hide_window(data);
    }

    /* check if we are currently processing an event relevant to this window */
    if (!thread_data || !thread_data->current_event ||
        !data->cocoa_window || thread_data->current_event->window != data->cocoa_window ||
        (thread_data->current_event->type != WINDOW_FRAME_CHANGED &&
         thread_data->current_event->type != WINDOW_DID_UNMINIMIZE))
    {
        sync_window_position(data, swp_flags, &old_rects);
        if (data->cocoa_window)
            set_cocoa_window_properties(data);
    }

    if (new_style & WS_VISIBLE)
    {
        if (data->cocoa_window)
        {
            if (!data->on_screen || (swp_flags & (SWP_FRAMECHANGED|SWP_STATECHANGED)))
                set_cocoa_window_properties(data);

            /* layered windows are not shown until their attributes are set */
            if (!data->on_screen &&
                (data->layered || !(NtUserGetWindowLongW( hwnd, GWL_EXSTYLE ) & WS_EX_LAYERED)))
                show_window(data);
        }
    }

    if (fullscreen || fullscreen != data->fullscreen)
    {
        RECT content;
        CGRect rect = (fullscreen && get_fullscreen_content_rect(&data->rects, &content)) ? cgrect_from_rect(content) : CGRectZero;
        macdrv_set_window_mask(data->cocoa_window, rect);
        data->fullscreen = fullscreen;
    }

done:
    release_win_data(data);

    /* the window may have been shown, hidden, moved or reordered above a
     * client view, or the client view may have moved */
    update_client_overlay(hwnd, true);
}


/***********************************************************************
 *              macdrv_window_close_requested
 *
 * Handler for WINDOW_CLOSE_REQUESTED events.
 */
void macdrv_window_close_requested(HWND hwnd)
{
    HMENU sysmenu;

    if (NtUserGetClassLongW(hwnd, GCL_STYLE) & CS_NOCLOSE)
    {
        TRACE("not closing win %p class style CS_NOCLOSE\n", hwnd);
        return;
    }

    sysmenu = NtUserGetSystemMenu(hwnd, FALSE);
    if (sysmenu)
    {
        UINT state = NtUserThunkedMenuItemInfo(sysmenu, SC_CLOSE, MF_BYCOMMAND,
                                               NtUserGetMenuState, NULL, NULL);
        if (state == 0xFFFFFFFF || (state & (MF_DISABLED | MF_GRAYED)))
        {
            TRACE("not closing win %p menu state 0x%08x\n", hwnd, state);
            return;
        }
    }

    perform_window_command(hwnd, 0, 0, SC_CLOSE, HTCLOSE);
}


/***********************************************************************
 *              macdrv_window_frame_changed
 *
 * Handler for WINDOW_FRAME_CHANGED events.
 */
void macdrv_window_frame_changed(HWND hwnd, const macdrv_event *event)
{
    struct macdrv_win_data *data;
    RECT rect;
    UINT flags = SWP_NOACTIVATE | SWP_NOZORDER;
    int width, height;
    BOOL being_dragged;

    if (!hwnd) return;
    if (!(data = get_win_data(hwnd))) return;
    if (!data->on_screen || data->minimized)
    {
        release_win_data(data);
        return;
    }

    /* Get geometry */

    TRACE("win %p/%p new Cocoa frame %s fullscreen %d in_resize %d\n", hwnd, data->cocoa_window,
          wine_dbgstr_cgrect(event->window_frame_changed.frame),
          event->window_frame_changed.fullscreen, event->window_frame_changed.in_resize);

    rect = rect_from_cgrect(event->window_frame_changed.frame);
    rect = window_rect_from_visible(&data->rects, rect);
    width = rect.right - rect.left;
    height = rect.bottom - rect.top;

    if (data->rects.window.left == rect.left && data->rects.window.top == rect.top)
        flags |= SWP_NOMOVE;
    else
        TRACE("%p moving from (%d,%d) to (%d,%d)\n", hwnd, data->rects.window.left,
              data->rects.window.top, rect.left, rect.top);

    if ((data->rects.window.right - data->rects.window.left == width &&
         data->rects.window.bottom - data->rects.window.top == height) ||
        (IsRectEmpty(&data->rects.window) && width == 1 && height == 1))
        flags |= SWP_NOSIZE;
    else
        TRACE("%p resizing from (%dx%d) to (%dx%d)\n", hwnd, data->rects.window.right - data->rects.window.left,
              data->rects.window.bottom - data->rects.window.top, width, height);

    being_dragged = data->drag_event != NULL;
    release_win_data(data);

    if (event->window_frame_changed.fullscreen)
        flags |= SWP_NOSENDCHANGING;
    if (!(flags & SWP_NOSIZE) || !(flags & SWP_NOMOVE))
    {
        bool send_sizemove = !event->window_frame_changed.in_resize && !being_dragged && !event->window_frame_changed.skip_size_move_loop;
        if (send_sizemove)
            send_message(hwnd, WM_ENTERSIZEMOVE, 0, 0);
        NtUserSetRawWindowPos(hwnd, rect, flags, FALSE);
        if (send_sizemove)
            send_message(hwnd, WM_EXITSIZEMOVE, 0, 0);
    }
}


/***********************************************************************
 *              macdrv_window_got_focus
 *
 * Handler for WINDOW_GOT_FOCUS events.
 */
void macdrv_window_got_focus(HWND hwnd, const macdrv_event *event)
{
    unsigned int style = NtUserGetWindowLongW(hwnd, GWL_STYLE);

    if (!hwnd) return;

    TRACE("win %p/%p serial %lu enabled %d visible %d style %08x focus %p active %p fg %p\n",
          hwnd, event->window, event->window_got_focus.serial, NtUserIsWindowEnabled(hwnd),
          NtUserIsWindowVisible(hwnd), style, get_focus(), get_active_window(), NtUserGetForegroundWindow());

    if (can_window_become_foreground(hwnd) && !(style & WS_MINIMIZE))
    {
        TRACE("setting foreground window to %p\n", hwnd);
        NtUserSetForegroundWindowInternal(hwnd);
        return;
    }

    TRACE("win %p/%p rejecting focus\n", hwnd, event->window);
    macdrv_window_rejected_focus(event);
}


/***********************************************************************
 *              macdrv_window_lost_focus
 *
 * Handler for WINDOW_LOST_FOCUS events.
 */
void macdrv_window_lost_focus(HWND hwnd, const macdrv_event *event)
{
    if (!hwnd) return;

    TRACE("win %p/%p fg %p\n", hwnd, event->window, NtUserGetForegroundWindow());

    if (hwnd == NtUserGetForegroundWindow())
    {
        send_message(hwnd, WM_CANCELMODE, 0, 0);
        if (hwnd == NtUserGetForegroundWindow())
            NtUserSetForegroundWindowInternal(NtUserGetDesktopWindow());
    }
}


/***********************************************************************
 *              macdrv_app_activated
 *
 * Handler for APP_ACTIVATED events.
 */
void macdrv_app_activated(void)
{
    TRACE("\n");
    macdrv_UpdateClipboard();
}


/***********************************************************************
 *              macdrv_app_deactivated
 *
 * Handler for APP_DEACTIVATED events.
 */
void macdrv_app_deactivated(void)
{
    NtUserClipCursor(NULL);

    if (get_active_window() == NtUserGetForegroundWindow())
    {
        TRACE("setting fg to desktop\n");
        NtUserSetForegroundWindowInternal(NtUserGetDesktopWindow());
    }
}


/***********************************************************************
 *              macdrv_window_maximize_requested
 *
 * Handler for WINDOW_MAXIMIZE_REQUESTED events.
 */
void macdrv_window_maximize_requested(HWND hwnd)
{
    perform_window_command(hwnd, WS_MAXIMIZEBOX, WS_MAXIMIZE, SC_MAXIMIZE, HTMAXBUTTON);
}


/***********************************************************************
 *              macdrv_window_minimize_requested
 *
 * Handler for WINDOW_MINIMIZE_REQUESTED events.
 */
void macdrv_window_minimize_requested(HWND hwnd)
{
    perform_window_command(hwnd, WS_MINIMIZEBOX, WS_MINIMIZE, SC_MINIMIZE, HTMINBUTTON);
}


/***********************************************************************
 *              macdrv_window_did_minimize
 *
 * Handler for WINDOW_DID_MINIMIZE events.
 */
void macdrv_window_did_minimize(HWND hwnd)
{
    TRACE("win %p\n", hwnd);

    /* If all our windows are minimized, disable cursor clipping. */
    if (!macdrv_is_any_wine_window_visible())
        NtUserClipCursor(NULL);
}


/***********************************************************************
 *              macdrv_window_did_unminimize
 *
 * Handler for WINDOW_DID_UNMINIMIZE events.
 */
void macdrv_window_did_unminimize(HWND hwnd)
{
    struct macdrv_win_data *data;
    unsigned int style;

    TRACE("win %p\n", hwnd);

    if (!(data = get_win_data(hwnd))) return;
    if (!data->minimized) goto done;

    style = NtUserGetWindowLongW(hwnd, GWL_STYLE);

    data->minimized = FALSE;
    if ((style & (WS_MINIMIZE | WS_VISIBLE)) == (WS_MINIMIZE | WS_VISIBLE))
    {
        TRACE("restoring win %p/%p\n", hwnd, data->cocoa_window);
        release_win_data(data);
        NtUserSetActiveWindow(hwnd);
        send_message(hwnd, WM_SYSCOMMAND, SC_RESTORE, 0);
        return;
    }

    TRACE("not restoring win %p/%p style %08x\n", hwnd, data->cocoa_window, style);

done:
    release_win_data(data);
}


/***********************************************************************
 *              macdrv_window_brought_forward
 *
 * Handler for WINDOW_BROUGHT_FORWARD events.
 */
void macdrv_window_brought_forward(HWND hwnd)
{
    TRACE("win %p\n", hwnd);
    NtUserSetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}


/***********************************************************************
 *              macdrv_window_resize_ended
 *
 * Handler for WINDOW_RESIZE_ENDED events.
 */
void macdrv_window_resize_ended(HWND hwnd)
{
    TRACE("hwnd %p\n", hwnd);
    send_message(hwnd, WM_EXITSIZEMOVE, 0, 0);
}


/***********************************************************************
 *              macdrv_window_restore_requested
 *
 * Handler for WINDOW_RESTORE_REQUESTED events.  This is specifically
 * for restoring from maximized, not from minimized.
 */
void macdrv_window_restore_requested(HWND hwnd, const macdrv_event *event)
{
    if (event->window_restore_requested.keep_frame && hwnd)
    {
        DWORD style = NtUserGetWindowLongW(hwnd, GWL_STYLE);
        struct macdrv_win_data *data;

        if ((style & WS_MAXIMIZE) && (style & WS_VISIBLE) && (data = get_win_data(hwnd)))
        {
            RECT rect = rect_from_cgrect(event->window_restore_requested.frame);
            rect = window_rect_from_visible(&data->rects, rect);
            release_win_data(data);

            NtUserSetRawWindowPos(hwnd, rect, 0, TRUE);
        }
    }

    perform_window_command(hwnd, WS_MAXIMIZE, 0, SC_RESTORE, HTMAXBUTTON);
}


/***********************************************************************
 *              macdrv_window_drag_begin
 *
 * Handler for WINDOW_DRAG_BEGIN events.
 */
void macdrv_window_drag_begin(HWND hwnd, const macdrv_event *event)
{
    DWORD style = NtUserGetWindowLongW(hwnd, GWL_STYLE);
    struct macdrv_win_data *data;
    HANDLE drag_event = NULL;
    OBJECT_ATTRIBUTES attr;
    BOOL loop = TRUE;
    MSG msg;

    TRACE("win %p\n", hwnd);

    if (style & (WS_DISABLED | WS_MAXIMIZE | WS_MINIMIZE)) return;
    if (!(style & WS_VISIBLE)) return;

    if (!(data = get_win_data(hwnd))) return;
    if (data->drag_event) goto done;

    InitializeObjectAttributes(&attr, NULL, OBJ_OPENIF, NULL, NULL);
    if (NtCreateEvent(&drag_event, EVENT_ALL_ACCESS, &attr, NotificationEvent, FALSE)) goto done;

    data->drag_event = drag_event;
    release_win_data(data);

    if (!event->window_drag_begin.no_activate && can_window_become_foreground(hwnd) &&
        NtUserGetForegroundWindow() != hwnd)
    {
        /* ask whether the window wants to be activated */
        LRESULT ma = send_message(hwnd, WM_MOUSEACTIVATE, (WPARAM)NtUserGetAncestor(hwnd, GA_ROOT),
                                  MAKELONG(HTCAPTION, WM_LBUTTONDOWN));
        if (ma != MA_NOACTIVATEANDEAT && ma != MA_NOACTIVATE)
        {
            TRACE("setting foreground window to %p\n", hwnd);
            NtUserSetForegroundWindowInternal(hwnd);
        }
    }

    NtUserClipCursor(NULL);
    send_message(hwnd, WM_ENTERSIZEMOVE, 0, 0);
    NtUserReleaseCapture();

    while (loop)
    {
        while (!NtUserPeekMessage(&msg, 0, 0, 0, PM_REMOVE))
        {
            DWORD result = NtUserMsgWaitForMultipleObjectsEx(1, &drag_event, INFINITE, QS_ALLINPUT,
                                                             MWMO_INPUTAVAILABLE);
            if (result == WAIT_OBJECT_0)
            {
                loop = FALSE;
                break;
            }
        }
        if (!loop)
            break;

        if (msg.message == WM_QUIT)
            break;

        if (!NtUserCallMsgFilter(&msg, MSGF_SIZE) && msg.message != WM_KEYDOWN &&
            msg.message != WM_MOUSEMOVE && msg.message != WM_LBUTTONDOWN && msg.message != WM_LBUTTONUP)
        {
            NtUserTranslateMessage(&msg, 0);
            NtUserDispatchMessage(&msg);
        }
    }

    send_message(hwnd, WM_EXITSIZEMOVE, 0, 0);

    TRACE("done\n");

    if ((data = get_win_data(hwnd)))
        data->drag_event = NULL;

done:
    release_win_data(data);
    if (drag_event) NtClose(drag_event);
}


/***********************************************************************
 *              macdrv_window_drag_end
 *
 * Handler for WINDOW_DRAG_END events.
 */
void macdrv_window_drag_end(HWND hwnd)
{
    struct macdrv_win_data *data;

    TRACE("win %p\n", hwnd);

    if (!(data = get_win_data(hwnd))) return;
    if (data->drag_event)
        NtSetEvent(data->drag_event, NULL);
    release_win_data(data);
}


/***********************************************************************
 *              macdrv_reassert_window_position
 *
 * Handler for REASSERT_WINDOW_POSITION events.
 */
void macdrv_reassert_window_position(HWND hwnd)
{
    struct macdrv_win_data *data = get_win_data(hwnd);
    if (data)
    {
        if (data->cocoa_window && data->on_screen)
            sync_window_position(data, SWP_NOZORDER | SWP_NOACTIVATE, NULL);
        release_win_data(data);
    }
}


/***********************************************************************
 *              macdrv_app_quit_requested
 *
 * Handler for APP_QUIT_REQUESTED events.
 */
void macdrv_app_quit_requested(const macdrv_event *event)
{
    struct app_quit_request_params params = {.dispatch = {.callback = app_quit_request_callback}};
    void *ret_ptr;
    ULONG ret_len;

    TRACE("reason %d\n", event->app_quit_requested.reason);

    if (event->app_quit_requested.reason == QUIT_REASON_LOGOUT)
        params.flags = ENDSESSION_LOGOFF;

    KeUserDispatchCallback(&params.dispatch, sizeof(params), &ret_ptr, &ret_len);
}


/***********************************************************************
 *              query_resize_size
 *
 * Handler for QUERY_RESIZE_SIZE query.
 */
bool query_resize_size(HWND hwnd, macdrv_query *query)
{
    struct macdrv_win_data *data = get_win_data(hwnd);
    RECT rect;
    int corner;
    bool ret = false;

    if (!data) return false;

    rect = rect_from_cgrect(query->resize_size.rect);
    rect = window_rect_from_visible(&data->rects, rect);

    if (query->resize_size.from_left)
    {
        if (query->resize_size.from_top)
            corner = WMSZ_TOPLEFT;
        else
            corner = WMSZ_BOTTOMLEFT;
    }
    else if (query->resize_size.from_top)
        corner = WMSZ_TOPRIGHT;
    else
        corner = WMSZ_BOTTOMRIGHT;

    if (send_message(hwnd, WM_SIZING, corner, (LPARAM)&rect))
    {
        rect = visible_rect_from_window(&data->rects, rect);
        query->resize_size.rect = cgrect_from_rect(rect);
        ret = true;
    }

    release_win_data(data);
    return ret;
}


/***********************************************************************
 *              query_resize_start
 *
 * Handler for QUERY_RESIZE_START query.
 */
bool query_resize_start(HWND hwnd)
{
    TRACE("hwnd %p\n", hwnd);

    NtUserClipCursor(NULL);

    sync_window_min_max_info(hwnd);
    send_message(hwnd, WM_ENTERSIZEMOVE, 0, 0);

    return true;
}


/***********************************************************************
 *              query_min_max_info
 *
 * Handler for QUERY_MIN_MAX_INFO query.
 */
bool query_min_max_info(HWND hwnd)
{
    TRACE("hwnd %p\n", hwnd);
    sync_window_min_max_info(hwnd);
    return true;
}


/***********************************************************************
 *              init_win_context
 */
void init_win_context(void)
{
    pthread_mutexattr_t attr;

    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&win_data_mutex, &attr);
    pthread_mutexattr_destroy(&attr);
}


/***********************************************************************
 *              DXMT interface
 *
 * DXMT (https://github.com/3Shain/dxmt) and mtld3d (https://github.com/athei/mtld3d)
 * attach a Metal view to a window's client view through a function table exported
 * as "macdrv_functions", using the window data layout that CrossOver provides.
 */
struct dxmt_win_data
{
    HWND                    hwnd;
    WineWindow             *cocoa_window;
    WineContentView        *cocoa_view;         /* unused by the consumers, left NULL as on CrossOver */
    WineContentView        *client_cocoa_view;
    struct macdrv_win_data *data;               /* not part of the DXMT layout */
};

/* The consumers declare the first four fields and read them at these offsets. */
C_ASSERT(offsetof(struct dxmt_win_data, hwnd) == 0);
C_ASSERT(offsetof(struct dxmt_win_data, cocoa_window) == 1 * sizeof(void *));
C_ASSERT(offsetof(struct dxmt_win_data, cocoa_view) == 2 * sizeof(void *));
C_ASSERT(offsetof(struct dxmt_win_data, client_cocoa_view) == 3 * sizeof(void *));

static void cf_client_surface_release(CFAllocatorRef allocator, const void *client_surface)
{
    client_surface_release((struct client_surface *)client_surface);
}

static struct dxmt_win_data *dxmt_get_win_data(HWND hwnd)
{
    struct macdrv_client_surface *surface;
    struct client_surface *client;
    struct macdrv_win_data *data;
    struct dxmt_win_data *ret;

    TRACE("%p\n", hwnd);

    /* Create a client surface on every call, as CrossOver does: the consumers
     * call this only to attach a Metal view to the window, and the surface's
     * cocoa view, which creating it makes the one the window shows, is where
     * that view goes. Create it before taking the window data, which creating
     * it takes as well. */
    if (!(client = macdrv_CreateClientSurface(hwnd, 0, FALSE))) return NULL;
    surface = impl_from_client_surface(client);

    if (!(data = get_win_data(hwnd)))
    {
        client_surface_release(client);
        return NULL;
    }
    if (!(ret = calloc(1, sizeof(*ret))))
    {
        release_win_data(data);
        client_surface_release(client);
        return NULL;
    }

    /* Let WineMetalLayer, and mtld3d, find the surface from its cocoa view. */
    macdrv_set_view_d3dmetal_client_surface(surface->cocoa_view, client);

    /* The window data keeps the reference until macdrv_DestroyWindow. */
    if (!data->d3dmetal_client_surfaces)
    {
        static const CFArrayCallBacks callbacks = { .release = cf_client_surface_release };
        data->d3dmetal_client_surfaces = CFArrayCreateMutable(NULL, 0, &callbacks);
    }
    CFArrayAppendValue(data->d3dmetal_client_surfaces, client);
    d3dmetal_surface_count++;

    ret->hwnd = data->hwnd;
    ret->cocoa_window = data->cocoa_window;
    ret->client_cocoa_view = surface->cocoa_view;
    ret->data = data;
    return ret;
}

/***********************************************************************
 *              is_d3dmetal_client_view_presenting
 *
 * Whether the window's client view is the cocoa view of a client surface
 * created through macdrv_functions that has presented a frame and still
 * holds a Metal view, one that DXMT or mtld3d has not released. Until the
 * first frame the Metal layer shows only its black background, which would
 * cover what the window draws with GDI before. The caller holds the window
 * data.
 */
bool is_d3dmetal_client_view_presenting(struct macdrv_win_data *data)
{
    struct macdrv_client_surface *surface = get_d3dmetal_client_view_surface(data);

    return surface && surface->presented && macdrv_view_has_metal_view(data->client_view);
}

/* The client surface created through macdrv_functions whose cocoa view is
 * the window's client view, if any. The caller holds the window data. */
static struct macdrv_client_surface *get_d3dmetal_client_view_surface(const struct macdrv_win_data *data)
{
    CFIndex i, count;

    if (!data->client_view || !data->d3dmetal_client_surfaces) return NULL;

    count = CFArrayGetCount(data->d3dmetal_client_surfaces);
    for (i = 0; i < count; i++)
    {
        struct client_surface *client = (struct client_surface *)CFArrayGetValueAtIndex(data->d3dmetal_client_surfaces, i);
        struct macdrv_client_surface *surface = impl_from_client_surface(client);

        if (surface->cocoa_view == data->client_view) return surface;
    }
    return NULL;
}

static void dxmt_release_win_data(struct dxmt_win_data *win_data)
{
    if (!win_data) return;
    release_win_data(win_data->data);
    free(win_data);
}

static WineWindow *dxmt_get_cocoa_window(HWND hwnd, BOOL require_on_screen)
{
    return macdrv_get_cocoa_window(hwnd, require_on_screen);
}

static void dxmt_init_display_devices(BOOL force)
{
    TRACE("%d - no-op\n", force);
}

struct find_client_surface_ctx
{
    const void *client;
    BOOL        found;
};

static void find_client_surface(const void *key, const void *value, void *context)
{
    const struct macdrv_win_data *data = value;
    struct find_client_surface_ctx *ctx = context;

    if (!ctx->found && data->d3dmetal_client_surfaces &&
        CFArrayContainsValue(data->d3dmetal_client_surfaces,
                             CFRangeMake(0, CFArrayGetCount(data->d3dmetal_client_surfaces)), ctx->client))
        ctx->found = TRUE;
}

/***********************************************************************
 *              macdrv_client_surface_presented
 *
 * Handler for CLIENT_SURFACE_PRESENTED events, which WineMetalLayer posts
 * when a Metal layer outside Wine takes a drawable from a client surface
 * created through macdrv_functions.
 */
void macdrv_client_surface_presented(const macdrv_event *event)
{
    struct find_client_surface_ctx ctx = { event->client_surface_presented.client_surface, FALSE };
    struct client_surface *client = event->client_surface_presented.client_surface;
    bool update = false;
    HWND hwnd = NULL;

    TRACE("client_surface %p\n", client);

    /* The event carries a bare pointer, and macdrv_DestroyWindow may have
     * released the surface since it was posted. Only present a surface that
     * the data of some window of this process still holds, which covers
     * swapchains on child windows, and keep a reference on it across the
     * present, which takes win32u's surface lock and then the window data,
     * so the window data must not be held then. */
    pthread_mutex_lock(&win_data_mutex);
    if (win_datas) CFDictionaryApplyFunction(win_datas, find_client_surface, &ctx);
    if (ctx.found)
    {
        struct macdrv_client_surface *surface = impl_from_client_surface(client);

        client_surface_add_ref(client);
        /* the event comes from a frame, see is_d3dmetal_client_view_presenting */
        update = !surface->presented;
        surface->presented = true;
        hwnd = client->hwnd;
    }
    pthread_mutex_unlock(&win_data_mutex);

    if (!ctx.found)
    {
        TRACE("client_surface %p is gone\n", client);
        return;
    }
    client_surface_present(client);
    client_surface_release(client);

    /* a view that starts to show frames goes into the client overlay */
    if (update) update_client_overlay(hwnd, false);
}

struct macdrv_functions_table
{
    void                   (*init_display_devices)(BOOL);
    struct dxmt_win_data  *(*get_win_data)(HWND);
    void                   (*release_win_data)(struct dxmt_win_data *);
    WineWindow            *(*get_cocoa_window)(HWND, BOOL);
    MTLDevice_id           (*create_metal_device)(void);
    void                   (*release_metal_device)(MTLDevice_id);
    WineMetalView         *(*view_create_metal_view)(WineContentView *, MTLDevice_id);
    CAMetalLayer          *(*view_get_metal_layer)(WineMetalView *);
    void                   (*view_release_metal_view)(WineMetalView *);
    void                   (*on_main_thread)(void *);
};

/* The consumers declare these ten entries in this order and read nothing past
 * them. CrossOver's table has more entries after on_main_thread, which only
 * D3DMetal uses. */
C_ASSERT(offsetof(struct macdrv_functions_table, get_win_data) == 1 * sizeof(void *));
C_ASSERT(offsetof(struct macdrv_functions_table, on_main_thread) == 9 * sizeof(void *));
C_ASSERT(sizeof(struct macdrv_functions_table) == 10 * sizeof(void *));

__attribute__((visibility("default"))) const struct macdrv_functions_table macdrv_functions =
{
    dxmt_init_display_devices,
    dxmt_get_win_data,
    dxmt_release_win_data,
    dxmt_get_cocoa_window,
    macdrv_create_metal_device,
    macdrv_release_metal_device,
    macdrv_view_create_metal_view,
    macdrv_view_get_metal_layer,
    macdrv_view_release_metal_view,
    macdrv_run_on_main_thread,
};
