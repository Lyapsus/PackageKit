/*
 * aur-builder.h — AUR package build pipeline
 *
 * Handles: git clone/update, .SRCINFO reading, makepkg invocation with
 * privilege dropping, built package path collection.
 *
 * Thread-safe per-handle. Caller is expected to be root (PackageKit daemon).
 * Build operations fork+setuid to the specified UID for makepkg.
 */

#ifndef AUR_BUILDER_H
#define AUR_BUILDER_H

#include <glib.h>
#include <sys/types.h>
#include "aur-srcinfo.h"

/* Opaque builder handle */
typedef struct _AurBuilder AurBuilder;

/* Build result — collected after successful makepkg */
typedef struct {
	gchar   **package_paths;   /* NULL-terminated array of .pkg.tar.zst paths */
	guint     count;
	gchar    *build_log;       /* combined stdout+stderr from makepkg, or NULL */
	gint      exit_code;       /* makepkg exit code */
} AurBuildResult;

/* Progress callback — called during clone/build with status messages */
typedef void (*AurBuilderProgressFunc) (const gchar *status_msg, gpointer user_data);

/* Cancel callback — return TRUE if build should be cancelled */
typedef gboolean (*AurBuilderCancelFunc) (gpointer user_data);

/* Create builder. clone_base_dir: parent for per-pkgbase clone dirs.
 * NULL defaults to ~/.cache/pk-aur/clone/ resolved for the build user. */
AurBuilder    *aur_builder_new           (const gchar *clone_base_dir);
void           aur_builder_free          (AurBuilder *builder);

/* Set progress callback */
void           aur_builder_set_progress  (AurBuilder *builder,
										  AurBuilderProgressFunc func,
										  gpointer user_data);

/* Set cancel callback — checked periodically during build */
void           aur_builder_set_cancel    (AurBuilder *builder,
										  AurBuilderCancelFunc func,
										  gpointer user_data);

/* Clone or update the AUR git repo for a pkgbase.
 * Returns path to clone directory, or NULL on error. Caller frees. */
gchar         *aur_builder_clone         (AurBuilder *builder,
										  const gchar *pkgbase,
										  GError **error);

/* Read .SRCINFO from an already-cloned pkgbase.
 * Returns NULL on error. Caller frees with aur_srcinfo_free(). */
AurSrcinfo    *aur_builder_read_srcinfo  (AurBuilder *builder,
										  const gchar *pkgbase,
										  GError **error);

/* Build the package. Forks, drops to build_uid, runs makepkg.
 * Caller must be root (or have CAP_SETUID).
 * If build_uid == 0, uses the current user (for testing without root).
 * Returns NULL on error. Caller frees with aur_build_result_free(). */
AurBuildResult *aur_builder_build        (AurBuilder *builder,
										  const gchar *pkgbase,
										  uid_t build_uid,
										  GError **error);

/* Get fresh .SRCINFO with pkgver() applied (for VCS packages).
 * Fetches sources via `makepkg -od`, then runs `makepkg --printsrcinfo`.
 * Requires a cloned AUR repo. build_uid: user to run makepkg as (non-root).
 * 30-second timeout for source fetching. Returns NULL on error/timeout. */
AurSrcinfo    *aur_builder_fresh_srcinfo (AurBuilder *builder,
										  const gchar *pkgbase,
										  uid_t build_uid,
										  GError **error);

void           aur_build_result_free     (AurBuildResult *result);

#define AUR_BUILDER_ERROR (aur_builder_error_quark())
GQuark         aur_builder_error_quark   (void);

typedef enum {
	AUR_BUILDER_ERROR_CLONE,     /* git clone/pull failed */
	AUR_BUILDER_ERROR_SRCINFO,   /* .SRCINFO missing or unparseable */
	AUR_BUILDER_ERROR_BUILD,     /* makepkg failed */
	AUR_BUILDER_ERROR_PRIVILEGE, /* cannot drop privileges */
	AUR_BUILDER_ERROR_IO,        /* filesystem error */
} AurBuilderError;

#endif /* AUR_BUILDER_H */
