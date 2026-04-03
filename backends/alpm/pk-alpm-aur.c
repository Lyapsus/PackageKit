/*
 * pk-alpm-aur.c — AUR integration for PackageKit ALPM backend
 *
 * Pure RPC + ID utility layer. No libalpm dependency, no process spawns.
 * Installed-state and update checks live in pk-alpm-aur-hooks.c which
 * has full libalpm access.
 */

#include "pk-alpm-aur.h"
#include <string.h>

#define AUR_REPO_FIELD "aur"

/* --- Helpers --- */

/*
 * Map an AurPackage (from RPC) to PkAlpmAurPkg.
 * arch: system architecture string (e.g. "x86_64"). Caller provides it.
 * installed flag is left FALSE — caller (hooks.c) sets it via localdb.
 */
static PkAlpmAurPkg *
aur_pkg_to_pk (AurPackage *src, const gchar *arch)
{
	PkAlpmAurPkg *pkg = g_new0 (PkAlpmAurPkg, 1);

	pkg->name        = g_strdup (src->name);
	pkg->pkgbase     = g_strdup (src->package_base);
	pkg->version     = g_strdup (src->version);
	pkg->arch        = g_strdup (arch);
	pkg->description = g_strdup (src->description);
	pkg->url         = g_strdup (src->url);
	pkg->maintainer  = g_strdup (src->maintainer);
	pkg->license     = src->license ? g_strjoinv (", ", src->license) : NULL;
	pkg->votes       = src->num_votes;
	pkg->popularity  = src->popularity;
	pkg->out_of_date = (src->out_of_date > 0);
	pkg->size        = 0; /* Unknown for AUR */

	/* Copy dependency arrays */
	pkg->depends     = g_strdupv (src->depends);
	pkg->makedepends = g_strdupv (src->make_depends);
	pkg->optdepends  = g_strdupv (src->opt_depends);
	pkg->conflicts   = g_strdupv (src->conflicts);
	pkg->provides    = g_strdupv (src->provides);

	/* Build PK package ID */
	pkg->package_id = pk_alpm_aur_build_id (pkg->name, pkg->version, pkg->arch);

	pkg->installed = FALSE;

	return pkg;
}

/* --- Public API --- */

PkAlpmAur *
pk_alpm_aur_new (const gchar *clone_dir)
{
	PkAlpmAur *aur;
	const gchar *rpc_url;
	const gchar *env_clone;

	aur = g_new0 (PkAlpmAur, 1);
	rpc_url = g_getenv ("AUR_RPC_URL");
	aur->rpc = aur_rpc_new (rpc_url);
	env_clone = g_getenv ("AUR_CLONE_DIR");
	aur->builder = aur_builder_new (env_clone ? env_clone : clone_dir);
	return aur;
}

void
pk_alpm_aur_free (PkAlpmAur *aur)
{
	if (aur == NULL) return;
	aur_rpc_free (aur->rpc);
	aur_builder_free (aur->builder);
	g_free (aur);
}

PkAlpmAurResult *
pk_alpm_aur_search (PkAlpmAur *aur, const gchar *query,
		    const gchar *arch, GError **error)
{
	AurResult *rpc_result;
	PkAlpmAurResult *result;
	guint i;

	g_return_val_if_fail (aur != NULL && query != NULL, NULL);

	rpc_result = aur_rpc_search (aur->rpc, query,
								 AUR_SEARCH_BY_NAME_DESC, error);
	if (rpc_result == NULL)
		return NULL;

	result = g_new0 (PkAlpmAurResult, 1);
	result->count = rpc_result->count;
	result->packages = g_new0 (PkAlpmAurPkg *, result->count + 1);

	for (i = 0; i < rpc_result->count; i++)
		result->packages[i] = aur_pkg_to_pk (rpc_result->packages[i], arch);

	aur_result_free (rpc_result);
	return result;
}

PkAlpmAurResult *
pk_alpm_aur_get_details (PkAlpmAur *aur, const gchar * const *names,
			 const gchar *arch, GError **error)
{
	AurResult *rpc_result;
	PkAlpmAurResult *result;
	guint i;

	g_return_val_if_fail (aur != NULL && names != NULL, NULL);

	rpc_result = aur_rpc_info (aur->rpc, names, error);
	if (rpc_result == NULL)
		return NULL;

	result = g_new0 (PkAlpmAurResult, 1);
	result->count = rpc_result->count;
	result->packages = g_new0 (PkAlpmAurPkg *, result->count + 1);

	for (i = 0; i < rpc_result->count; i++)
		result->packages[i] = aur_pkg_to_pk (rpc_result->packages[i], arch);

	aur_result_free (rpc_result);
	return result;
}

gboolean
pk_alpm_aur_is_aur_id (const gchar *package_id)
{
	const gchar *last_semi;

	if (package_id == NULL)
		return FALSE;
	/* Package ID format: name;version;arch;repo — check if repo is "aur" */
	last_semi = strrchr (package_id, ';');
	if (last_semi == NULL)
		return FALSE;
	return g_strcmp0 (last_semi + 1, AUR_REPO_FIELD) == 0;
}

gchar *
pk_alpm_aur_id_name (const gchar *package_id)
{
	const gchar *semi;

	if (package_id == NULL)
		return NULL;
	semi = strchr (package_id, ';');
	if (semi == NULL)
		return g_strdup (package_id);
	return g_strndup (package_id, (gsize)(semi - package_id));
}

gchar *
pk_alpm_aur_build_id (const gchar *name, const gchar *version, const gchar *arch)
{
	return g_strdup_printf ("%s;%s;%s;%s", name, version,
				arch ? arch : "any", AUR_REPO_FIELD);
}

void
pk_alpm_aur_pkg_free (PkAlpmAurPkg *pkg)
{
	if (pkg == NULL) return;
	g_free (pkg->package_id);
	g_free (pkg->name);
	g_free (pkg->pkgbase);
	g_free (pkg->version);
	g_free (pkg->arch);
	g_free (pkg->description);
	g_free (pkg->url);
	g_free (pkg->maintainer);
	g_free (pkg->license);
	g_strfreev (pkg->depends);
	g_strfreev (pkg->makedepends);
	g_strfreev (pkg->optdepends);
	g_strfreev (pkg->conflicts);
	g_strfreev (pkg->provides);
	g_free (pkg);
}

void
pk_alpm_aur_result_free (PkAlpmAurResult *result)
{
	guint i;

	if (result == NULL) return;
	for (i = 0; i < result->count; i++)
		pk_alpm_aur_pkg_free (result->packages[i]);
	g_free (result->packages);
	g_free (result);
}
