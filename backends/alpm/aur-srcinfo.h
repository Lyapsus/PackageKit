/*
 * aur-srcinfo.h — .SRCINFO parser
 *
 * Parses the .SRCINFO format (plain text, no bash execution) from AUR git repos.
 * Safe alternative to sourcing PKGBUILDs — no code execution.
 *
 * Format:
 *   pkgbase = name
 *     key = value          (shared/base-level attrs)
 *   pkgname = name1
 *     key = value          (per-package overrides)
 *   pkgname = name2
 *     key = value
 */

#ifndef AUR_SRCINFO_H
#define AUR_SRCINFO_H

#include <glib.h>

/* A single package within a .SRCINFO (one pkgname section).
 * Inherits base-level values where not overridden. */
typedef struct {
	gchar    *pkgname;
	gchar    *pkgdesc;
	gchar    *pkgver;
	gchar    *pkgrel;
	gchar    *epoch;          /* NULL if not set */
	gchar    *url;
	gchar   **arch;           /* e.g. ["x86_64"] or ["x86_64", "aarch64"] or ["any"] */
	gchar   **license;
	gchar   **depends;
	gchar   **makedepends;
	gchar   **checkdepends;
	gchar   **optdepends;
	gchar   **conflicts;
	gchar   **provides;
	gchar   **replaces;
	gchar   **source;
	gchar   **groups;
} AurSrcinfoPackage;

/* Parsed .SRCINFO — one pkgbase with N packages */
typedef struct {
	gchar              *pkgbase;
	AurSrcinfoPackage **packages;  /* NULL-terminated array */
	guint               count;
} AurSrcinfo;

/* Parse .SRCINFO content string. Returns NULL on error. */
AurSrcinfo        *aur_srcinfo_parse     (const gchar *content, GError **error);

/* Find a specific pkgname within a parsed .SRCINFO. Returns NULL if not found.
 * Returned pointer is owned by the AurSrcinfo — do not free. */
AurSrcinfoPackage *aur_srcinfo_find_pkg  (AurSrcinfo *si, const gchar *pkgname);

/* Build full version string: [epoch:]pkgver-pkgrel. Caller frees. */
gchar             *aur_srcinfo_pkg_version (AurSrcinfoPackage *pkg);

void               aur_srcinfo_free      (AurSrcinfo *si);

#define AUR_SRCINFO_ERROR (aur_srcinfo_error_quark())
GQuark             aur_srcinfo_error_quark (void);

typedef enum {
	AUR_SRCINFO_ERROR_FORMAT,   /* malformed .SRCINFO */
	AUR_SRCINFO_ERROR_MISSING,  /* required field missing */
} AurSrcinfoError;

#endif /* AUR_SRCINFO_H */
