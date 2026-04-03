/*
 * aur-rpc.h — AUR RPC v5 client library
 *
 * Provides search and info queries against the AUR RPC API.
 * Thread-safe per-handle (each AurRpc* is independent).
 * Caller owns all returned data and must free with provided functions.
 */

#ifndef AUR_RPC_H
#define AUR_RPC_H

#include <glib.h>
#include <stdint.h>

/* Opaque client handle */
typedef struct _AurRpc AurRpc;

/* Package info — superset of search + info fields.
 * Fields absent from search results are NULL/0. */
typedef struct {
	/* Identity */
	gchar    *name;
	gchar    *package_base;
	gchar    *version;
	gchar    *description;
	gchar    *url;
	gchar    *url_path;       /* /cgit/aur.git/snapshot/{base}.tar.gz */
	gchar    *maintainer;     /* NULL if orphan */
	gchar    *submitter;      /* info only */

	/* Metadata */
	guint     id;
	guint     package_base_id;
	gint64    first_submitted; /* unix timestamp */
	gint64    last_modified;   /* unix timestamp */
	gint64    out_of_date;     /* unix timestamp, 0 if not flagged */
	guint     num_votes;
	gdouble   popularity;

	/* Dependencies — info endpoint only, NULL from search */
	gchar   **depends;
	gchar   **make_depends;
	gchar   **opt_depends;
	gchar   **check_depends;
	gchar   **conflicts;
	gchar   **provides;
	gchar   **replaces;
	gchar   **license;
	gchar   **keywords;
	gchar   **groups;
} AurPackage;

/* Search-by field for aur_rpc_search() */
typedef enum {
	AUR_SEARCH_BY_NAME_DESC,  /* default — matches name and description */
	AUR_SEARCH_BY_NAME,       /* name only */
	AUR_SEARCH_BY_MAINTAINER,
	AUR_SEARCH_BY_DEPENDS,
	AUR_SEARCH_BY_MAKEDEPENDS,
	AUR_SEARCH_BY_OPTDEPENDS,
	AUR_SEARCH_BY_CHECKDEPENDS,
	AUR_SEARCH_BY_SUBMITTER,
	AUR_SEARCH_BY_PROVIDES,
	AUR_SEARCH_BY_CONFLICTS,
	AUR_SEARCH_BY_REPLACES,
	AUR_SEARCH_BY_KEYWORDS,
	AUR_SEARCH_BY_GROUPS,
	AUR_SEARCH_BY_COMAINTAINERS,
} AurSearchBy;

/* Result set from search or info queries */
typedef struct {
	AurPackage **packages;     /* NULL-terminated array */
	guint        count;
} AurResult;

/* Create/destroy client handle.
 * base_url: NULL for default "https://aur.archlinux.org" */
AurRpc     *aur_rpc_new          (const gchar *base_url);
void        aur_rpc_free         (AurRpc *rpc);

/* Search AUR packages.
 * Returns NULL on error, sets *error. Caller frees with aur_result_free(). */
AurResult  *aur_rpc_search       (AurRpc *rpc,
								  const gchar *query,
								  AurSearchBy by,
								  GError **error);

/* Get detailed info for one or more packages by exact name.
 * names: NULL-terminated array. Returns NULL on error. */
AurResult  *aur_rpc_info         (AurRpc *rpc,
								  const gchar * const *names,
								  GError **error);

/* Free results */
void        aur_package_free     (AurPackage *pkg);
void        aur_result_free      (AurResult *result);

/* Error domain */
#define AUR_RPC_ERROR (aur_rpc_error_quark())
GQuark      aur_rpc_error_quark  (void);

typedef enum {
	AUR_RPC_ERROR_CURL,        /* libcurl failure */
	AUR_RPC_ERROR_HTTP,        /* non-200 response */
	AUR_RPC_ERROR_PARSE,       /* JSON parse failure */
	AUR_RPC_ERROR_API,         /* AUR API returned error type */
} AurRpcError;

#endif /* AUR_RPC_H */
