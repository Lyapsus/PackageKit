/*
 * pk-alpm-aur-hooks.h — AUR hook declarations for ALPM backend
 *
 * Compiles only within the PK build tree.
 */

#ifndef PK_ALPM_AUR_HOOKS_H
#define PK_ALPM_AUR_HOOKS_H

#include <glib.h>
#include <pk-backend.h>

#include "pk-alpm-aur.h"

/* Lifecycle — stores PkAlpmAur* in PkBackendAlpmPrivate.aur_ctx */
void     pk_alpm_aur_init            (PkBackend *backend);
void     pk_alpm_aur_destroy         (PkBackend *backend);

/* Hook 1a: Search — append AUR results after syncdb search */
void     pk_alpm_aur_hook_search     (PkBackendJob *job,
									  const gchar * const *needles,
									  GError **error);

/* Hook 1b: Resolve — find AUR package by exact name */
gboolean pk_alpm_aur_hook_resolve    (PkBackendJob *job,
									  const gchar *name,
									  GError **error);

/* Hook 2: Get details for an AUR package ID */
void     pk_alpm_aur_hook_get_details (PkBackendJob *job,
									   const gchar *package_id,
									   GError **error);

/* Hook 3: Install single AUR package (clone + build + install via libalpm) */
gboolean pk_alpm_aur_hook_install    (PkBackendJob *job,
									  const gchar *package_id,
									  GError **error);

/* Hook 3b: Install all AUR packages from a package ID list */
void     pk_alpm_aur_hook_install_packages (PkBackendJob *job,
											const gchar **package_ids,
											GError **error);

/* Hook 4: Append AUR updates */
void     pk_alpm_aur_hook_get_updates (PkBackendJob *job,
									   GError **error);

#endif /* PK_ALPM_AUR_HOOKS_H */
