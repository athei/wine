/*
 * Mac driver OpenGL support through EGL
 *
 * Copyright 2026 David Andrews
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

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "macdrv.h"
#include "wine/debug.h"
#include "wine/opengl_driver.h"

WINE_DEFAULT_DEBUG_CHANNEL(wgl);

/* OpenGL through Mesa's EGL, with Zink on KosmicKrisp (Vulkan on Metal). win32u does the
 * contexts, pixel formats and pbuffers; this gives it the window surfaces, which render into
 * the CAMetalLayer of the client surface's Metal view, the layer Vulkan surfaces use. */

struct macdrv_egl_drawable
{
    struct opengl_drawable base;
    int applied_interval;   /* swap interval last set on the EGL surface, -1 before the first swap */
    BOOL scaled;            /* DPI virtualized when created, so win32u renders it at its virtual size */
};

static const struct opengl_funcs *funcs;
static const struct egl_platform *egl;
static EGLDisplay egl_display;
static struct opengl_driver_funcs macdrv_egl_driver_funcs;
static const struct opengl_drawable_funcs macdrv_egl_drawable_funcs;

static struct macdrv_egl_drawable *impl_from_opengl_drawable(struct opengl_drawable *base)
{
    return CONTAINING_RECORD(base, struct macdrv_egl_drawable, base);
}

static void macdrv_egl_init_egl_platform(struct egl_platform *platform)
{
    platform->type = EGL_PLATFORM_SURFACELESS_MESA;
    platform->native_display = EGL_DEFAULT_DISPLAY;
    /* the display macdrv_egl_init() initialized */
    platform->display = egl_display;
    egl = platform;
}

static BOOL is_drawable_virtualized(const struct opengl_drawable *base)
{
    return !base->client->raw && (base->virtual_size.cx != base->monitor_size.cx ||
                                  base->virtual_size.cy != base->monitor_size.cy);
}

static void check_drawable_size(struct opengl_drawable *base)
{
    struct macdrv_egl_drawable *gl = impl_from_opengl_drawable(base);
    static int once;

    /* The EGL surface takes its size from the Metal layer, which covers the window's
       physical client area. win32u renders a window that is DPI virtualized when its
       drawable is created into a framebuffer of its virtual size and scales that to
       the surface (scale_virtualized_windows); a window whose scale only changes later
       keeps rendering at its physical size. */
    if (gl->scaled || !is_drawable_virtualized(base)) return;
    if (!once++)
        FIXME("DPI scale changed after the surface was created, %s renders at %dx%d instead of %dx%d\n",
              debugstr_opengl_drawable(base), (int)base->monitor_size.cx, (int)base->monitor_size.cy,
              (int)base->virtual_size.cx, (int)base->virtual_size.cy);
}

static BOOL macdrv_egl_surface_create(struct client_surface *client, int format, struct opengl_drawable **drawable)
{
    struct macdrv_client_surface *surface = impl_from_client_surface(client);
    EGLConfig config = egl->configs[(format - 1) % egl->config_count];
    EGLint attribs[5], *attrib = attribs, *srgb = NULL, err = EGL_SUCCESS;
    struct macdrv_egl_drawable *gl;
    CAMetalLayer *layer;

    TRACE("client %s, format %d\n", debugstr_client_surface(client), format);

    if (!macdrv_client_surface_acquire_metal_swapchain(surface)) return FALSE;
    if (!(gl = opengl_drawable_create(&macdrv_egl_drawable_funcs, format, client, NULL))) return FALSE;
    gl->applied_interval = -1;
    gl->scaled = is_drawable_virtualized(&gl->base);

    /* A window is opaque whatever its pixel format's alpha holds, as on Windows. */
    if (!egl->extensions[EGL_EXT_present_opaque])
        WARN("Missing EGL_EXT_present_opaque extension\n");
    else
    {
        *attrib++ = EGL_PRESENT_OPAQUE_EXT;
        *attrib++ = EGL_TRUE;
    }
    if (gl->base.srgb && egl->extensions[EGL_KHR_gl_colorspace])
    {
        srgb = attrib;
        *attrib++ = EGL_GL_COLORSPACE;
        *attrib++ = EGL_GL_COLORSPACE_SRGB;
    }
    *attrib++ = EGL_NONE;

    opengl_drawable_map_buffer(&gl->base, GL_FRONT_LEFT, GL_BACK_LEFT);
    opengl_drawable_map_buffer(&gl->base, GL_FRONT, GL_BACK);
    opengl_drawable_map_buffer(&gl->base, GL_FRONT_AND_BACK, GL_BACK);
    if (gl->base.stereo) opengl_drawable_map_buffer(&gl->base, GL_FRONT_RIGHT, GL_BACK_RIGHT);

    layer = macdrv_swapchain_get_layer(surface->metal_swapchain);
    gl->base.surface = funcs->p_eglCreateWindowSurface(egl->display, config, (EGLNativeWindowType)layer, attribs);
    if (!gl->base.surface) err = funcs->p_eglGetError();
    /* win32u only offers sRGB for the configs that can make an sRGB pbuffer, which Mesa
       decides by the same color buffer as for a window surface. Should a window surface
       still refuse it, keep the window working with a linear one. */
    if (!gl->base.surface && srgb && err == EGL_BAD_MATCH)
    {
        WARN("No sRGB surface for format %d, using a linear one\n", format);
        *srgb = EGL_NONE;
        gl->base.srgb = FALSE;
        gl->base.surface = funcs->p_eglCreateWindowSurface(egl->display, config, (EGLNativeWindowType)layer, attribs);
        if (!gl->base.surface) err = funcs->p_eglGetError();
    }
    if (!gl->base.surface)
    {
        ERR("Failed to create an EGL surface for %s, error %#x\n", debugstr_client_surface(client), err);
        opengl_drawable_release(&gl->base);
        return FALSE;
    }
    check_drawable_size(&gl->base);

    TRACE("Created drawable %s with egl_surface %p\n", debugstr_opengl_drawable(&gl->base), gl->base.surface);
    *drawable = &gl->base;
    return TRUE;
}

static void macdrv_egl_drawable_destroy(struct opengl_drawable *base)
{
    /* The Metal layer belongs to the client surface, which outlives the EGL surface. */
    TRACE("drawable %s\n", debugstr_opengl_drawable(base));
}

static void macdrv_egl_drawable_flush(struct opengl_drawable *base, UINT flags)
{
    TRACE("drawable %s, flags %#x\n", debugstr_opengl_drawable(base), flags);

    /* The swap interval is applied in swap, where the surface is current. The surface follows
       the Metal layer's size on its own. */
    if (flags & GL_FLUSH_UPDATED) check_drawable_size(base);
}

static BOOL macdrv_egl_drawable_swap(struct opengl_drawable *base)
{
    struct macdrv_egl_drawable *gl = impl_from_opengl_drawable(base);
    int interval = 0;

    /* As with CGL, a single-buffered surface would otherwise wait for vsync on every flush. */
    if (allow_vsync && base->doublebuffer && base->interval != INT_MIN) interval = abs(base->interval);

    /* eglSwapInterval applies to the current draw surface */
    if (interval != gl->applied_interval && funcs->p_eglGetCurrentSurface(EGL_DRAW) == base->surface)
    {
        if (funcs->p_eglSwapInterval(egl->display, interval)) gl->applied_interval = interval;
        else WARN("eglSwapInterval %d failed, error %#x\n", interval, funcs->p_eglGetError());
    }

    if (TRACE_ON(wgl))
    {
        EGLint width = 0, height = 0;

        funcs->p_eglQuerySurface(egl->display, base->surface, EGL_WIDTH, &width);
        funcs->p_eglQuerySurface(egl->display, base->surface, EGL_HEIGHT, &height);
        TRACE("drawable %s, surface %dx%d, client %dx%d physical\n", debugstr_opengl_drawable(base),
              width, height, (int)base->monitor_size.cx, (int)base->monitor_size.cy);
    }

    client_surface_present(base->client);
    /* As with the other EGL drivers, SwapBuffers succeeds when EGL can't swap, such as
       without a current context. */
    if (!funcs->p_eglSwapBuffers(egl->display, base->surface))
        WARN("eglSwapBuffers failed, error %#x\n", funcs->p_eglGetError());
    return TRUE;
}

static const struct opengl_drawable_funcs macdrv_egl_drawable_funcs =
{
    .size = sizeof(struct macdrv_egl_drawable),
    .destroy = macdrv_egl_drawable_destroy,
    .flush = macdrv_egl_drawable_flush,
    .swap = macdrv_egl_drawable_swap,
};

/***********************************************************************
 *              macdrv_egl_init
 *
 * Use win32u's EGL driver with window surfaces on Metal layers, whatever
 * driver EGL has. On failure, reason says why.
 */
UINT macdrv_egl_init(const struct opengl_funcs *opengl_funcs, const struct opengl_driver_funcs **driver_funcs,
                     char *reason, size_t len)
{
    const char *driver;
    EGLint major, minor;

    if (!opengl_funcs->egl_handle)
    {
        snprintf(reason, len, "libEGL not loaded");
        return STATUS_NOT_SUPPORTED;
    }
    if (!opengl_funcs->p_eglGetPlatformDisplay || !opengl_funcs->p_eglGetCurrentSurface)
    {
        snprintf(reason, len, "libEGL lacks eglGetPlatformDisplay or eglGetCurrentSurface");
        return STATUS_NOT_SUPPORTED;
    }

    egl_display = opengl_funcs->p_eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
    if (!egl_display || !opengl_funcs->p_eglInitialize(egl_display, &major, &minor))
    {
        snprintf(reason, len, "EGL initialization failed, error %#x", opengl_funcs->p_eglGetError());
        egl_display = EGL_NO_DISPLAY;
        return STATUS_NOT_SUPPORTED;
    }

    driver = opengl_funcs->p_eglGetDisplayDriverName ? opengl_funcs->p_eglGetDisplayDriverName(egl_display) : NULL;
    TRACE("EGL %d.%d, driver %s\n", major, minor, debugstr_a(driver));

    funcs = opengl_funcs;

    /* win32u's own EGL driver for everything but the platform and the window surfaces */
    macdrv_egl_driver_funcs = **driver_funcs;
    macdrv_egl_driver_funcs.p_init_egl_platform = macdrv_egl_init_egl_platform;
    macdrv_egl_driver_funcs.p_surface_create = macdrv_egl_surface_create;
    macdrv_egl_driver_funcs.scale_virtualized_windows = TRUE;

    *driver_funcs = &macdrv_egl_driver_funcs;
    return STATUS_SUCCESS;
}
