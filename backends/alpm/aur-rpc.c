/*
 * aur-rpc.c — AUR RPC v5 client implementation
 *
 * Uses libcurl for HTTP, json-glib for parsing.
 * Each AurRpc handle owns its own CURL easy handle.
 */

#include "aur-rpc.h"
#include <curl/curl.h>
#include <json-glib/json-glib.h>
#include <string.h>

#define AUR_DEFAULT_BASE_URL "https://aur.archlinux.org"
#define AUR_RPC_PATH         "/rpc/v5"
#define AUR_USER_AGENT       "packagekit-alpm-aur/0.1"
#define AUR_MAX_RETRIES      3
#define AUR_RETRY_BASE_MS    500

struct _AurRpc {
	CURL  *curl;
	gchar *base_url;
};

G_DEFINE_QUARK (aur-rpc-error-quark, aur_rpc_error)

/* --- curl write callback --- */

typedef struct {
	gchar  *data;
	gsize   len;
	gsize   alloc;
} CurlBuffer;

static gsize
curl_write_cb (gchar *ptr, gsize size, gsize nmemb, gpointer userdata)
{
	CurlBuffer *buf = userdata;
	gsize bytes = size * nmemb;

	if (buf->len + bytes + 1 > buf->alloc) {
		buf->alloc = (buf->len + bytes + 1) * 2;
		buf->data = g_realloc (buf->data, buf->alloc);
	}
	memcpy (buf->data + buf->len, ptr, bytes);
	buf->len += bytes;
	buf->data[buf->len] = '\0';
	return bytes;
}

/* --- URL construction --- */

static const gchar *
search_by_param (AurSearchBy by)
{
	switch (by) {
	case AUR_SEARCH_BY_NAME_DESC:    return "name-desc";
	case AUR_SEARCH_BY_NAME:         return "name";
	case AUR_SEARCH_BY_MAINTAINER:   return "maintainer";
	case AUR_SEARCH_BY_DEPENDS:      return "depends";
	case AUR_SEARCH_BY_MAKEDEPENDS:  return "makedepends";
	case AUR_SEARCH_BY_OPTDEPENDS:   return "optdepends";
	case AUR_SEARCH_BY_CHECKDEPENDS: return "checkdepends";
	case AUR_SEARCH_BY_SUBMITTER:    return "submitter";
	case AUR_SEARCH_BY_PROVIDES:     return "provides";
	case AUR_SEARCH_BY_CONFLICTS:    return "conflicts";
	case AUR_SEARCH_BY_REPLACES:     return "replaces";
	case AUR_SEARCH_BY_KEYWORDS:     return "keywords";
	case AUR_SEARCH_BY_GROUPS:       return "groups";
	case AUR_SEARCH_BY_COMAINTAINERS: return "comaintainers";
	}
	return "name-desc";
}

static gchar *
build_search_url (AurRpc *rpc, const gchar *query, AurSearchBy by)
{
	gchar *escaped = g_uri_escape_string (query, NULL, FALSE);
	gchar *url = g_strdup_printf ("%s%s/search/%s?by=%s",
								  rpc->base_url, AUR_RPC_PATH,
								  escaped, search_by_param (by));
	g_free (escaped);
	return url;
}

static gchar *
build_info_url (AurRpc *rpc, const gchar * const *names)
{
	GString *url = g_string_new (NULL);
	guint i;
	g_string_printf (url, "%s%s/info?", rpc->base_url, AUR_RPC_PATH);
	for (i = 0; names[i] != NULL; i++) {
		gchar *escaped;
		if (i > 0)
			g_string_append_c (url, '&');
		escaped = g_uri_escape_string (names[i], NULL, FALSE);
		g_string_append_printf (url, "arg[]=%s", escaped);
		g_free (escaped);
	}
	return g_string_free (url, FALSE);
}

/* --- HTTP fetch --- */

static gboolean
is_retryable_curl (CURLcode res)
{
	switch (res) {
	case CURLE_COULDNT_RESOLVE_HOST:
	case CURLE_COULDNT_CONNECT:
	case CURLE_OPERATION_TIMEDOUT:
	case CURLE_GOT_NOTHING:
	case CURLE_SEND_ERROR:
	case CURLE_RECV_ERROR:
		return TRUE;
	default:
		return FALSE;
	}
}

static gchar *
fetch_url (AurRpc *rpc, const gchar *url, GError **error)
{
	CurlBuffer buf;
	long http_code;
	CURLcode res;
	guint attempt;
	guint delay_ms;

	curl_easy_setopt (rpc->curl, CURLOPT_URL, url);
	curl_easy_setopt (rpc->curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
	curl_easy_setopt (rpc->curl, CURLOPT_USERAGENT, AUR_USER_AGENT);
	curl_easy_setopt (rpc->curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt (rpc->curl, CURLOPT_TIMEOUT, 30L);

	for (attempt = 0; attempt < AUR_MAX_RETRIES; attempt++) {
		buf.data = NULL;
		buf.len = 0;
		buf.alloc = 0;
		http_code = 0;

		curl_easy_setopt (rpc->curl, CURLOPT_WRITEDATA, &buf);
		res = curl_easy_perform (rpc->curl);

		if (res != CURLE_OK) {
			g_free (buf.data);
			if (is_retryable_curl (res) &&
			    attempt + 1 < AUR_MAX_RETRIES) {
				delay_ms = AUR_RETRY_BASE_MS << attempt;
				g_usleep (delay_ms * 1000);
				continue;
			}
			g_set_error (error, AUR_RPC_ERROR, AUR_RPC_ERROR_CURL,
						 "curl: %s (url: %s, attempt %u/%u)",
						 curl_easy_strerror (res), url,
						 attempt + 1, AUR_MAX_RETRIES);
			return NULL;
		}

		curl_easy_getinfo (rpc->curl, CURLINFO_RESPONSE_CODE,
				   &http_code);

		/* Retry on server errors (5xx) */
		if (http_code >= 500 && attempt + 1 < AUR_MAX_RETRIES) {
			g_free (buf.data);
			delay_ms = AUR_RETRY_BASE_MS << attempt;
			g_usleep (delay_ms * 1000);
			continue;
		}

		if (http_code != 200) {
			g_set_error (error, AUR_RPC_ERROR, AUR_RPC_ERROR_HTTP,
						 "HTTP %ld from %s", http_code, url);
			g_free (buf.data);
			return NULL;
		}

		return buf.data;
	}

	/* Should not reach here, but safety net */
	g_set_error (error, AUR_RPC_ERROR, AUR_RPC_ERROR_CURL,
				 "fetch failed after %u attempts: %s",
				 AUR_MAX_RETRIES, url);
	return NULL;
}

/* --- JSON parsing --- */

/* Extract a string array from a JSON array node. Returns NULL-terminated strv. */
static gchar **
json_array_to_strv (JsonArray *arr)
{
	guint len;
	gchar **result;
	guint i;

	if (arr == NULL)
		return NULL;
	len = json_array_get_length (arr);
	if (len == 0)
		return NULL;
	result = g_new0 (gchar *, len + 1);
	for (i = 0; i < len; i++)
		result[i] = g_strdup (json_array_get_string_element (arr, i));
	return result;
}

static gchar **
json_object_get_strv (JsonObject *obj, const gchar *member)
{
	JsonNode *node;

	if (!json_object_has_member (obj, member))
		return NULL;
	node = json_object_get_member (obj, member);
	if (JSON_NODE_HOLDS_NULL (node))
		return NULL;
	return json_array_to_strv (json_node_get_array (node));
}

static const gchar *
json_object_get_string_or_null (JsonObject *obj, const gchar *member)
{
	JsonNode *node;

	if (!json_object_has_member (obj, member))
		return NULL;
	node = json_object_get_member (obj, member);
	if (JSON_NODE_HOLDS_NULL (node))
		return NULL;
	return json_node_get_string (node);
}

static AurPackage *
parse_package (JsonObject *obj)
{
	AurPackage *pkg = g_new0 (AurPackage, 1);
	const gchar *s;

	/* Required fields (always present) */
	pkg->name            = g_strdup (json_object_get_string_member (obj, "Name"));
	pkg->version         = g_strdup (json_object_get_string_member (obj, "Version"));
	pkg->package_base    = g_strdup (json_object_get_string_member (obj, "PackageBase"));
	pkg->id              = (guint) json_object_get_int_member (obj, "ID");
	pkg->package_base_id = (guint) json_object_get_int_member (obj, "PackageBaseID");
	pkg->first_submitted = json_object_get_int_member (obj, "FirstSubmitted");
	pkg->last_modified   = json_object_get_int_member (obj, "LastModified");
	pkg->num_votes       = (guint) json_object_get_int_member (obj, "NumVotes");
	pkg->popularity      = json_object_get_double_member (obj, "Popularity");

	/* Optional string fields */
	s = json_object_get_string_or_null (obj, "Description");
	pkg->description = g_strdup (s);
	s = json_object_get_string_or_null (obj, "URL");
	pkg->url = g_strdup (s);
	s = json_object_get_string_or_null (obj, "URLPath");
	pkg->url_path = g_strdup (s);
	s = json_object_get_string_or_null (obj, "Maintainer");
	pkg->maintainer = g_strdup (s);
	s = json_object_get_string_or_null (obj, "Submitter");
	pkg->submitter = g_strdup (s);

	/* OutOfDate: null or unix timestamp */
	if (json_object_has_member (obj, "OutOfDate") &&
		!JSON_NODE_HOLDS_NULL (json_object_get_member (obj, "OutOfDate")))
		pkg->out_of_date = json_object_get_int_member (obj, "OutOfDate");

	/* Dependency arrays — only from info endpoint */
	pkg->depends        = json_object_get_strv (obj, "Depends");
	pkg->make_depends   = json_object_get_strv (obj, "MakeDepends");
	pkg->opt_depends    = json_object_get_strv (obj, "OptDepends");
	pkg->check_depends  = json_object_get_strv (obj, "CheckDepends");
	pkg->conflicts      = json_object_get_strv (obj, "Conflicts");
	pkg->provides       = json_object_get_strv (obj, "Provides");
	pkg->replaces       = json_object_get_strv (obj, "Replaces");
	pkg->license        = json_object_get_strv (obj, "License");
	pkg->keywords       = json_object_get_strv (obj, "Keywords");
	pkg->groups         = json_object_get_strv (obj, "Groups");

	return pkg;
}

static AurResult *
parse_response (const gchar *json_str, GError **error)
{
	g_autoptr(JsonParser) parser = json_parser_new ();
	JsonNode *root;
	JsonObject *root_obj;
	guint count;
	JsonArray *results_arr;
	AurResult *result;
	guint i;

	if (!json_parser_load_from_data (parser, json_str, -1, error)) {
		g_prefix_error (error, "AUR RPC JSON parse: ");
		return NULL;
	}

	root = json_parser_get_root (parser);
	if (!JSON_NODE_HOLDS_OBJECT (root)) {
		g_set_error (error, AUR_RPC_ERROR, AUR_RPC_ERROR_PARSE,
					 "AUR RPC: root is not a JSON object");
		return NULL;
	}

	root_obj = json_node_get_object (root);

	/* Check for API error */
	if (json_object_has_member (root_obj, "type")) {
		const gchar *type = json_object_get_string_member (root_obj, "type");
		if (g_strcmp0 (type, "error") == 0) {
			const gchar *err_msg = "unknown error";
			if (json_object_has_member (root_obj, "error"))
				err_msg = json_object_get_string_member (root_obj, "error");
			g_set_error (error, AUR_RPC_ERROR, AUR_RPC_ERROR_API,
						 "AUR RPC error: %s", err_msg);
			return NULL;
		}
	}

	/* Parse results array */
	count = (guint) json_object_get_int_member (root_obj, "resultcount");
	results_arr = json_object_get_array_member (root_obj, "results");

	result = g_new0 (AurResult, 1);
	result->count = count;
	result->packages = g_new0 (AurPackage *, count + 1);

	for (i = 0; i < count; i++) {
		JsonObject *pkg_obj = json_array_get_object_element (results_arr, i);
		result->packages[i] = parse_package (pkg_obj);
	}

	return result;
}

/* --- Public API --- */

AurRpc *
aur_rpc_new (const gchar *base_url)
{
	AurRpc *rpc = g_new0 (AurRpc, 1);
	rpc->curl = curl_easy_init ();
	rpc->base_url = g_strdup (base_url ? base_url : AUR_DEFAULT_BASE_URL);
	return rpc;
}

void
aur_rpc_free (AurRpc *rpc)
{
	if (rpc == NULL)
		return;
	curl_easy_cleanup (rpc->curl);
	g_free (rpc->base_url);
	g_free (rpc);
}

AurResult *
aur_rpc_search (AurRpc *rpc, const gchar *query, AurSearchBy by, GError **error)
{
	gchar *url;
	gchar *json;
	AurResult *result;

	g_return_val_if_fail (rpc != NULL, NULL);
	g_return_val_if_fail (query != NULL && *query != '\0', NULL);

	url = build_search_url (rpc, query, by);
	json = fetch_url (rpc, url, error);
	g_free (url);
	if (json == NULL)
		return NULL;

	result = parse_response (json, error);
	g_free (json);
	return result;
}

AurResult *
aur_rpc_info (AurRpc *rpc, const gchar * const *names, GError **error)
{
	gchar *url;
	gchar *json;
	AurResult *result;

	g_return_val_if_fail (rpc != NULL, NULL);
	g_return_val_if_fail (names != NULL && names[0] != NULL, NULL);

	url = build_info_url (rpc, names);
	json = fetch_url (rpc, url, error);
	g_free (url);
	if (json == NULL)
		return NULL;

	result = parse_response (json, error);
	g_free (json);
	return result;
}

void
aur_package_free (AurPackage *pkg)
{
	if (pkg == NULL)
		return;
	g_free (pkg->name);
	g_free (pkg->package_base);
	g_free (pkg->version);
	g_free (pkg->description);
	g_free (pkg->url);
	g_free (pkg->url_path);
	g_free (pkg->maintainer);
	g_free (pkg->submitter);
	g_strfreev (pkg->depends);
	g_strfreev (pkg->make_depends);
	g_strfreev (pkg->opt_depends);
	g_strfreev (pkg->check_depends);
	g_strfreev (pkg->conflicts);
	g_strfreev (pkg->provides);
	g_strfreev (pkg->replaces);
	g_strfreev (pkg->license);
	g_strfreev (pkg->keywords);
	g_strfreev (pkg->groups);
	g_free (pkg);
}

void
aur_result_free (AurResult *result)
{
	guint i;

	if (result == NULL)
		return;
	for (i = 0; i < result->count; i++)
		aur_package_free (result->packages[i]);
	g_free (result->packages);
	g_free (result);
}
