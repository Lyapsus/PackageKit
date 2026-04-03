/*
 * aur-srcinfo.c — .SRCINFO parser implementation
 */

#include "aur-srcinfo.h"
#include <string.h>

G_DEFINE_QUARK (aur-srcinfo-error-quark, aur_srcinfo_error)

/* Accumulator for multi-value fields during parsing */
typedef struct {
	gchar  *pkgname;
	gchar  *pkgdesc;
	gchar  *pkgver;
	gchar  *pkgrel;
	gchar  *epoch;
	gchar  *url;
	GPtrArray *arch;
	GPtrArray *license;
	GPtrArray *depends;
	GPtrArray *makedepends;
	GPtrArray *checkdepends;
	GPtrArray *optdepends;
	GPtrArray *conflicts;
	GPtrArray *provides;
	GPtrArray *replaces;
	GPtrArray *source;
	GPtrArray *groups;
} ParseSection;

static ParseSection *
parse_section_new (void)
{
	ParseSection *s = g_new0 (ParseSection, 1);
	s->arch         = g_ptr_array_new_with_free_func (g_free);
	s->license      = g_ptr_array_new_with_free_func (g_free);
	s->depends      = g_ptr_array_new_with_free_func (g_free);
	s->makedepends  = g_ptr_array_new_with_free_func (g_free);
	s->checkdepends = g_ptr_array_new_with_free_func (g_free);
	s->optdepends   = g_ptr_array_new_with_free_func (g_free);
	s->conflicts    = g_ptr_array_new_with_free_func (g_free);
	s->provides     = g_ptr_array_new_with_free_func (g_free);
	s->replaces     = g_ptr_array_new_with_free_func (g_free);
	s->source       = g_ptr_array_new_with_free_func (g_free);
	s->groups       = g_ptr_array_new_with_free_func (g_free);
	return s;
}

static gchar **
ptr_array_to_strv (GPtrArray *arr)
{
	if (arr == NULL)
		return NULL;
	if (arr->len == 0) {
		g_ptr_array_unref (arr);
		return NULL;
	}
	g_ptr_array_add (arr, NULL);
	return (gchar **) g_ptr_array_free (arr, FALSE);
}

/* Merge: if per-package array is non-empty, copy it. Otherwise inherit from base.
 * Non-destructive — does not consume the GPtrArray. */
static gchar **
merge_strv (GPtrArray *pkg_arr, gchar **base_strv)
{
	if (pkg_arr->len > 0) {
		gchar **result;
		guint i;

		result = g_new0 (gchar *, pkg_arr->len + 1);
		for (i = 0; i < pkg_arr->len; i++)
			result[i] = g_strdup (g_ptr_array_index (pkg_arr, i));
		return result;
	}
	if (base_strv != NULL)
		return g_strdupv (base_strv);
	return NULL;
}

static void
parse_section_free_arrays_only (ParseSection *s)
{
	/* Free the GPtrArrays that weren't converted to strv */
	if (s->arch)         g_ptr_array_unref (s->arch);
	if (s->license)      g_ptr_array_unref (s->license);
	if (s->depends)      g_ptr_array_unref (s->depends);
	if (s->makedepends)  g_ptr_array_unref (s->makedepends);
	if (s->checkdepends) g_ptr_array_unref (s->checkdepends);
	if (s->optdepends)   g_ptr_array_unref (s->optdepends);
	if (s->conflicts)    g_ptr_array_unref (s->conflicts);
	if (s->provides)     g_ptr_array_unref (s->provides);
	if (s->replaces)     g_ptr_array_unref (s->replaces);
	if (s->source)       g_ptr_array_unref (s->source);
	if (s->groups)       g_ptr_array_unref (s->groups);
}

static void
parse_section_free (ParseSection *s)
{
	if (s == NULL) return;
	g_free (s->pkgname);
	g_free (s->pkgdesc);
	g_free (s->pkgver);
	g_free (s->pkgrel);
	g_free (s->epoch);
	g_free (s->url);
	parse_section_free_arrays_only (s);
	g_free (s);
}

static void
apply_kv (ParseSection *s, const gchar *key, const gchar *value)
{
	/* Single-value fields: last wins */
	if (g_strcmp0 (key, "pkgname") == 0)      { g_free (s->pkgname); s->pkgname = g_strdup (value); }
	else if (g_strcmp0 (key, "pkgdesc") == 0)  { g_free (s->pkgdesc); s->pkgdesc = g_strdup (value); }
	else if (g_strcmp0 (key, "pkgver") == 0)   { g_free (s->pkgver);  s->pkgver  = g_strdup (value); }
	else if (g_strcmp0 (key, "pkgrel") == 0)   { g_free (s->pkgrel);  s->pkgrel  = g_strdup (value); }
	else if (g_strcmp0 (key, "epoch") == 0)     { g_free (s->epoch);   s->epoch   = g_strdup (value); }
	else if (g_strcmp0 (key, "url") == 0)       { g_free (s->url);     s->url     = g_strdup (value); }
	/* Multi-value fields: accumulate */
	else if (g_strcmp0 (key, "arch") == 0)           g_ptr_array_add (s->arch,         g_strdup (value));
	else if (g_strcmp0 (key, "license") == 0)        g_ptr_array_add (s->license,      g_strdup (value));
	else if (g_strcmp0 (key, "depends") == 0)        g_ptr_array_add (s->depends,      g_strdup (value));
	else if (g_strcmp0 (key, "makedepends") == 0)    g_ptr_array_add (s->makedepends,  g_strdup (value));
	else if (g_strcmp0 (key, "checkdepends") == 0)   g_ptr_array_add (s->checkdepends, g_strdup (value));
	else if (g_strcmp0 (key, "optdepends") == 0)     g_ptr_array_add (s->optdepends,   g_strdup (value));
	else if (g_strcmp0 (key, "conflicts") == 0)      g_ptr_array_add (s->conflicts,    g_strdup (value));
	else if (g_strcmp0 (key, "provides") == 0)       g_ptr_array_add (s->provides,     g_strdup (value));
	else if (g_strcmp0 (key, "replaces") == 0)       g_ptr_array_add (s->replaces,     g_strdup (value));
	else if (g_strcmp0 (key, "source") == 0)         g_ptr_array_add (s->source,       g_strdup (value));
	else if (g_strcmp0 (key, "groups") == 0)         g_ptr_array_add (s->groups,       g_strdup (value));
	/* Silently ignore unknown keys (checksums, validpgpkeys, options, etc.) */
}

/* Pre-converted base arrays for inheritance across multiple packages */
typedef struct {
	gchar **arch;
	gchar **license;
	gchar **depends;
	gchar **makedepends;
	gchar **checkdepends;
	gchar **optdepends;
	gchar **conflicts;
	gchar **provides;
	gchar **replaces;
	gchar **source;
	gchar **groups;
} BaseStrv;

static BaseStrv
base_to_strv (ParseSection *base)
{
	/* Convert GPtrArrays to strv. This consumes the GPtrArrays (frees the container). */
	BaseStrv b;
	b.arch         = ptr_array_to_strv (base->arch);         base->arch = NULL;
	b.license      = ptr_array_to_strv (base->license);      base->license = NULL;
	b.depends      = ptr_array_to_strv (base->depends);      base->depends = NULL;
	b.makedepends  = ptr_array_to_strv (base->makedepends);  base->makedepends = NULL;
	b.checkdepends = ptr_array_to_strv (base->checkdepends); base->checkdepends = NULL;
	b.optdepends   = ptr_array_to_strv (base->optdepends);   base->optdepends = NULL;
	b.conflicts    = ptr_array_to_strv (base->conflicts);    base->conflicts = NULL;
	b.provides     = ptr_array_to_strv (base->provides);     base->provides = NULL;
	b.replaces     = ptr_array_to_strv (base->replaces);     base->replaces = NULL;
	b.source       = ptr_array_to_strv (base->source);       base->source = NULL;
	b.groups       = ptr_array_to_strv (base->groups);       base->groups = NULL;
	return b;
}

static void
base_strv_free (BaseStrv *b)
{
	g_strfreev (b->arch);
	g_strfreev (b->license);
	g_strfreev (b->depends);
	g_strfreev (b->makedepends);
	g_strfreev (b->checkdepends);
	g_strfreev (b->optdepends);
	g_strfreev (b->conflicts);
	g_strfreev (b->provides);
	g_strfreev (b->replaces);
	g_strfreev (b->source);
	g_strfreev (b->groups);
}

static AurSrcinfoPackage *
build_package (ParseSection *base, ParseSection *pkg, const BaseStrv *bstrv)
{
	AurSrcinfoPackage *p = g_new0 (AurSrcinfoPackage, 1);

	/* Single-value: per-package overrides base */
	p->pkgname = g_strdup (pkg->pkgname);
	p->pkgdesc = g_strdup (pkg->pkgdesc ? pkg->pkgdesc : base->pkgdesc);
	p->pkgver  = g_strdup (pkg->pkgver  ? pkg->pkgver  : base->pkgver);
	p->pkgrel  = g_strdup (pkg->pkgrel  ? pkg->pkgrel  : base->pkgrel);
	p->epoch   = g_strdup (pkg->epoch   ? pkg->epoch   : base->epoch);
	p->url     = g_strdup (pkg->url     ? pkg->url     : base->url);

	/* Multi-value: per-package overrides or inherits from pre-converted base strv */
	p->arch         = merge_strv (pkg->arch,         bstrv->arch);
	p->license      = merge_strv (pkg->license,      bstrv->license);
	p->depends      = merge_strv (pkg->depends,      bstrv->depends);
	p->makedepends  = merge_strv (pkg->makedepends,  bstrv->makedepends);
	p->checkdepends = merge_strv (pkg->checkdepends, bstrv->checkdepends);
	p->optdepends   = merge_strv (pkg->optdepends,   bstrv->optdepends);
	p->conflicts    = merge_strv (pkg->conflicts,    bstrv->conflicts);
	p->provides     = merge_strv (pkg->provides,     bstrv->provides);
	p->replaces     = merge_strv (pkg->replaces,     bstrv->replaces);
	p->source       = merge_strv (pkg->source,       bstrv->source);
	p->groups       = merge_strv (pkg->groups,       bstrv->groups);

	return p;
}

AurSrcinfo *
aur_srcinfo_parse (const gchar *content, GError **error)
{
	g_auto(GStrv) lines = NULL;
	ParseSection *base;
	GPtrArray *pkg_sections;
	ParseSection *current;
	gchar *pkgbase_name = NULL;
	gboolean in_base = TRUE;
	guint i;
	BaseStrv bstrv;
	AurSrcinfo *si;

	g_return_val_if_fail (content != NULL, NULL);

	lines = g_strsplit (content, "\n", -1);
	base = parse_section_new ();
	pkg_sections = g_ptr_array_new_with_free_func ((GDestroyNotify) parse_section_free);
	current = base;

	for (i = 0; lines[i] != NULL; i++) {
		gchar *line = lines[i];
		gchar *stripped;
		gchar *eq;
		gchar *key;
		gchar *value;

		/* Skip empty lines and comments */
		stripped = g_strstrip (g_strdup (line));
		if (*stripped == '\0' || *stripped == '#') {
			g_free (stripped);
			continue;
		}

		/* Parse "key = value" */
		eq = strchr (stripped, '=');
		if (eq == NULL) {
			g_free (stripped);
			continue;  /* Malformed line, skip */
		}

		*eq = '\0';
		key = g_strstrip (stripped);
		value = g_strstrip (eq + 1);

		if (g_strcmp0 (key, "pkgbase") == 0) {
			pkgbase_name = g_strdup (value);
		} else if (g_strcmp0 (key, "pkgname") == 0) {
			/* New package section */
			if (in_base) {
				in_base = FALSE;
			}
			current = parse_section_new ();
			apply_kv (current, key, value);
			g_ptr_array_add (pkg_sections, current);
		} else {
			apply_kv (current, key, value);
		}

		g_free (stripped);
	}

	/* Validate */
	if (pkgbase_name == NULL) {
		g_set_error (error, AUR_SRCINFO_ERROR, AUR_SRCINFO_ERROR_MISSING,
					 ".SRCINFO missing pkgbase");
		parse_section_free (base);
		g_ptr_array_free (pkg_sections, TRUE);
		return NULL;
	}

	if (pkg_sections->len == 0) {
		g_set_error (error, AUR_SRCINFO_ERROR, AUR_SRCINFO_ERROR_MISSING,
					 ".SRCINFO missing pkgname");
		g_free (pkgbase_name);
		parse_section_free (base);
		g_ptr_array_free (pkg_sections, TRUE);
		return NULL;
	}

	/* Convert base arrays to strv once (consumes GPtrArrays, sets them NULL) */
	bstrv = base_to_strv (base);

	/* Build result */
	si = g_new0 (AurSrcinfo, 1);
	si->pkgbase = pkgbase_name;
	si->count = pkg_sections->len;
	si->packages = g_new0 (AurSrcinfoPackage *, si->count + 1);

	for (i = 0; i < pkg_sections->len; i++) {
		ParseSection *psec = g_ptr_array_index (pkg_sections, i);
		si->packages[i] = build_package (base, psec, &bstrv);
	}

	base_strv_free (&bstrv);
	parse_section_free (base);
	g_ptr_array_unref (pkg_sections); /* frees elements via destroy func */

	return si;
}

AurSrcinfoPackage *
aur_srcinfo_find_pkg (AurSrcinfo *si, const gchar *pkgname)
{
	guint i;

	g_return_val_if_fail (si != NULL && pkgname != NULL, NULL);
	for (i = 0; i < si->count; i++) {
		if (g_strcmp0 (si->packages[i]->pkgname, pkgname) == 0)
			return si->packages[i];
	}
	return NULL;
}

gchar *
aur_srcinfo_pkg_version (AurSrcinfoPackage *pkg)
{
	g_return_val_if_fail (pkg != NULL, NULL);
	if (pkg->epoch != NULL)
		return g_strdup_printf ("%s:%s-%s", pkg->epoch, pkg->pkgver, pkg->pkgrel);
	return g_strdup_printf ("%s-%s", pkg->pkgver, pkg->pkgrel);
}

static void
srcinfo_package_free (AurSrcinfoPackage *pkg)
{
	if (pkg == NULL) return;
	g_free (pkg->pkgname);
	g_free (pkg->pkgdesc);
	g_free (pkg->pkgver);
	g_free (pkg->pkgrel);
	g_free (pkg->epoch);
	g_free (pkg->url);
	g_strfreev (pkg->arch);
	g_strfreev (pkg->license);
	g_strfreev (pkg->depends);
	g_strfreev (pkg->makedepends);
	g_strfreev (pkg->checkdepends);
	g_strfreev (pkg->optdepends);
	g_strfreev (pkg->conflicts);
	g_strfreev (pkg->provides);
	g_strfreev (pkg->replaces);
	g_strfreev (pkg->source);
	g_strfreev (pkg->groups);
	g_free (pkg);
}

void
aur_srcinfo_free (AurSrcinfo *si)
{
	guint i;

	if (si == NULL) return;
	g_free (si->pkgbase);
	for (i = 0; i < si->count; i++)
		srcinfo_package_free (si->packages[i]);
	g_free (si->packages);
	g_free (si);
}
