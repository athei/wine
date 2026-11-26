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
#include <CommonCrypto/CommonDigest.h>
#include <copyfile.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fts.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <removefile.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <time.h>
#include <unistd.h>

#include "named_loader.h"

#define DIR_FLAGS (O_DIRECTORY | O_RDONLY | O_NOFOLLOW | O_CLOEXEC)

static int join_path( char *out, const char *dir, const char *name )
{
    int n = snprintf( out, PATH_MAX, "%s/%s", dir, name );
    return n >= 0 && n < PATH_MAX;
}

/* Bundle names themselves aren't part of the signature. Hash the relative tree,
 * not its root name or inode. In particular include Info.plist, CodeResources,
 * the embedded profile and executable bytes, not just the Mach-O cdhash. */
#define HASH_LEN (CC_SHA256_DIGEST_LENGTH * 2)

static void hex_digest( CC_SHA256_CTX *ctx, char hex[HASH_LEN + 1] )
{
    static const char digits[] = "0123456789abcdef";
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    unsigned int i;
    CC_SHA256_Final( digest, ctx );
    for (i = 0; i < sizeof(digest); ++i)
    {
        hex[2*i] = digits[digest[i] >> 4];
        hex[2*i+1] = digits[digest[i] & 15];
    }
    hex[HASH_LEN] = 0;
}

static int compare_names( const FTSENT **a, const FTSENT **b )
{
    return strcmp( (*a)->fts_name, (*b)->fts_name );
}

static int bundle_digest( char *path, char hex[HASH_LEN + 1] )
{
    char *paths[] = { path, NULL }, buffer[16384];
    size_t prefix = strlen(path);
    CC_SHA256_CTX ctx;
    FTS *tree;
    FTSENT *ent;
    ssize_t n;
    int fd, ok = 0;

    if (!(tree = fts_open( paths, FTS_PHYSICAL | FTS_NOCHDIR, compare_names ))) return 0;
    CC_SHA256_Init( &ctx );
    while ((errno = 0, ent = fts_read( tree )))
    {
        mode_t mode;
        off_t size;
        const char *name = ent->fts_path + prefix;
        if (ent->fts_info == FTS_DP) continue;
        /* Mach9's signed loader contains only regular files and directories. */
        if (ent->fts_info != FTS_D && ent->fts_info != FTS_F) goto done;
        mode = ent->fts_statp->st_mode;
        CC_SHA256_Update( &ctx, name, strlen(name) + 1 );
        CC_SHA256_Update( &ctx, &mode, sizeof(mode) );
        /* Do not reuse a copy stripped of the source quarantine attribute.
         * Resource forks aren't part of a supported signed Mach9 bundle. */
        if (getxattr( ent->fts_path, XATTR_RESOURCEFORK_NAME, NULL, 0, 0, XATTR_NOFOLLOW ) != -1 ||
            errno != ENOATTR) goto done;
        n = getxattr( ent->fts_path, "com.apple.quarantine", buffer, sizeof(buffer), 0, XATTR_NOFOLLOW );
        if (n == -1 && errno != ENOATTR) goto done;
        /* copyfile may rewrite the informational download time and agent on
         * directories; compare the quarantine flags rather than that history. */
        if (ent->fts_info == FTS_D && n > 0)
        {
            char *end = memchr( buffer, ';', n );
            if (end) n = end - buffer;
        }
        CC_SHA256_Update( &ctx, &n, sizeof(n) );
        if (n > 0) CC_SHA256_Update( &ctx, buffer, n );
        if (ent->fts_info == FTS_D) continue;
        size = ent->fts_statp->st_size;
        CC_SHA256_Update( &ctx, &size, sizeof(size) );
        if ((fd = open( ent->fts_path, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC )) == -1) goto done;
        while ((n = read( fd, buffer, sizeof(buffer) )) > 0)
        {
            CC_SHA256_Update( &ctx, buffer, n );
            size -= n;
        }
        close( fd );
        if (n || size) goto done;
    }
    if (!errno)
    {
        hex_digest( &ctx, hex );
        ok = 1;
    }
 done:
    fts_close( tree );
    return ok;
}

/* Hold the root lock through verification and publication. Unchanged titles
 * keep a deterministic path, including after an idle entry is reclaimed. */
static void prune_entries( DIR *root, const char *root_path, const char *key )
{
    struct dirent *ent;
    char path[PATH_MAX];
    struct stat st;
    time_t now = time(NULL);
    int fd;

    while ((ent = readdir( root )))
    {
        if (ent->d_name[0] == '.' || !strcmp( ent->d_name, key ) || !join_path( path, root_path, ent->d_name )) continue;
        if ((fd = openat( dirfd( root ), ent->d_name, DIR_FLAGS | O_EXLOCK | O_NONBLOCK )) == -1) continue;
        if (!strncmp( ent->d_name, key, HASH_LEN + 1 ) ||
            (!fstat( fd, &st ) && difftime( now, st.st_mtime ) > 30 * 86400))
            removefile( path, NULL, REMOVEFILE_RECURSIVE );
        close( fd );
    }
}

static int valid_dependency( int fd, const char *expected )
{
    char target[PATH_MAX];
    struct stat st;
    size_t len = strlen(expected);
    return readlinkat( fd, "ntdll.so", target, sizeof(target) ) == (ssize_t)len &&
           !memcmp( target, expected, len ) && !fstatat( fd, "ntdll.so", &st, 0 ) && S_ISREG(st.st_mode);
}

/* macOS can display ':' in a POSIX filename as '/'. Keep the unmodified
 * display name on the lease directory, never inside the signed bundle. */
#define TITLE_ATTR "org.winehq.named-loader.title"

static int bundle_name( const char *title, char name[NAME_MAX + 1] )
{
    size_t i, n = 0;
    int nonblank = 0;

    for (i = 0; title[i]; ++i)
    {
        unsigned char c = title[i];
        if (i >= NAME_MAX || c == '/' || c < 32 || c == 127) return 0;
        if (c == ':') continue;
        if (n >= NAME_MAX - 4) return 0;
        if (c != ' ') nonblank = 1;
        name[n++] = c;
    }
    if (!nonblank || name[0] == '.') return 0;
    memcpy( name + n, ".app", 5 );
    return 1;
}

static int read_title( int fd, char title[NAME_MAX + 1] )
{
    ssize_t len = fgetxattr( fd, TITLE_ATTR, title, NAME_MAX, 0, 0 );
    if (len <= 0 || memchr( title, 0, len )) return 0;
    title[len] = 0;
    return 1;
}

char *prepare_named_loader( const char *parent, const char *ntdll_dir, const char *title, int *lease )
{
    char root_path[PATH_MAX], entry[PATH_MAX], bundle[PATH_MAX], exe[PATH_MAX];
    char src[PATH_MAX], ntdll[PATH_MAX], link[PATH_MAX], key[2*HASH_LEN + 2], digest[HASH_LEN + 1];
    char name[NAME_MAX + 1], saved_title[NAME_MAX + 1];
    char *ret = NULL;
    DIR *root;
    CC_SHA256_CTX ctx;
    struct stat st;
    int fd = -1, root_fd;

    *lease = -1;
    if (!bundle_name( title, name ) || !join_path( root_path, parent, "wine-named-loaders-v3" ) ||
        !join_path( ntdll, ntdll_dir, "ntdll.so" ) || !join_path( src, ntdll_dir, "wine.app" )) return NULL;
    CC_SHA256_Init( &ctx );
    /* Older entries derived the menu title from the filename and don't have
     * TITLE_ATTR. Don't invalidate a live lease when upgrading the format. */
    CC_SHA256_Update( &ctx, TITLE_ATTR, sizeof(TITLE_ATTR) );
    CC_SHA256_Update( &ctx, ntdll_dir, strlen(ntdll_dir) + 1 );
    CC_SHA256_Update( &ctx, title, strlen(title) + 1 );
    hex_digest( &ctx, key );
    key[HASH_LEN] = '-';
    if (!bundle_digest( src, key + HASH_LEN + 1 ) || !join_path( entry, root_path, key )) return NULL;
    if (!join_path( bundle, entry, name ) || !join_path( link, entry, "ntdll.so" ) ||
        !join_path( exe, bundle, "Contents/MacOS/wine" )) return NULL;
    if (mkdir( root_path, 0700 ) && errno != EEXIST) return NULL;
    if ((root_fd = open( root_path, DIR_FLAGS | O_EXLOCK )) == -1) return NULL;
    if (fstat( root_fd, &st ) || st.st_uid != geteuid() || (st.st_mode & 077) || !(root = fdopendir( root_fd )))
    {
        close( root_fd );
        return NULL;
    }
    prune_entries( root, root_path, key );
    if ((fd = open( entry, DIR_FLAGS | O_SHLOCK )) != -1)
    {
        if (read_title( fd, saved_title ) && !strcmp( title, saved_title ) &&
            valid_dependency( fd, ntdll ) && !access( exe, X_OK ) && bundle_digest( bundle, digest ) &&
            !strcmp( digest, key + HASH_LEN + 1 )) goto ready;
        /* Never mutate or repair a bundle leased by another process. */
        if (flock( fd, LOCK_EX | LOCK_NB ) == -1 || removefile( entry, NULL, REMOVEFILE_RECURSIVE )) goto done;
        close( fd );
        fd = -1;
    }
    if (mkdir( entry, 0700 ) || (fd = open( entry, DIR_FLAGS | O_SHLOCK )) == -1) goto done;
    if (fsetxattr( fd, TITLE_ATTR, title, strlen(title), 0, XATTR_CREATE ) || symlink( ntdll, link ) ||
        copyfile( src, bundle, NULL, COPYFILE_ALL | COPYFILE_RECURSIVE | COPYFILE_NOFOLLOW | COPYFILE_CLONE ) ||
        !valid_dependency( fd, ntdll ) || access( exe, X_OK ) || !bundle_digest( bundle, digest ) ||
        strcmp( digest, key + HASH_LEN + 1 ))
    {
        removefile( entry, NULL, REMOVEFILE_RECURSIVE );
        goto done;
    }
 ready:
    if (fd <= STDERR_FILENO)
    {
        int dup = fcntl( fd, F_DUPFD_CLOEXEC, STDERR_FILENO + 1 );
        close( fd );
        fd = dup;
    }
    if (fd != -1 && (ret = strdup( exe )))
    {
        futimens( fd, NULL ); /* Reclaim long-idle titles, not live leases. */
        *lease = fd;
        fd = -1;
    }
 done:
    if (fd != -1) close( fd );
    closedir( root );
    return ret;
}

/* The loader must find the original ntdll.so next to its .app. Check that
 * dependency before exec: after exec, the default-loader fallback is too late.
 * Validate relative to the actual leased directory, not another user's path. */
int inherit_named_loader( int lease, const char *exe, const char *ntdll_dir )
{
    char expected[PATH_MAX], value[32];
    struct stat st;

    if (lease == -1 || !join_path( expected, ntdll_dir, "ntdll.so" ) ||
        !valid_dependency( lease, expected ) ||
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
    char exe[PATH_MAX], expected[NAME_MAX + 1], *end, *name;
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
                !fcntl( fd, F_SETFD, FD_CLOEXEC ) && read_title( fd, title ) && bundle_name( title, expected ) &&
                strlen(name) + 4 == strlen(expected) && !strncmp( expected, name, strlen(name) )) ret = title;
        }
    }
    unsetenv( NAMED_LOADER_LEASE_ENV );
    return ret;
}
#endif
