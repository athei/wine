/* Private interface to the macOS per-process loader bundle. */
#ifndef __NTDLL_NAMED_LOADER_H
#define __NTDLL_NAMED_LOADER_H

#define NAMED_LOADER_LEASE_ENV "__WINE_NAMED_LOADER_LEASE"

char *prepare_named_loader( const char *parent, const char *ntdll_dir, const char *title, int *lease );
int inherit_named_loader( int lease, const char *path, const char *ntdll_dir );
/* Return the original display name, not the filesystem spelling. */
const char *adopt_named_loader(void);

#endif
