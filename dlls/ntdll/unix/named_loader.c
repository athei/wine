/*
 * Per-process named macOS loader bundles.
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

#if defined(__APPLE__) && defined(__aarch64__)
#include <copyfile.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <removefile.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "named_loader.h"

#define DIR_FLAGS (O_DIRECTORY | O_RDONLY | O_NOFOLLOW | O_CLOEXEC)

static int join_path( char *out, const char *dir, const char *name )
{
    int n = snprintf( out, PATH_MAX, "%s/%s", dir, name );
    return n >= 0 && n < PATH_MAX;
}

/* Under the root lock: nobody can publish a new entry between our lease
 * check and removal. A killed writer and a crashed game release their locks
 * just like a normal exit. Never open a prior entry to launch another game. */
static void prune_entries( DIR *root, const char *root_path )
{
    struct dirent *ent;
    char path[PATH_MAX];
    int fd;

    while ((ent = readdir( root )))
    {
        if (strncmp( ent->d_name, "run.", 4 ) || !join_path( path, root_path, ent->d_name )) continue;
        if ((fd = openat( dirfd( root ), ent->d_name, DIR_FLAGS | O_EXLOCK | O_NONBLOCK )) == -1) continue;
        removefile( path, NULL, REMOVEFILE_RECURSIVE );
        close( fd );
    }
}

/* A fresh snapshot includes the current profile and all sealed resources,
 * even if they changed without replacing Contents/MacOS/wine. All children
 * share the root; only allocating/reclaiming the private lease directories
 * is serialized. Copy the actual contents outside the lock. */
char *prepare_named_loader( const char *parent, const char *ntdll_dir, const char *title, int *lease )
{
    char root_path[PATH_MAX], entry[PATH_MAX], bundle[PATH_MAX], exe[PATH_MAX];
    char src[PATH_MAX], ntdll[PATH_MAX], link[PATH_MAX];
    char *ret = NULL;
    DIR *root;
    struct stat st;
    int fd, root_fd, n;

    *lease = -1;
    if (!join_path( root_path, parent, "wine-named-loaders-v2" ) ||
        !join_path( entry, root_path, "run.XXXXXX" ) ||
        !join_path( ntdll, ntdll_dir, "ntdll.so" ) || !join_path( src, ntdll_dir, "wine.app" )) return NULL;
    if (mkdir( root_path, 0700 ) && errno != EEXIST) return NULL;
    if ((root_fd = open( root_path, DIR_FLAGS | O_EXLOCK )) == -1) return NULL;
    if (fstat( root_fd, &st ) || st.st_uid != geteuid() || (st.st_mode & 077) || !(root = fdopendir( root_fd )))
    {
        close( root_fd );
        return NULL;
    }
    prune_entries( root, root_path );
    if (!mkdtemp( entry ))
    {
        closedir( root );
        return NULL;
    }
    fd = open( entry, DIR_FLAGS | O_EXLOCK );
    /* The grandchild may replace all three standard descriptors before exec. */
    if (fd >= 0 && fd <= STDERR_FILENO)
    {
        int dup = fcntl( fd, F_DUPFD_CLOEXEC, STDERR_FILENO + 1 );
        close( fd );
        fd = dup;
    }
    if (fd == -1) rmdir( entry );
    closedir( root );
    if (fd == -1) return NULL;

    n = snprintf( bundle, sizeof(bundle), "%s/%s.app", entry, title );
    if (n > 0 && n < (int)sizeof(bundle) && join_path( link, entry, "ntdll.so" ) &&
        join_path( exe, bundle, "Contents/MacOS/wine" ) && !symlink( ntdll, link ) &&
        !copyfile( src, bundle, NULL, COPYFILE_ALL | COPYFILE_RECURSIVE | COPYFILE_NOFOLLOW | COPYFILE_CLONE ) &&
        (ret = strdup( exe )))
        *lease = fd;
    else
    {
        removefile( entry, NULL, REMOVEFILE_RECURSIVE );
        close( fd );
    }
    return ret;
}

/* The loader must find the original ntdll.so next to its .app. Check that
 * dependency before exec: after exec, the default-loader fallback is too late.
 * Validate relative to the actual leased directory, not another user's path. */
int inherit_named_loader( int lease, const char *exe, const char *ntdll_dir )
{
    char link[PATH_MAX], expected[PATH_MAX], value[32];
    struct stat st;
    ssize_t len;

    if (lease == -1 || !join_path( expected, ntdll_dir, "ntdll.so" ) ||
        (len = readlinkat( lease, "ntdll.so", link, sizeof(link) )) != (ssize_t)strlen( expected ) ||
        memcmp( link, expected, len ) || fstatat( lease, "ntdll.so", &st, 0 ) || !S_ISREG( st.st_mode ) ||
        lstat( exe, &st ) || !S_ISREG( st.st_mode ) || access( exe, X_OK )) return 0;
    snprintf( value, sizeof(value), "%d", lease );
    if (fcntl( lease, F_SETFD, 0 ) == -1) return 0;
    if (!setenv( NAMED_LOADER_LEASE_ENV, value, 1 )) return 1;
    fcntl( lease, F_SETFD, FD_CLOEXEC );
    return 0;
}

/* Keep the inherited lease for this process, but not its children. Only
 * alter an fd if it names the directory around our actual running bundle.
 * The kernel closes it on exit, including abrupt Windows termination. */
const char *adopt_named_loader(void)
{
    static const char suffix[] = ".app/Contents/MacOS/wine";
    static char title[NAME_MAX + 1];
    char exe[PATH_MAX], *end, *name;
    const char *ret = NULL;
    uint32_t size = sizeof(exe);
    const char *value = getenv( NAMED_LOADER_LEASE_ENV );
    struct stat fd_st, path_st;
    long fd;
    size_t len;

    if (!value) return NULL;
    fd = strtol( value, &end, 10 );
    if (end != value && !*end && fd > STDERR_FILENO && fd <= INT_MAX && !_NSGetExecutablePath( exe, &size ) &&
        (len = strlen( exe )) >= sizeof(suffix) && !strcmp( exe + len - (sizeof(suffix) - 1), suffix ))
    {
        exe[len - (sizeof(suffix) - 1)] = 0;
        if ((name = strrchr( exe, '/' )))
        {
            *name++ = 0;
            if (*name && strlen( name ) < sizeof(title) && !stat( exe, &path_st ) && !fstat( fd, &fd_st ) &&
                S_ISDIR( fd_st.st_mode ) && fd_st.st_ino == path_st.st_ino && fd_st.st_dev == path_st.st_dev &&
                !fcntl( fd, F_SETFD, FD_CLOEXEC )) ret = strcpy( title, name );
        }
    }
    unsetenv( NAMED_LOADER_LEASE_ENV );
    return ret;
}
#endif
