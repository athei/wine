/*
 * Emulator initialisation code
 *
 * Copyright 2000 Alexandre Julliard
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

#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dlfcn.h>
#include <limits.h>
#ifdef HAVE_SYS_SYSCTL_H
# include <sys/sysctl.h>
#endif
#ifdef __APPLE__
# include <mach-o/dyld.h>
# include <mach-o/ldsyms.h>
# include <spawn.h>
#endif

#include "main.h"

#if defined(__APPLE__) && defined(__x86_64__) && !defined(HAVE_WINE_PRELOADER)

/* Not using the preloader on x86_64:
 * Reserve the same areas as the preloader does, but using zero-fill sections
 * (the only way to prevent system frameworks from using them, including allocations
 * before main() runs).
 */
__asm__(".zerofill WINE_RESERVE,WINE_RESERVE");
static char __wine_reserve[0x1fffff000] __attribute__((section("WINE_RESERVE, WINE_RESERVE")));

static const struct wine_preload_info preload_info[] =
{
    { __wine_reserve,  sizeof(__wine_reserve)  }, /*         0x1000 -    0x200000000: low 8GB */
    { 0, 0 }                                      /* end of list */
};

const __attribute((visibility("default"))) struct wine_preload_info *wine_main_preload_info = preload_info;

static void init_reserved_areas(void)
{
    int i;

    for (i = 0; wine_main_preload_info[i].size != 0; i++)
    {
        /* Match how the preloader maps reserved areas: */
        mmap(wine_main_preload_info[i].addr, wine_main_preload_info[i].size, PROT_NONE,
             MAP_FIXED | MAP_NORESERVE | MAP_PRIVATE | MAP_ANON, -1, 0);
    }
}

#elif defined(__APPLE__) && defined(__aarch64__)

/* arm64 macOS normally refuses mappings below 4GB. With the cross-architecture entitlement
 * the kernel keeps only one hard page of the 4GB __PAGEZERO and maps the rest as a
 * replaceable PROT_NONE region, which nothing else can have claimed before main() runs.
 * Turn it into Wine's low reserved area. The entitlement is required: before its 4K page exec
 * the loader probes this area and exits with an error without it (see exec_with_4k_pages), so
 * the address check below is only a guard.
 * The kernel slides the loader by a random amount above 4GB and covers the area from 4GB up
 * to it with the same kind of replaceable mapping. Wine only maps over such a mapping in a
 * reserved area, so reserve that area too: executables at 0x100000000 without relocations,
 * as old x64 linkers made them, can only load there. */
static struct wine_preload_info low_reserve[] =
{
    { (void *)0x10000, 0x100000000 - 0x10000 },  /* 64KB - 4GB */
    { (void *)0x100000000, 0 },                   /* 4GB - loader, set in init_reserved_areas */
    { 0, 0 }                                      /* end of list */
};
/* ntdll stops at the first entry of size 0, so the 4GB - loader entry, which stays 0 when
 * it cannot be mapped, must remain the last one before the terminator. */

const __attribute((visibility("default"))) struct wine_preload_info *wine_main_preload_info = NULL;

static void init_reserved_areas(void)
{
    char *loader = (char *)((uintptr_t)&_mh_execute_header & ~(uintptr_t)0xffff);

    if (mmap( low_reserve[0].addr, low_reserve[0].size, PROT_NONE,
              MAP_FIXED | MAP_NORESERVE | MAP_PRIVATE | MAP_ANON, -1, 0 ) != low_reserve[0].addr)
        return;
    wine_main_preload_info = low_reserve;

    if (loader > (char *)low_reserve[1].addr &&
        mmap( low_reserve[1].addr, loader - (char *)low_reserve[1].addr, PROT_NONE,
              MAP_FIXED | MAP_NORESERVE | MAP_PRIVATE | MAP_ANON, -1, 0 ) == low_reserve[1].addr)
        low_reserve[1].size = loader - (char *)low_reserve[1].addr;
}

#else

/* the preloader will set this variable */
const __attribute((visibility("default"))) struct wine_preload_info *wine_main_preload_info = NULL;

static void init_reserved_areas(void)
{
}

#endif

/* canonicalize path and return its directory name */
static char *realpath_dirname( const char *name )
{
    char *p, *fullpath = realpath( name, NULL );

    if (fullpath)
    {
        p = strrchr( fullpath, '/' );
        if (p == fullpath) p++;
        if (p) *p = 0;
    }
    return fullpath;
}

/* if string ends with tail, remove it */
static char *remove_tail( const char *str, const char *tail )
{
    size_t len = strlen( str );
    size_t tail_len = strlen( tail );
    char *ret;

    if (len < tail_len) return NULL;
    if (strcmp( str + len - tail_len, tail )) return NULL;
    ret = malloc( len - tail_len + 1 );
    memcpy( ret, str, len - tail_len );
    ret[len - tail_len] = 0;
    return ret;
}

/* build a path from the specified dir and name */
static char *build_path( const char *dir, const char *name )
{
    size_t len = strlen( dir );
    char *ret = malloc( len + strlen( name ) + 2 );

    memcpy( ret, dir, len );
    if (len && ret[len - 1] != '/') ret[len++] = '/';
    strcpy( ret + len, name );
    return ret;
}

static const char *get_self_exe(void)
{
#if defined(__linux__) || defined(__FreeBSD_kernel__) || defined(__NetBSD__)
    return "/proc/self/exe";
#elif defined (__FreeBSD__) || defined(__DragonFly__)
    static int pathname[] = { CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, -1 };
    size_t path_size = PATH_MAX;
    char *path = malloc( path_size );
    if (path && !sysctl( pathname, sizeof(pathname)/sizeof(pathname[0]), path, &path_size, NULL, 0 ))
        return path;
    free( path );
#elif defined(__APPLE__)
    uint32_t path_size = PATH_MAX;
    char *path = malloc( path_size );
    if (path && !_NSGetExecutablePath( path, &path_size ))
        return path;
    free( path );
#endif
    return NULL;
}

static void *try_dlopen( const char *argv0 )
{
    char *dir, *path, *p;
    void *handle;

    if (!argv0) return NULL;
    if (!(dir = realpath_dirname( argv0 ))) return NULL;

    if ((p = remove_tail( dir, "/loader" )))
        path = build_path( p, "dlls/ntdll/ntdll.so" );
#ifdef __APPLE__
    /* the loader can run from an app bundle next to ntdll.so, e.g. wine.app/Contents/MacOS/wine,
     * so that it can carry entitlements granted by a provisioning profile */
    else if ((p = remove_tail( dir, ".app/Contents/MacOS" )) && strrchr( p, '/' ))
    {
        *strrchr( p, '/' ) = 0;
        path = build_path( p, "ntdll.so" );
    }
#endif
    else
        path = build_path( dir, "ntdll.so" );

    handle = dlopen( path, RTLD_NOW );
    free( p );
    free( dir );
    free( path );
    return handle;
}


#if defined(__APPLE__) && defined(__aarch64__)

extern char **environ;

static const char exec_marker[] = "__WINE_4K_EXEC";
#define MIN_LOADER_GAP  0x2000000  /* see exec_with_4k_pages */
#define MAX_SLIDE_EXECS 8

/* exec the loader with 4K pages and exec_marker set to value; returns an error on failure */
static int exec_self_4k( char *argv[], const char *value, const char **call )
{
    posix_spawnattr_t attr;
    char *exe;
    int err;

    errno = 0;
    *call = "executable path";
    if (!(exe = (char *)get_self_exe()))
        return errno ? errno : ENAMETOOLONG;  /* _NSGetExecutablePath sets no errno */

    *call = "posix_spawnattr_init";
    if (!(err = posix_spawnattr_init( &attr )))
    {
        if ((err = posix_spawnattr_setflags( &attr, POSIX_SPAWN_SETEXEC )))
            *call = "posix_spawnattr_setflags";
        else if ((err = posix_spawnattr_set_4k_page_size_np( &attr )))
            *call = "posix_spawnattr_set_4k_page_size_np";
        else if (setenv( exec_marker, value, 1 ))
        {
            *call = "setenv";
            err = errno;
        }
        else
        {
            *call = "posix_spawn";
            err = posix_spawn( NULL, exe, NULL, &attr, argv, environ );
            unsetenv( exec_marker );
        }
        posix_spawnattr_destroy( &attr );
    }
    free( exe );
    return err;
}

/* Windows code expects 4K pages, and on 16K pages Wine has to merge four of them into one
 * host page. The kernel grants a process 4K pages at exec time when its binary has the
 * cross-architecture entitlement, so re-exec the loader with them, before anything is
 * mapped. A forked child keeps the page size; a plain exec drops it and comes back here.
 * ntdll starts new Wine processes with 4K pages when it has them, so for the page size this
 * exec only runs for a loader started from outside Wine, or through a plain exec such as a
 * Unix spawn. A binary without the entitlement would be killed in that exec, with no
 * message, so first check that the low 4GB can be mapped, which only the entitlement allows,
 * and exit with an error saying so otherwise.
 * With 4K pages, the loader also execs itself again, keeping them, for a new slide when the
 * kernel put it less than 32MB above 4GB, too close for most executables at 0x100000000 (see
 * init_reserved_areas). The slide is random up to about 80MB and falls into one of three
 * bands, the lowest of which ends at 16MB, so that happens for about one start in three.
 * __WINE_4K_EXEC marks a re-exec'd loader, so that it stops instead of exec'ing again if the
 * page size did not change, and counts the execs for a new slide, so that every loader tries
 * at most MAX_SLIDE_EXECS of them. It is removed at once, so no child inherits it. If an exec
 * for a new slide fails, the loader keeps the slide it has. */
static void exec_with_4k_pages( char *argv[] )
{
    const char *value = getenv( exec_marker ), *call, *reason = NULL;
    int err = 0, reexec = value != NULL, slide_execs = value ? atoi( value ) : 0;
    char buffer[16];

    unsetenv( exec_marker );
    if (sysconf( _SC_PAGESIZE ) == 0x1000)
    {
        if (slide_execs >= MAX_SLIDE_EXECS ||
            (char *)&_mh_execute_header - (char *)0x100000000 >= MIN_LOADER_GAP)
            return;
        snprintf( buffer, sizeof(buffer), "%d", slide_execs + 1 );
        err = exec_self_4k( argv, buffer, &call );
        fprintf( stderr, "wine: %s for a new loader slide failed: %s, keeping this one\n",
                 call, strerror( err ));
        return;
    }

    errno = 0;
    if (reexec)
    {
        call = "exec with 4K pages";
        reason = "the page size is still not 4K";
    }
    else if (mmap( low_reserve[0].addr, 0x4000, PROT_NONE,
                   MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0 ) != low_reserve[0].addr)
    {
        call = "mmap below 4GB";
        err = errno;
    }
    else
    {
        snprintf( buffer, sizeof(buffer), "%d", slide_execs );
        err = exec_self_4k( argv, buffer, &call );
    }

    fprintf( stderr, "wine: %s failed: %s\n", call, reason ? reason : strerror( err ));
    fprintf( stderr, "wine: the loader must run from the signed wine.app "
             "with the cross-architecture entitlement\n" );
    exit(1);
}

#endif

/**********************************************************************
 *           main
 */
int main( int argc, char *argv[] )
{
    void *handle;

#if defined(__APPLE__) && defined(__aarch64__)
    exec_with_4k_pages( argv );
#endif
    init_reserved_areas();

    if ((handle = try_dlopen( get_self_exe() )) ||
        (handle = try_dlopen( argv[0] )))
    {
        void (*init_func)(int, char **) = dlsym( handle, "__wine_main" );
        if (init_func) init_func( argc, argv );
        fprintf( stderr, "wine: __wine_main function not found in ntdll.so\n" );
        exit(1);
    }

    fprintf( stderr, "wine: could not load ntdll.so: %s\n", dlerror() );
    pthread_detach( pthread_self() );  /* force importing libpthread for OpenGL */
    exit(1);
}
