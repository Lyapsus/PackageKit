/*
 * pk-alpm-aur-hooks.c — AUR hook implementations for ALPM backend
 *
 * Compiles only within the PK build tree (needs pk-backend.h, libalpm).
 * Tested via container build (test-container).
 */

#include <string.h>
#include <syslog.h>
#include <alpm.h>
#include <pk-backend.h>

#include "pk-backend-alpm.h"
#include "pk-alpm-error.h"
#include "pk-alpm-transaction.h"
#include "pk-alpm-aur-hooks.h"

/* --- Helpers for dependency resolution --- */

/*
 * Extract bare package name from a dependency string via libalpm parser.
 * "foo>=1.0" -> "foo", "bar" -> "bar". Caller frees.
 */
static gchar *
dep_bare_name (const gchar *dep)
{
	alpm_depend_t *parsed;
	gchar *result;

	parsed = alpm_dep_from_string (dep);
	if (parsed == NULL)
		return g_strdup (dep);
	result = g_strdup (parsed->name);
	alpm_dep_free (parsed);
	return result;
}

/*
 * Check if a dependency can be satisfied from sync DBs or local DB.
 */
static gboolean
dep_available (alpm_handle_t *alpm, alpm_db_t *localdb, const gchar *depstring)
{
	alpm_pkg_t *pkg;
	alpm_list_t *syncdbs;

	syncdbs = alpm_get_syncdbs (alpm);
	pkg = alpm_find_dbs_satisfier (alpm, syncdbs, depstring);
	if (pkg != NULL)
		return TRUE;

	/* Also check if already installed locally (respects version constraints + Provides) */
	return alpm_find_satisfier (alpm_db_get_pkgcache (localdb), depstring) != NULL;
}

/*
 * Check if bare package name appears in a dependency string array.
 */
static gboolean
name_in_deparray (const gchar *name, gchar **deps)
{
	guint i;

	if (deps == NULL)
		return FALSE;
	for (i = 0; deps[i] != NULL; i++) {
		gchar *bare;
		gboolean match;

		bare = dep_bare_name (deps[i]);
		match = (g_strcmp0 (name, bare) == 0);
		g_free (bare);
		if (match)
			return TRUE;
	}
	return FALSE;
}

/* Get primary architecture from alpm handle. Returns static string, do not free. */
static const gchar *
get_system_arch (alpm_handle_t *alpm)
{
	alpm_list_t *arches;

	arches = alpm_option_get_architectures (alpm);
	if (arches != NULL && arches->data != NULL)
		return (const gchar *) arches->data;
	return "x86_64";
}

/*
 * Set installed flag on AUR result packages by checking localdb.
 */
static void
mark_installed (alpm_db_t *localdb, PkAlpmAurResult *result)
{
	guint i;

	if (result == NULL)
		return;
	for (i = 0; i < result->count; i++) {
		PkAlpmAurPkg *pkg = result->packages[i];
		pkg->installed = (alpm_db_get_pkg (localdb, pkg->name) != NULL);
	}
}

/* Cancel callback for aur_builder — wraps pk_backend_job_is_cancelled */
static gboolean
check_job_cancelled (gpointer user_data)
{
	return pk_backend_job_is_cancelled ((PkBackendJob *) user_data);
}

/*
 * BFS phase: discover all AUR dependencies starting from initial_names.
 *
 * Populates dep_graph (name -> GPtrArray of AUR dep names) and
 * name_to_pkgid (name -> package_id) for all discovered AUR packages.
 *
 * Returns TRUE on success, FALSE on RPC failure (caller should fall back
 * to original order). On failure, dep_graph may be partially populated.
 */
static gboolean
discover_aur_deps (PkAlpmAur *aur_ctx,
				   alpm_handle_t *alpm,
				   alpm_db_t *localdb,
				   GPtrArray *initial_names,
				   GHashTable *name_to_pkgid,
				   GHashTable *name_to_pkgbase,
				   GHashTable *dep_graph,
				   GError **error)
{
	GHashTable *queued;
	GQueue *to_resolve;
	guint i;

	queued = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
	to_resolve = g_queue_new ();

	for (i = 0; i < initial_names->len; i++) {
		const gchar *n = g_ptr_array_index (initial_names, i);
		g_queue_push_tail (to_resolve, g_strdup (n));
		g_hash_table_add (queued, g_strdup (n));
	}

	while (!g_queue_is_empty (to_resolve)) {
		GPtrArray *batch;
		AurResult *rpc_result;
		guint j;

		batch = g_ptr_array_new_with_free_func (g_free);
		while (!g_queue_is_empty (to_resolve))
			g_ptr_array_add (batch, g_queue_pop_head (to_resolve));
		g_ptr_array_add (batch, NULL);

		rpc_result = aur_rpc_info (aur_ctx->rpc,
								   (const gchar *const *) batch->pdata,
								   error);
		g_ptr_array_unref (batch);

		if (rpc_result == NULL) {
			g_queue_free_full (to_resolve, g_free);
			g_hash_table_unref (queued);
			return FALSE;
		}

		for (j = 0; j < rpc_result->count; j++) {
			AurPackage *apkg = rpc_result->packages[j];
			GPtrArray *aur_deps;
			gchar **dep_arrays[2];
			guint a;

			aur_deps = g_ptr_array_new_with_free_func (g_free);

			/* Record package_id for discovered deps */
			if (!g_hash_table_contains (name_to_pkgid, apkg->name)) {
				gchar *pid;
				pid = pk_alpm_aur_build_id (apkg->name,
											apkg->version,
											get_system_arch (alpm));
				g_hash_table_insert (name_to_pkgid,
									 g_strdup (apkg->name), pid);
			}

			/* Record pkgbase for discovered deps */
			if (!g_hash_table_contains (name_to_pkgbase, apkg->name)) {
				g_hash_table_insert (name_to_pkgbase,
									 g_strdup (apkg->name),
									 g_strdup (apkg->package_base ?
											   apkg->package_base :
											   apkg->name));
			}

			dep_arrays[0] = apkg->depends;
			dep_arrays[1] = apkg->make_depends;

			for (a = 0; a < 2; a++) {
				guint d;
				if (dep_arrays[a] == NULL)
					continue;
				for (d = 0; dep_arrays[a][d] != NULL; d++) {
					gchar *bare;

					if (dep_available (alpm, localdb,
									   dep_arrays[a][d]))
						continue;

					bare = dep_bare_name (dep_arrays[a][d]);
					g_ptr_array_add (aur_deps, g_strdup (bare));

					if (!g_hash_table_contains (queued, bare)) {
						g_hash_table_add (queued,
										  g_strdup (bare));
						g_queue_push_tail (to_resolve,
										   g_strdup (bare));
					}
					g_free (bare);
				}
			}

			g_hash_table_insert (dep_graph,
								 g_strdup (apkg->name), aur_deps);
		}

		aur_result_free (rpc_result);
	}

	g_queue_free (to_resolve);
	g_hash_table_unref (queued);
	return TRUE;
}

/*
 * Topological sort via Kahn's algorithm (deps first).
 *
 * dep_graph: name -> GPtrArray of AUR dep names
 * name_to_pkgid: name -> package_id
 * initial_names: originally requested package names (for cycle fallback)
 *
 * Returns a GPtrArray of package_id strings in build order.
 * Caller owns the result.
 */
static GPtrArray *
toposort_build_order (GHashTable *dep_graph,
					  GHashTable *name_to_pkgid,
					  GPtrArray *initial_names)
{
	GPtrArray *build_order;
	GHashTable *in_degree;
	GHashTable *reverse_deps;
	GQueue *ready;
	GHashTableIter iter;
	gpointer key, value;
	guint i;

	build_order = g_ptr_array_new_with_free_func (g_free);
	in_degree = g_hash_table_new_full (g_str_hash, g_str_equal,
										g_free, NULL);
	reverse_deps = g_hash_table_new_full (g_str_hash, g_str_equal,
										   g_free,
										   (GDestroyNotify) g_ptr_array_unref);

	/* Initialize in-degree to 0 for all confirmed AUR packages */
	g_hash_table_iter_init (&iter, dep_graph);
	while (g_hash_table_iter_next (&iter, &key, NULL))
		g_hash_table_insert (in_degree, g_strdup (key),
							 GUINT_TO_POINTER (0));

	/* Compute in-degrees and reverse map */
	g_hash_table_iter_init (&iter, dep_graph);
	while (g_hash_table_iter_next (&iter, &key, &value)) {
		GPtrArray *deps = value;
		guint d, deg;

		deg = 0;
		for (d = 0; d < deps->len; d++) {
			const gchar *dn = g_ptr_array_index (deps, d);
			GPtrArray *rdeps;

			/* Only count deps that are in our AUR set */
			if (!g_hash_table_contains (in_degree, dn))
				continue;

			deg++;

			rdeps = g_hash_table_lookup (reverse_deps, dn);
			if (rdeps == NULL) {
				rdeps = g_ptr_array_new_with_free_func (g_free);
				g_hash_table_insert (reverse_deps,
									 g_strdup (dn), rdeps);
			}
			g_ptr_array_add (rdeps, g_strdup (key));
		}
		/* in_degree[key] = number of AUR deps */
		g_hash_table_insert (in_degree, g_strdup (key),
							 GUINT_TO_POINTER (deg));
	}

	/* Start with nodes that have no AUR deps */
	ready = g_queue_new ();
	g_hash_table_iter_init (&iter, in_degree);
	while (g_hash_table_iter_next (&iter, &key, &value)) {
		if (GPOINTER_TO_UINT (value) == 0)
			g_queue_push_tail (ready, g_strdup (key));
	}

	while (!g_queue_is_empty (ready)) {
		gchar *node = g_queue_pop_head (ready);
		GPtrArray *rdeps;
		const gchar *pid;

		pid = g_hash_table_lookup (name_to_pkgid, node);
		if (pid != NULL)
			g_ptr_array_add (build_order, g_strdup (pid));

		rdeps = g_hash_table_lookup (reverse_deps, node);
		if (rdeps != NULL) {
			guint d;
			for (d = 0; d < rdeps->len; d++) {
				const gchar *dn =
					g_ptr_array_index (rdeps, d);
				guint deg;

				deg = GPOINTER_TO_UINT (
					g_hash_table_lookup (in_degree,
										 dn));
				if (deg > 0) {
					deg--;
					g_hash_table_insert (
						in_degree,
						g_strdup (dn),
						GUINT_TO_POINTER (deg));
					if (deg == 0)
						g_queue_push_tail (
							ready,
							g_strdup (dn));
				}
			}
		}

		g_free (node);
	}

	/* Check for cycles */
	if (build_order->len < g_hash_table_size (in_degree)) {
		syslog (LOG_DAEMON | LOG_WARNING,
			"AUR: circular dependency detected, "
			"using original order");
		g_ptr_array_set_size (build_order, 0);
		for (i = 0; i < initial_names->len; i++) {
			const gchar *n =
				g_ptr_array_index (initial_names, i);
			const gchar *pid =
				g_hash_table_lookup (name_to_pkgid, n);
			if (pid != NULL)
				g_ptr_array_add (build_order,
								 g_strdup (pid));
		}
	}

	g_queue_free (ready);
	g_hash_table_unref (in_degree);
	g_hash_table_unref (reverse_deps);
	return build_order;
}

/*
 * Resolve AUR dependency tree and return build order.
 *
 * For each AUR package, fetches deps via RPC, checks which deps are
 * not in sync DBs or local DB, queries AUR for those, recurses.
 * Returns NULL-terminated array of package_ids in topological order
 * (dependencies first). Returns NULL if no AUR packages found.
 * On RPC failure, falls back to original package order.
 * Caller frees with g_strfreev().
 */
static gchar **
resolve_aur_build_order (PkBackendJob *job,
						 const gchar **package_ids,
						 GHashTable **out_pkgbase_map,
						 GError **error)
{
	PkBackend *backend;
	PkBackendAlpmPrivate *priv;
	GPtrArray *initial_names;
	GHashTable *name_to_pkgid; /* name -> package_id */
	GHashTable *name_to_pkgbase; /* name -> pkgbase */
	GHashTable *dep_graph;     /* name -> GPtrArray of AUR dep names */
	GPtrArray *build_order;
	gchar **result;
	const gchar **p;
	guint i;

	backend = pk_backend_job_get_backend (job);
	priv = pk_backend_get_user_data (backend);
	initial_names = g_ptr_array_new_with_free_func (g_free);
	name_to_pkgid = g_hash_table_new_full (g_str_hash, g_str_equal,
											g_free, g_free);
	name_to_pkgbase = g_hash_table_new_full (g_str_hash, g_str_equal,
											  g_free, g_free);

	if (out_pkgbase_map != NULL)
		*out_pkgbase_map = NULL;

	/* Collect AUR package names from the request */
	for (p = package_ids; *p != NULL; ++p) {
		gchar *name;

		if (!pk_alpm_aur_is_aur_id (*p))
			continue;
		name = pk_alpm_aur_id_name (*p);
		g_hash_table_insert (name_to_pkgid, g_strdup (name),
							 g_strdup (*p));
		g_ptr_array_add (initial_names, name);
	}

	if (initial_names->len == 0) {
		g_ptr_array_unref (initial_names);
		g_hash_table_unref (name_to_pkgid);
		g_hash_table_unref (name_to_pkgbase);
		return NULL;
	}

	dep_graph = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
										(GDestroyNotify) g_ptr_array_unref);

	if (!discover_aur_deps ((PkAlpmAur *) priv->aur_ctx,
							priv->alpm, priv->localdb,
							initial_names, name_to_pkgid,
							name_to_pkgbase, dep_graph, error)) {
		/* RPC failed — fall back to original order */
		g_hash_table_unref (dep_graph);
		g_hash_table_unref (name_to_pkgbase);
		g_clear_error (error);
		result = g_new0 (gchar *, initial_names->len + 1);
		for (i = 0; i < initial_names->len; i++) {
			const gchar *n = g_ptr_array_index (initial_names, i);
			result[i] = g_strdup (
				g_hash_table_lookup (name_to_pkgid, n));
		}
		g_ptr_array_unref (initial_names);
		g_hash_table_unref (name_to_pkgid);
		return result;
	}

	/* Check that all initially requested packages were found in AUR */
	for (i = 0; i < initial_names->len; i++) {
		const gchar *n = g_ptr_array_index (initial_names, i);
		if (!g_hash_table_contains (dep_graph, n)) {
			g_set_error (error, PK_ALPM_ERROR, PK_ERROR_ENUM_PACKAGE_NOT_FOUND,
						 "AUR package not found: %s", n);
			g_hash_table_unref (dep_graph);
			g_ptr_array_unref (initial_names);
			g_hash_table_unref (name_to_pkgid);
			g_hash_table_unref (name_to_pkgbase);
			return NULL;
		}
	}

	build_order = toposort_build_order (dep_graph, name_to_pkgid,
										initial_names);

	g_hash_table_unref (dep_graph);
	g_ptr_array_unref (initial_names);
	g_hash_table_unref (name_to_pkgid);

	if (build_order->len == 0) {
		g_ptr_array_unref (build_order);
		g_hash_table_unref (name_to_pkgbase);
		return NULL;
	}

	/* Transfer pkgbase map to caller */
	if (out_pkgbase_map != NULL)
		*out_pkgbase_map = name_to_pkgbase;
	else
		g_hash_table_unref (name_to_pkgbase);

	g_ptr_array_add (build_order, NULL);
	return (gchar **) g_ptr_array_free (build_order, FALSE);
}

void
pk_alpm_aur_init (PkBackend *backend)
{
	PkBackendAlpmPrivate *priv = pk_backend_get_user_data (backend);

	if (priv->aur_ctx == NULL)
		priv->aur_ctx = pk_alpm_aur_new (NULL);
}

void
pk_alpm_aur_destroy (PkBackend *backend)
{
	PkBackendAlpmPrivate *priv = pk_backend_get_user_data (backend);

	if (priv->aur_ctx != NULL) {
		pk_alpm_aur_free ((PkAlpmAur *) priv->aur_ctx);
		priv->aur_ctx = NULL;
	}
}

/*
 * Hook 1: Search — emit AUR results as PK_INFO_ENUM_AVAILABLE
 */
void
pk_alpm_aur_hook_search (PkBackendJob *job,
						 const gchar * const *needles,
						 GError **error)
{
	PkBackend *backend;
	PkBackendAlpmPrivate *priv;
	PkAlpmAur *aur_ctx;
	guint i;
	PkAlpmAurResult *result;
	const gchar *query;

	backend = pk_backend_job_get_backend (job);
	priv = pk_backend_get_user_data (backend);
	aur_ctx = (PkAlpmAur *) priv->aur_ctx;

	if (aur_ctx == NULL || needles == NULL || needles[0] == NULL)
		return;

	/* Use first search term (PK sends array but AUR RPC takes single query) */
	query = needles[0];
	if (query[0] == '\0')
		return;

	/* AUR RPC requires >= 2 chars */
	if (strlen (query) < 2)
		return;

	pk_backend_job_set_status (job, PK_STATUS_ENUM_QUERY);

	result = pk_alpm_aur_search (aur_ctx, query,
				     get_system_arch (priv->alpm), error);
	if (result == NULL)
		return;

	mark_installed (priv->localdb, result);

	for (i = 0; i < result->count; i++) {
		PkAlpmAurPkg *pkg = result->packages[i];
		PkInfoEnum info;

		if (pk_backend_job_is_cancelled (job))
			break;

		info = pkg->installed ? PK_INFO_ENUM_INSTALLED : PK_INFO_ENUM_AVAILABLE;
		pk_backend_job_package (job, info, pkg->package_id,
								pkg->description ? pkg->description : "");
	}

	pk_alpm_aur_result_free (result);
}

/*
 * Hook 1b: Resolve — find AUR package by exact name
 */
gboolean
pk_alpm_aur_hook_resolve (PkBackendJob *job,
						  const gchar *name,
						  GError **error)
{
	PkBackend *backend;
	PkBackendAlpmPrivate *priv;
	PkAlpmAur *aur_ctx;
	const gchar *names[2];
	PkAlpmAurResult *result;

	backend = pk_backend_job_get_backend (job);
	priv = pk_backend_get_user_data (backend);
	aur_ctx = (PkAlpmAur *) priv->aur_ctx;

	if (aur_ctx == NULL)
		return FALSE;

	names[0] = name;
	names[1] = NULL;

	result = pk_alpm_aur_get_details (aur_ctx, names,
					  get_system_arch (priv->alpm), error);
	if (result == NULL)
		return FALSE;

	if (result->count == 0) {
		pk_alpm_aur_result_free (result);
		return FALSE;
	}

	pk_backend_job_package (job, PK_INFO_ENUM_AVAILABLE,
							result->packages[0]->package_id,
							result->packages[0]->description ? result->packages[0]->description : "");

	pk_alpm_aur_result_free (result);
	return TRUE;
}

/*
 * Hook 2: Get details for an AUR package
 */
void
pk_alpm_aur_hook_get_details (PkBackendJob *job,
							  const gchar *package_id,
							  GError **error)
{
	PkBackend *backend;
	PkBackendAlpmPrivate *priv;
	PkAlpmAur *aur_ctx;
	gchar *name;
	const gchar *names[2];
	PkAlpmAurResult *result;
	PkAlpmAurPkg *pkg;

	backend = pk_backend_job_get_backend (job);
	priv = pk_backend_get_user_data (backend);
	aur_ctx = (PkAlpmAur *) priv->aur_ctx;

	if (aur_ctx == NULL)
		return;

	name = pk_alpm_aur_id_name (package_id);
	names[0] = name;
	names[1] = NULL;

	result = pk_alpm_aur_get_details (aur_ctx, names,
					  get_system_arch (priv->alpm), error);
	g_free (name);

	if (result == NULL)
		return;

	if (result->count == 0) {
		pk_alpm_aur_result_free (result);
		return;
	}

	pkg = result->packages[0];

	pk_backend_job_details (job, package_id,
							NULL, /* summary */
							pkg->license ? pkg->license : "Unknown",
							PK_GROUP_ENUM_OTHER,
							pkg->description ? pkg->description : "",
							pkg->url ? pkg->url : "",
							0, /* installed size unknown */
							0  /* download size unknown */);

	pk_alpm_aur_result_free (result);
}

/*
 * Hook 3: Install an AUR package
 *
 * Flow: clone -> build (as user) -> install .pkg.tar.zst (as root via libalpm)
 * PKGBUILD review: skipped (same as paru with SkipReview)
 *
 * pkgbase_hint: if non-NULL, skip the RPC call to look up PackageBase.
 * Provided by hook_install_packages when discover_aur_deps already fetched it.
 */
static gboolean
hook_install (PkBackendJob *job,
			  const gchar *package_id,
			  const gchar *pkgbase_hint,
			  GError **error)
{
	PkBackend *backend;
	PkBackendAlpmPrivate *priv;
	PkAlpmAur *aur_ctx;
	gchar *pkgname = NULL;
	gchar *pkgbase = NULL;
	gchar *clone_dir = NULL;
	AurSrcinfo *si = NULL;
	AurSrcinfoPackage *sipkg;
	gchar **saved_depends = NULL;
	gchar **saved_makedepends = NULL;
	uid_t build_uid;
	AurBuildResult *build_result = NULL;
	gchar **pkg_paths;
	guint i;
	alpm_siglevel_t level;
	gboolean ret = FALSE;
	gboolean trans_started = FALSE;

	backend = pk_backend_job_get_backend (job);
	priv = pk_backend_get_user_data (backend);
	aur_ctx = (PkAlpmAur *) priv->aur_ctx;

	if (aur_ctx == NULL) {
		g_set_error (error, PK_ALPM_ERROR, PK_ERROR_ENUM_INTERNAL_ERROR,
					 "AUR subsystem not initialized");
		return FALSE;
	}

	pkgname = pk_alpm_aur_id_name (package_id);

	/* Step 1: Get PackageBase — use hint if available, otherwise RPC */
	if (pkgbase_hint != NULL) {
		pkgbase = g_strdup (pkgbase_hint);
	} else {
		const gchar *names[2];
		PkAlpmAurResult *info_result;

		names[0] = pkgname;
		names[1] = NULL;
		info_result = pk_alpm_aur_get_details (aur_ctx, names,
						get_system_arch (priv->alpm), error);
		if (info_result == NULL || info_result->count == 0) {
			if (error != NULL && *error == NULL)
				g_set_error (error, PK_ALPM_ERROR, PK_ERROR_ENUM_PACKAGE_NOT_FOUND,
							 "AUR package not found: %s", pkgname);
			pk_alpm_aur_result_free (info_result);
			goto cleanup;
		}
		pkgbase = g_strdup (info_result->packages[0]->pkgbase ?
							info_result->packages[0]->pkgbase : pkgname);
		pk_alpm_aur_result_free (info_result);
	}

	/* Step 2: Clone/update the AUR git repo */
	pk_backend_job_set_status (job, PK_STATUS_ENUM_DOWNLOAD);
	clone_dir = aur_builder_clone (aur_ctx->builder, pkgbase, error);
	if (clone_dir == NULL)
		goto cleanup;
	g_free (clone_dir);
	clone_dir = NULL;

	/* Step 3: Parse .SRCINFO (safe, no code execution) */
	si = aur_builder_read_srcinfo (aur_ctx->builder, pkgbase, error);
	if (si == NULL)
		goto cleanup;

	/* Verify the package exists in the .SRCINFO — search by pkgname, not pkgbase,
	 * because split packages have pkgbase != pkgname */
	sipkg = aur_srcinfo_find_pkg (si, pkgname);
	if (sipkg == NULL && si->count > 0)
		sipkg = si->packages[0]; /* fallback to first package */

	/* Warn if AUR package conflicts with installed official packages */
	if (sipkg != NULL && sipkg->conflicts != NULL) {
		guint c;
		for (c = 0; sipkg->conflicts[c] != NULL; c++) {
			gchar *cname;
			alpm_pkg_t *local_pkg;

			cname = dep_bare_name (sipkg->conflicts[c]);
			local_pkg = alpm_db_get_pkg (priv->localdb, cname);
			if (local_pkg != NULL) {
				const alpm_list_t *dbs;
				for (dbs = alpm_get_syncdbs (priv->alpm);
					 dbs != NULL; dbs = dbs->next) {
					if (alpm_db_get_pkg (dbs->data, cname)
						!= NULL) {
						syslog (LOG_DAEMON | LOG_WARNING,
							"AUR package '%s' "
							"conflicts with "
							"official package '%s'",
							pkgbase, cname);
						break;
					}
				}
			}
			g_free (cname);
		}
	}

	/* Step 3b: Pre-install depends + makedepends from repos via pacman.
	 * makepkg no longer uses -s (would deadlock on pacman db lock).
	 * We're root, lock is released (sync transaction already committed). */
	if (sipkg != NULL) {
		GPtrArray *all_deps;
		guint d;

		all_deps = g_ptr_array_new ();
		g_ptr_array_add (all_deps, (gpointer) "pacman");
		g_ptr_array_add (all_deps, (gpointer) "-S");
		g_ptr_array_add (all_deps, (gpointer) "--noconfirm");
		g_ptr_array_add (all_deps, (gpointer) "--needed");
		g_ptr_array_add (all_deps, (gpointer) "--asdeps");

		if (sipkg->depends != NULL)
			for (d = 0; sipkg->depends[d] != NULL; d++)
				g_ptr_array_add (all_deps, sipkg->depends[d]);
		if (sipkg->makedepends != NULL)
			for (d = 0; sipkg->makedepends[d] != NULL; d++)
				g_ptr_array_add (all_deps, sipkg->makedepends[d]);

		if (all_deps->len > 5) { /* has at least one dep */
			gint dep_status = 0;

			g_ptr_array_add (all_deps, NULL);
			g_spawn_sync (NULL, (gchar **) all_deps->pdata, NULL,
						  G_SPAWN_SEARCH_PATH,
						  NULL, NULL, NULL, NULL, &dep_status, NULL);
			if (dep_status != 0)
				syslog (LOG_DAEMON | LOG_WARNING,
					"AUR: dependency pre-install failed "
					"for %s (exit %d)",
					pkgbase, dep_status);
		}
		g_ptr_array_unref (all_deps);
	}

	/* Save dep arrays before freeing .SRCINFO (needed for makedep cleanup) */
	if (sipkg != NULL) {
		saved_depends = g_strdupv (sipkg->depends);
		saved_makedepends = g_strdupv (sipkg->makedepends);
	}
	aur_srcinfo_free (si);
	si = NULL;

	/* Step 4: Build with makepkg (fork+setuid to client user) */
	pk_backend_job_set_status (job, PK_STATUS_ENUM_INSTALL);
	build_uid = pk_backend_job_get_uid (job);

	if (build_uid == 0) {
		g_set_error (error, PK_ALPM_ERROR, PK_ERROR_ENUM_NOT_AUTHORIZED,
					 "cannot build AUR package as root — "
					 "install must be initiated by a non-root user");
		goto cleanup;
	}

	/* Set cancel callback so long builds can be interrupted */
	aur_builder_set_cancel (aur_ctx->builder,
							check_job_cancelled, job);

	build_result = aur_builder_build (aur_ctx->builder, pkgbase,
									  build_uid, error);
	if (build_result == NULL)
		goto cleanup;

	if (build_result->exit_code != 0) {
		if (error != NULL && *error == NULL) {
			const gchar *tail;
			/* Show last 200 chars of build log for diagnostics */
			tail = build_result->build_log;
			if (tail != NULL && strlen (tail) > 200)
				tail = tail + strlen (tail) - 200;
			g_set_error (error, PK_ALPM_ERROR, PK_ERROR_ENUM_PACKAGE_FAILED_TO_BUILD,
						 "makepkg failed for %s (exit %d): %s",
						 pkgbase, build_result->exit_code,
						 tail ? tail : "(no log)");
		}
		goto cleanup;
	}

	if (build_result->package_paths == NULL || build_result->count == 0) {
		g_set_error (error, PK_ALPM_ERROR, PK_ERROR_ENUM_PACKAGE_FAILED_TO_BUILD,
					 "makepkg produced no packages for %s", pkgbase);
		goto cleanup;
	}

	/* Step 5: Install built .pkg.tar.zst via libalpm.
	 * We run our own transaction here — the main sync transaction has
	 * already committed, so the db lock is free. */
	pk_backend_job_set_status (job, PK_STATUS_ENUM_INSTALL);
	level = ALPM_SIG_PACKAGE_OPTIONAL;
	pkg_paths = build_result->package_paths;

	if (alpm_trans_init (priv->alpm, 0) < 0) {
		alpm_errno_t alpm_err = alpm_errno (priv->alpm);
		g_set_error (error, PK_ALPM_ERROR, alpm_err,
					 "cannot init transaction: %s", alpm_strerror (alpm_err));
		goto cleanup;
	}
	trans_started = TRUE;

	for (i = 0; pkg_paths[i] != NULL; i++) {
		alpm_pkg_t *pkg = NULL;

		if (alpm_pkg_load (priv->alpm, pkg_paths[i], 1, level, &pkg) < 0) {
			alpm_errno_t alpm_err = alpm_errno (priv->alpm);
			g_set_error (error, PK_ALPM_ERROR, alpm_err,
						 "%s: %s", pkg_paths[i], alpm_strerror (alpm_err));
			goto cleanup;
		}

		if (alpm_add_pkg (priv->alpm, pkg) < 0) {
			alpm_errno_t alpm_err = alpm_errno (priv->alpm);
			alpm_pkg_free (pkg);
			g_set_error (error, PK_ALPM_ERROR, alpm_err,
						 "%s: %s", pkg_paths[i], alpm_strerror (alpm_err));
			goto cleanup;
		}
	}

	/* Prepare and commit */
	{
		alpm_list_t *data = NULL;

		if (alpm_trans_prepare (priv->alpm, &data) < 0) {
			alpm_errno_t alpm_err = alpm_errno (priv->alpm);
			g_set_error (error, PK_ALPM_ERROR, alpm_err,
						 "prepare failed: %s", alpm_strerror (alpm_err));
			alpm_list_free (data);
			goto cleanup;
		}

		if (alpm_trans_commit (priv->alpm, &data) < 0) {
			alpm_errno_t alpm_err = alpm_errno (priv->alpm);
			g_set_error (error, PK_ALPM_ERROR, alpm_err,
						 "commit failed: %s", alpm_strerror (alpm_err));
			alpm_list_free (data);
			goto cleanup;
		}
		alpm_list_free (data);
	}

	/* Transaction succeeded — release before makedep cleanup */
	alpm_trans_release (priv->alpm);
	trans_started = FALSE;

	/* Remove makedepends that aren't runtime deps and are now orphaned.
	 * Only remove packages that pacman -Qdtq reports as orphans: installed
	 * as deps (--asdeps) with no installed package depending on them.
	 * This protects pre-existing packages (e.g. git installed explicitly). */
	if (saved_makedepends != NULL) {
		const gchar *qdtq_argv[] = { "pacman", "-Qdtq", NULL };
		gchar *orphan_out = NULL;
		gchar **orphan_list = NULL;
		GPtrArray *to_remove;
		guint m;

		g_spawn_sync (NULL, (gchar **) qdtq_argv, NULL,
					  G_SPAWN_SEARCH_PATH,
					  NULL, NULL, &orphan_out, NULL, NULL, NULL);
		if (orphan_out != NULL && *g_strstrip (orphan_out) != '\0')
			orphan_list = g_strsplit (orphan_out, "\n", -1);
		g_free (orphan_out);

		to_remove = g_ptr_array_new_with_free_func (g_free);

		for (m = 0; saved_makedepends[m] != NULL; m++) {
			gchar *bare;

			bare = dep_bare_name (saved_makedepends[m]);

			/* Skip runtime deps */
			if (name_in_deparray (bare, saved_depends)) {
				g_free (bare);
				continue;
			}
			/* Only remove if it's an orphan (installed as dep, no dependents) */
			if (orphan_list == NULL ||
				!g_strv_contains ((const gchar *const *) orphan_list,
								  bare)) {
				g_free (bare);
				continue;
			}

			g_ptr_array_add (to_remove, bare);
		}

		if (to_remove->len > 0) {
			GPtrArray *rm_args;

			rm_args = g_ptr_array_new ();
			g_ptr_array_add (rm_args, (gpointer) "pacman");
			g_ptr_array_add (rm_args, (gpointer) "-R");
			g_ptr_array_add (rm_args, (gpointer) "--noconfirm");
			for (m = 0; m < to_remove->len; m++)
				g_ptr_array_add (rm_args,
								 g_ptr_array_index (to_remove, m));
			g_ptr_array_add (rm_args, NULL);
			g_spawn_sync (NULL, (gchar **) rm_args->pdata, NULL,
						  G_SPAWN_SEARCH_PATH,
						  NULL, NULL, NULL, NULL, NULL, NULL);
			g_ptr_array_unref (rm_args);
		}

		g_ptr_array_unref (to_remove);
		g_strfreev (orphan_list);
	}

	ret = TRUE;

cleanup:
	if (trans_started)
		alpm_trans_release (priv->alpm);
	if (build_result != NULL)
		aur_build_result_free (build_result);
	if (si != NULL)
		aur_srcinfo_free (si);
	g_free (clone_dir);
	g_strfreev (saved_depends);
	g_strfreev (saved_makedepends);
	g_free (pkgbase);
	g_free (pkgname);
	return ret;
}

/*
 * Public wrapper for standalone single-package install (no pkgbase hint).
 */
gboolean
pk_alpm_aur_hook_install (PkBackendJob *job,
						  const gchar *package_id,
						  GError **error)
{
	return hook_install (job, package_id, NULL, error);
}

/*
 * Hook 3b: Install all AUR packages from a package ID list.
 * Called after the main sync transaction commits (lock is released).
 *
 * Resolves AUR dependency tree (recursive AUR-depends-on-AUR),
 * topological-sorts into build order (leaves first), then builds
 * and installs each package in order.
 *
 * Passes the pkgbase map from discover_aur_deps to hook_install,
 * eliminating redundant per-package RPC calls.
 */
void
pk_alpm_aur_hook_install_packages (PkBackendJob *job,
								   const gchar **package_ids,
								   GError **error)
{
	PkBackend *backend;
	PkBackendAlpmPrivate *priv;
	GHashTable *pkgbase_map;
	gchar **build_order;
	guint i;

	backend = pk_backend_job_get_backend (job);
	priv = pk_backend_get_user_data (backend);
	pkgbase_map = NULL;

	if (priv->aur_ctx == NULL)
		return;

	build_order = resolve_aur_build_order (job, package_ids,
											&pkgbase_map, error);
	if (build_order == NULL)
		return; /* no AUR packages in list */

	for (i = 0; build_order[i] != NULL; i++) {
		gchar *name;
		const gchar *hint;

		if (pk_backend_job_is_cancelled (job))
			break;

		name = pk_alpm_aur_id_name (build_order[i]);
		hint = pkgbase_map != NULL ?
			g_hash_table_lookup (pkgbase_map, name) : NULL;
		g_free (name);

		if (!hook_install (job, build_order[i], hint, error)) {
			g_strfreev (build_order);
			if (pkgbase_map != NULL)
				g_hash_table_unref (pkgbase_map);
			return;
		}
	}

	g_strfreev (build_order);
	if (pkgbase_map != NULL)
		g_hash_table_unref (pkgbase_map);
}

/*
 * Check if a package name indicates a VCS package (-git, -svn, etc.)
 */
static gboolean
is_vcs_package (const gchar *name)
{
	return (g_str_has_suffix (name, "-git") ||
			g_str_has_suffix (name, "-svn") ||
			g_str_has_suffix (name, "-hg") ||
			g_str_has_suffix (name, "-bzr"));
}

/*
 * Hook 4: Get AUR updates
 *
 * For VCS packages (-git etc.), fetches sources and runs
 * makepkg --printsrcinfo to get the real pkgver() version,
 * then compares with installed. Falls back to LOW priority
 * if the real version can't be determined.
 */
void
pk_alpm_aur_hook_get_updates (PkBackendJob *job,
							  GError **error)
{
	PkBackend *backend;
	PkBackendAlpmPrivate *priv;
	PkAlpmAur *aur_ctx;
	const gchar *arch;
	alpm_list_t *local_pkgs;
	const alpm_list_t *it;
	GPtrArray *foreign_names;
	GHashTable *inst_versions; /* name (borrowed) -> version (borrowed) */
	gchar **name_array;
	AurResult *rpc_result;
	uid_t build_uid;
	guint i;

	backend = pk_backend_job_get_backend (job);
	priv = pk_backend_get_user_data (backend);
	aur_ctx = (PkAlpmAur *) priv->aur_ctx;

	if (aur_ctx == NULL)
		return;
	arch = get_system_arch (priv->alpm);
	build_uid = pk_backend_job_get_uid (job);

	/* Collect foreign packages: installed locally but not in any syncdb.
	 * Equivalent of "pacman -Qm" but via libalpm directly. */
	local_pkgs = alpm_db_get_pkgcache (priv->localdb);
	foreign_names = g_ptr_array_new_with_free_func (g_free);
	inst_versions = g_hash_table_new (g_str_hash, g_str_equal);

	for (it = local_pkgs; it != NULL; it = it->next) {
		alpm_pkg_t *pkg = it->data;
		const gchar *name = alpm_pkg_get_name (pkg);
		const alpm_list_t *dbs;
		gboolean in_sync = FALSE;

		for (dbs = alpm_get_syncdbs (priv->alpm);
		     dbs != NULL; dbs = dbs->next) {
			if (alpm_db_get_pkg (dbs->data, name) != NULL) {
				in_sync = TRUE;
				break;
			}
		}

		if (!in_sync) {
			g_ptr_array_add (foreign_names, g_strdup (name));
			/* Strings borrowed from alpm_pkg_t — valid while
			 * localdb stays open (entire backend lifetime). */
			g_hash_table_insert (inst_versions,
					     (gpointer) name,
					     (gpointer) alpm_pkg_get_version (pkg));
		}
	}

	if (foreign_names->len == 0) {
		g_ptr_array_unref (foreign_names);
		g_hash_table_unref (inst_versions);
		return;
	}

	/* Query AUR for all foreign packages in one batch */
	g_ptr_array_add (foreign_names, NULL);
	name_array = (gchar **) g_ptr_array_free (foreign_names, FALSE);

	rpc_result = aur_rpc_info (aur_ctx->rpc,
				   (const gchar *const *) name_array, error);
	g_strfreev (name_array);

	if (rpc_result == NULL) {
		g_hash_table_unref (inst_versions);
		return;
	}

	/* Compare versions and emit updates */
	for (i = 0; i < rpc_result->count; i++) {
		AurPackage *aur_pkg = rpc_result->packages[i];
		const gchar *inst_ver;
		const gchar *emit_ver;
		int cmp;
		PkInfoEnum info;
		gchar *pkg_id;
		gchar *saved_real_ver;

		if (pk_backend_job_is_cancelled (job))
			break;

		inst_ver = g_hash_table_lookup (inst_versions,
						aur_pkg->name);
		if (inst_ver == NULL)
			continue;

		cmp = alpm_pkg_vercmp (aur_pkg->version, inst_ver);

		/* For non-VCS: skip if AUR is not newer.
		 * For VCS: always check (version from RPC is unreliable). */
		if (cmp <= 0 && !is_vcs_package (aur_pkg->name))
			continue;

		info = PK_INFO_ENUM_NORMAL;
		emit_ver = aur_pkg->version;
		saved_real_ver = NULL;

		if (is_vcs_package (aur_pkg->name) && build_uid != 0) {
			/* VCS: fetch sources + printsrcinfo for real version */
			gchar *clone_dir;
			AurSrcinfo *fresh_si;
			GError *vcs_err = NULL;
			const gchar *pkgbase;

			pkgbase = aur_pkg->package_base ?
				  aur_pkg->package_base : aur_pkg->name;
			clone_dir = aur_builder_clone (aur_ctx->builder,
						       pkgbase, &vcs_err);
			if (clone_dir != NULL) {
				g_free (clone_dir);
				fresh_si = aur_builder_fresh_srcinfo (
					aur_ctx->builder, pkgbase,
					build_uid, &vcs_err);
				if (fresh_si != NULL) {
					AurSrcinfoPackage *spkg;
					gchar *real_ver;

					spkg = aur_srcinfo_find_pkg (
						fresh_si, aur_pkg->name);
					if (spkg == NULL &&
					    fresh_si->count > 0)
						spkg = fresh_si->packages[0];

					real_ver = aur_srcinfo_pkg_version (
						spkg);

					if (real_ver != NULL) {
						cmp = alpm_pkg_vercmp (
							real_ver, inst_ver);
						if (cmp <= 0) {
							g_free (real_ver);
							aur_srcinfo_free (
								fresh_si);
							continue;
						}
						/* Use real version for the emitted package_id */
						saved_real_ver = real_ver;
						emit_ver = saved_real_ver;
					} else {
						/* Could not determine real version */
						info = PK_INFO_ENUM_LOW;
					}
					aur_srcinfo_free (fresh_si);
				} else {
					info = PK_INFO_ENUM_LOW;
					g_clear_error (&vcs_err);
				}
			} else {
				info = PK_INFO_ENUM_LOW;
				g_clear_error (&vcs_err);
			}
		} else if (is_vcs_package (aur_pkg->name)) {
			/* VCS but uid=0 — can't run makepkg, mark LOW */
			info = PK_INFO_ENUM_LOW;
		} else if (cmp <= 0) {
			/* Non-VCS but not newer — already filtered above,
			 * but guard against logic changes. */
			continue;
		}

		pkg_id = pk_alpm_aur_build_id (aur_pkg->name,
						emit_ver, arch);
		pk_backend_job_package (job, info, pkg_id,
					aur_pkg->description ?
					aur_pkg->description : "");
		g_free (pkg_id);
		g_free (saved_real_ver);
	}

	aur_result_free (rpc_result);
	g_hash_table_unref (inst_versions);
}
