/*
 * pk-alpm-aur.h — AUR integration for PackageKit ALPM backend
 *
 * Bridges AUR RPC + build pipeline with PackageKit's backend API.
 * Provides functions to be called from the existing pk_backend_* entry points
 * when AUR packages are involved.
 *
 * Package ID convention: name;version;arch;aur
 * The "aur" repo field distinguishes AUR packages from sync DB packages.
 */

#ifndef PK_ALPM_AUR_H
#define PK_ALPM_AUR_H

#include <glib.h>
#include "aur-rpc.h"
#include "aur-srcinfo.h"
#include "aur-builder.h"

/* Singleton-ish context — initialized once per backend lifetime */
typedef struct {
	AurRpc      *rpc;
	AurBuilder  *builder;
} PkAlpmAur;

/* Package info mapped to PackageKit conventions */
typedef struct {
	gchar    *package_id;     /* name;version;arch;aur */
	gchar    *name;
	gchar    *pkgbase;           /* PackageBase — may differ from name for split packages */
	gchar    *version;
	gchar    *arch;
	gchar    *description;
	gchar    *url;
	gchar    *maintainer;
	gchar    *license;        /* joined if multiple */
	guint64   size;           /* 0 for AUR (unknown until built) */
	guint     votes;
	gdouble   popularity;
	gboolean  out_of_date;
	gboolean  installed;      /* whether this package is currently installed */
	/* Dependency info (from info endpoint) */
	gchar   **depends;
	gchar   **makedepends;
	gchar   **optdepends;
	gchar   **conflicts;
	gchar   **provides;
} PkAlpmAurPkg;

typedef struct {
	PkAlpmAurPkg **packages;
	guint          count;
} PkAlpmAurResult;

/* Init/destroy */
PkAlpmAur       *pk_alpm_aur_new         (const gchar *clone_dir);
void             pk_alpm_aur_free        (PkAlpmAur *aur);

/* Search AUR — returns packages mapped to PK format.
 * arch: system architecture (e.g. "x86_64") for package IDs.
 * installed flag is FALSE; caller sets it via localdb. */
PkAlpmAurResult *pk_alpm_aur_search      (PkAlpmAur *aur,
					   const gchar *query,
					   const gchar *arch,
					   GError **error);

/* Get detailed info for specific AUR packages by name.
 * names: NULL-terminated array.
 * arch: system architecture (e.g. "x86_64") for package IDs.
 * installed flag is FALSE; caller sets it via localdb. */
PkAlpmAurResult *pk_alpm_aur_get_details (PkAlpmAur *aur,
					   const gchar * const *names,
					   const gchar *arch,
					   GError **error);

/* Check if a package ID refers to AUR */
gboolean         pk_alpm_aur_is_aur_id   (const gchar *package_id);

/* Extract package name from a PK package ID */
gchar           *pk_alpm_aur_id_name     (const gchar *package_id);

/* Build a PK-format package ID for an AUR package */
gchar           *pk_alpm_aur_build_id    (const gchar *name,
										  const gchar *version,
										  const gchar *arch);

/* Free results */
void             pk_alpm_aur_pkg_free    (PkAlpmAurPkg *pkg);
void             pk_alpm_aur_result_free (PkAlpmAurResult *result);

#endif /* PK_ALPM_AUR_H */
