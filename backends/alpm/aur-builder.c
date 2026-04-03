/*
 * aur-builder.c — AUR package build pipeline implementation
 */

#define _GNU_SOURCE
#include "aur-builder.h"
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <pwd.h>
#include <grp.h>
#include <errno.h>
#include <poll.h>

#define AUR_GIT_BASE "https://aur.archlinux.org"

G_DEFINE_QUARK (aur-builder-error-quark, aur_builder_error)

struct _AurBuilder {
	gchar *clone_base_dir;
	AurBuilderProgressFunc progress_func;
	gpointer progress_data;
	AurBuilderCancelFunc cancel_func;
	gpointer cancel_data;
};

static void
emit_progress (AurBuilder *builder, const gchar *fmt, ...)
{
	va_list args;
	gchar *msg;

	if (builder->progress_func == NULL)
		return;
	va_start (args, fmt);
	msg = g_strdup_vprintf (fmt, args);
	va_end (args);
	builder->progress_func (msg, builder->progress_data);
	g_free (msg);
}

/* Run a command, capture combined stdout+stderr. Returns exit code. */
static gint
run_command (const gchar *const *argv, const gchar *workdir,
			 gchar **output, GError **error)
{
	gint exit_status = 0;
	gchar *std_out = NULL;
	gchar *std_err = NULL;

	GSpawnFlags flags = G_SPAWN_SEARCH_PATH;
	gboolean ok = g_spawn_sync (workdir, (gchar **) argv, NULL, flags,
								NULL, NULL, &std_out, &std_err,
								&exit_status, error);
	if (!ok) {
		g_free (std_out);
		g_free (std_err);
		return -1;
	}

	if (output != NULL) {
		if (std_err != NULL && *std_err != '\0') {
			gchar *combined = g_strconcat (std_out ? std_out : "", std_err, NULL);
			g_free (std_out);
			std_out = NULL;
			*output = combined;
		} else {
			*output = std_out;
			std_out = NULL;
		}
	}
	g_free (std_out);
	g_free (std_err);

	if (WIFEXITED (exit_status))
		return WEXITSTATUS (exit_status);
	return -1;
}

/* Chown a directory tree to the given uid (+ its primary gid).
 * Returns 0 on success, -1 if the user doesn't exist, or the chown exit code. */
static gint
chown_to_user (const gchar *path, uid_t uid)
{
	struct passwd *pw;
	const gchar *argv[] = { "chown", "-R", NULL, NULL, NULL };
	gchar uid_str[64];

	if (uid == 0)
		return 0;

	pw = getpwuid (uid);
	if (pw == NULL)
		return -1;

	g_snprintf (uid_str, sizeof (uid_str), "%u:%u",
				(unsigned) uid, (unsigned) pw->pw_gid);
	argv[2] = uid_str;
	argv[3] = path;
	return run_command (argv, NULL, NULL, NULL);
}

/* Drop privileges to build_uid in a forked child process.
 * Sets uid, gid, supplementary groups, and HOME/USER/LOGNAME.
 * Calls _exit(126) on any failure — never returns on error. */
static void
drop_privileges (uid_t build_uid)
{
	struct passwd *pw;

	if (build_uid == 0 || geteuid () != 0)
		return;

	pw = getpwuid (build_uid);
	if (pw == NULL)
		_exit (126);

	setenv ("HOME", pw->pw_dir, 1);
	setenv ("USER", pw->pw_name, 1);
	setenv ("LOGNAME", pw->pw_name, 1);

	if (setgid (pw->pw_gid) != 0) _exit (126);
	if (initgroups (pw->pw_name, pw->pw_gid) != 0) _exit (126);
	if (setuid (build_uid) != 0) _exit (126);
}

/* Find built package files in a clone dir (forward decl — used by build). */
static gchar **aur_builder_find_packages (AurBuilder *builder,
										  const gchar *pkgbase);

/* --- Public API --- */

AurBuilder *
aur_builder_new (const gchar *clone_base_dir)
{
	AurBuilder *builder = g_new0 (AurBuilder, 1);
	if (clone_base_dir != NULL) {
		builder->clone_base_dir = g_strdup (clone_base_dir);
	} else {
		/* Default: /var/cache/pk-aur/clone/ — PK daemon runs as root,
		 * this is a system-level cache. Build user access is handled
		 * by chown before makepkg runs. */
		builder->clone_base_dir = g_strdup ("/var/cache/pk-aur/clone");
	}
	return builder;
}

void
aur_builder_free (AurBuilder *builder)
{
	if (builder == NULL) return;
	g_free (builder->clone_base_dir);
	g_free (builder);
}

void
aur_builder_set_progress (AurBuilder *builder, AurBuilderProgressFunc func, gpointer user_data)
{
	builder->progress_func = func;
	builder->progress_data = user_data;
}

void
aur_builder_set_cancel (AurBuilder *builder, AurBuilderCancelFunc func, gpointer user_data)
{
	builder->cancel_func = func;
	builder->cancel_data = user_data;
}

gchar *
aur_builder_clone (AurBuilder *builder, const gchar *pkgbase, GError **error)
{
	gchar *clone_dir;
	gchar *git_dir;
	gchar *output = NULL;
	gint ret;
	gchar *srcinfo_check;
	gboolean srcinfo_exists;
	const gchar *git_base;

	g_return_val_if_fail (builder != NULL && pkgbase != NULL, NULL);

	clone_dir = g_build_filename (builder->clone_base_dir, pkgbase, NULL);
	git_dir = g_build_filename (clone_dir, ".git", NULL);

	/* Ensure parent dir exists */
	if (g_mkdir_with_parents (builder->clone_base_dir, 0755) != 0) {
		g_set_error (error, AUR_BUILDER_ERROR, AUR_BUILDER_ERROR_IO,
					 "cannot create %s: %s", builder->clone_base_dir, g_strerror (errno));
		g_free (clone_dir);
		g_free (git_dir);
		return NULL;
	}

	if (g_file_test (git_dir, G_FILE_TEST_IS_DIR)) {
		/* Already cloned — clean build artifacts then pull.
		 * -c safe.directory=* allows git on dirs owned by other users
		 * (daemon=root, clone=builduser) without process-global env vars. */
		const gchar *clean_argv[] = {
			"git", "-c", "safe.directory=*", "-C", clone_dir,
			"clean", "-fdx", NULL
		};
		const gchar *reset_argv[] = {
			"git", "-c", "safe.directory=*", "-C", clone_dir,
			"checkout", "--", ".", NULL
		};
		const gchar *argv[] = {
			"git", "-c", "safe.directory=*", "-C", clone_dir,
			"pull", "--ff-only", NULL
		};
		run_command (clean_argv, NULL, NULL, NULL);
		run_command (reset_argv, NULL, NULL, NULL);
		emit_progress (builder, "updating %s", pkgbase);
		ret = run_command (argv, NULL, &output, error);
	} else {
		/* Fresh clone with retry on transient network failures */
		gchar *url;
		const gchar *argv[8];
		guint attempt;

		emit_progress (builder, "cloning %s", pkgbase);

		git_base = g_getenv ("AUR_GIT_BASE");
		if (git_base == NULL)
			git_base = AUR_GIT_BASE;
		url = g_strdup_printf ("%s/%s.git", git_base, pkgbase);
		argv[0] = "git"; argv[1] = "-c"; argv[2] = "safe.directory=*";
		argv[3] = "clone"; argv[4] = "--depth=1";
		argv[5] = url; argv[6] = clone_dir; argv[7] = NULL;

		for (attempt = 0; attempt < 3; attempt++) {
			/* Clean up any partial clone from previous attempt */
			if (g_file_test (clone_dir, G_FILE_TEST_EXISTS)) {
				const gchar *rm_argv[] = {
					"rm", "-rf", clone_dir, NULL
				};
				run_command (rm_argv, NULL, NULL, NULL);
			}

			g_free (output);
			output = NULL;
			g_clear_error (error);
			ret = run_command (argv, NULL, &output, error);
			if (ret == 0)
				break;
			if (attempt + 1 < 3)
				g_usleep (500000 << attempt); /* 500ms, 1s */
		}
		g_free (url);
	}

	g_free (git_dir);

	if (ret != 0) {
		if (error != NULL && *error == NULL)
			g_set_error (error, AUR_BUILDER_ERROR, AUR_BUILDER_ERROR_CLONE,
						 "git failed (exit %d): %s", ret, output ? output : "");
		g_free (output);
		g_free (clone_dir);
		return NULL;
	}

	g_free (output);

	/* AUR returns empty repos for nonexistent packages — verify .SRCINFO exists */
	srcinfo_check = g_build_filename (clone_dir, ".SRCINFO", NULL);
	srcinfo_exists = g_file_test (srcinfo_check, G_FILE_TEST_IS_REGULAR);
	g_free (srcinfo_check);
	if (!srcinfo_exists) {
		const gchar *rm_argv[] = { "rm", "-rf", clone_dir, NULL };
		g_set_error (error, AUR_BUILDER_ERROR, AUR_BUILDER_ERROR_CLONE,
					 "AUR package '%s' does not exist (empty repo)", pkgbase);
		/* Clean up the empty clone */
		run_command (rm_argv, NULL, NULL, NULL);
		g_free (clone_dir);
		return NULL;
	}

	emit_progress (builder, "cloned %s -> %s", pkgbase, clone_dir);
	return clone_dir;
}

AurSrcinfo *
aur_builder_read_srcinfo (AurBuilder *builder, const gchar *pkgbase, GError **error)
{
	gchar *srcinfo_path;
	gchar *content = NULL;
	AurSrcinfo *si;

	g_return_val_if_fail (builder != NULL && pkgbase != NULL, NULL);

	srcinfo_path = g_build_filename (
		builder->clone_base_dir, pkgbase, ".SRCINFO", NULL);

	if (!g_file_get_contents (srcinfo_path, &content, NULL, error)) {
		g_prefix_error (error, ".SRCINFO read: ");
		g_free (srcinfo_path);
		return NULL;
	}
	g_free (srcinfo_path);

	si = aur_srcinfo_parse (content, error);
	g_free (content);
	return si;
}

AurBuildResult *
aur_builder_build (AurBuilder *builder, const gchar *pkgbase,
				   uid_t build_uid, GError **error)
{
	gchar *clone_dir;
	gint pipe_fd[2];
	pid_t pid;
	GString *log;
	gchar buf[4096];
	gssize n;
	gint status;
	gint exit_code;
	gboolean cancelled;
	AurBuildResult *result;

	g_return_val_if_fail (builder != NULL && pkgbase != NULL, NULL);

	clone_dir = g_build_filename (
		builder->clone_base_dir, pkgbase, NULL);

	if (!g_file_test (clone_dir, G_FILE_TEST_IS_DIR)) {
		g_set_error (error, AUR_BUILDER_ERROR, AUR_BUILDER_ERROR_IO,
					 "not cloned: %s — call aur_builder_clone first", pkgbase);
		g_free (clone_dir);
		return NULL;
	}

	/* Chown the clone dir to build user so makepkg can write there.
	 * We're root (PK daemon), build_uid is the GUI user. */
	chown_to_user (clone_dir, build_uid);

	emit_progress (builder, "building %s", pkgbase);

	if (pipe (pipe_fd) != 0) {
		g_set_error (error, AUR_BUILDER_ERROR, AUR_BUILDER_ERROR_IO,
					 "pipe: %s", g_strerror (errno));
		g_free (clone_dir);
		return NULL;
	}

	pid = fork ();
	if (pid < 0) {
		g_set_error (error, AUR_BUILDER_ERROR, AUR_BUILDER_ERROR_IO,
					 "fork: %s", g_strerror (errno));
		close (pipe_fd[0]);
		close (pipe_fd[1]);
		g_free (clone_dir);
		return NULL;
	}

	if (pid == 0) {
		/* Child: drop privileges and exec makepkg */
		close (pipe_fd[0]); /* close read end */

		/* Redirect stdout+stderr to pipe */
		dup2 (pipe_fd[1], STDOUT_FILENO);
		dup2 (pipe_fd[1], STDERR_FILENO);
		close (pipe_fd[1]);

		drop_privileges (build_uid);

		if (chdir (clone_dir) != 0)
			_exit (127);

		/* makepkg flags:
		 * -c: clean up work dirs after build
		 * --noconfirm: don't prompt
		 * --needed: skip if already installed
		 * NOT -s: deps are pre-installed by the PK hook (avoids pacman db lock
		 *         conflict — PK daemon holds the lock during transactions)
		 * NOT -r: makedep removal handled by caller
		 * NOT -i: PK backend handles install via libalpm */
		execlp ("makepkg", "makepkg", "-c", "--noconfirm", "--needed", NULL);
		_exit (127); /* exec failed */
	}

	/* Parent: clone_dir no longer needed (child used it before exec) */
	g_free (clone_dir);

	/* Parent: read build output with periodic cancel checks */
	close (pipe_fd[1]); /* close write end */

	log = g_string_new (NULL);
	cancelled = FALSE;
	{
		struct pollfd pfd;
		int ready;

		pfd.fd = pipe_fd[0];
		pfd.events = POLLIN;

		for (;;) {
			ready = poll (&pfd, 1, 1000); /* 1s timeout */
			if (ready < 0) {
				if (errno == EINTR)
					continue;
				break;
			}
			if (ready == 0) {
				/* Timeout — check cancellation */
				if (builder->cancel_func != NULL &&
					builder->cancel_func (builder->cancel_data)) {
					kill (pid, SIGTERM);
					cancelled = TRUE;
					break;
				}
				continue;
			}
			n = read (pipe_fd[0], buf, sizeof (buf) - 1);
			if (n <= 0)
				break;
			buf[n] = '\0';
			g_string_append (log, buf);
		}
	}
	close (pipe_fd[0]);

	waitpid (pid, &status, 0);

	if (cancelled) {
		g_set_error (error, AUR_BUILDER_ERROR, AUR_BUILDER_ERROR_BUILD,
					 "build of %s cancelled", pkgbase);
		g_string_free (log, TRUE);
		return NULL;
	}

	exit_code = WIFEXITED (status) ? WEXITSTATUS (status) : -1;

	result = g_new0 (AurBuildResult, 1);
	result->exit_code = exit_code;
	result->build_log = g_string_free (log, FALSE);

	if (exit_code == 126) {
		g_set_error (error, AUR_BUILDER_ERROR, AUR_BUILDER_ERROR_PRIVILEGE,
					 "cannot drop privileges to uid %u", (unsigned) build_uid);
		aur_build_result_free (result);
		return NULL;
	}

	if (exit_code != 0) {
		g_set_error (error, AUR_BUILDER_ERROR, AUR_BUILDER_ERROR_BUILD,
					 "makepkg failed (exit %d)", exit_code);
		/* Return the result anyway so caller can inspect the log */
		return result;
	}

	/* Collect built packages */
	result->package_paths = aur_builder_find_packages (builder, pkgbase);
	if (result->package_paths != NULL) {
		guint c = 0;
		gchar **p;
		for (p = result->package_paths; *p != NULL; p++)
			c++;
		result->count = c;
	}

	emit_progress (builder, "built %s: %u package(s)", pkgbase, result->count);
	return result;
}

AurSrcinfo *
aur_builder_fresh_srcinfo (AurBuilder *builder, const gchar *pkgbase,
						   uid_t build_uid, GError **error)
{
	gchar *clone_dir;
	gint pipe_fd[2];
	pid_t pid;
	GString *output;
	gchar buf[4096];
	gssize n;
	gint status;
	gboolean timed_out;
	AurSrcinfo *si;

	g_return_val_if_fail (builder != NULL && pkgbase != NULL, NULL);

	clone_dir = g_build_filename (builder->clone_base_dir, pkgbase, NULL);

	if (!g_file_test (clone_dir, G_FILE_TEST_IS_DIR)) {
		g_set_error (error, AUR_BUILDER_ERROR, AUR_BUILDER_ERROR_IO,
					 "not cloned: %s", pkgbase);
		g_free (clone_dir);
		return NULL;
	}

	/* Chown to build user so makepkg can write */
	chown_to_user (clone_dir, build_uid);

	if (pipe (pipe_fd) != 0) {
		g_set_error (error, AUR_BUILDER_ERROR, AUR_BUILDER_ERROR_IO,
					 "pipe: %s", g_strerror (errno));
		g_free (clone_dir);
		return NULL;
	}

	pid = fork ();
	if (pid < 0) {
		g_set_error (error, AUR_BUILDER_ERROR, AUR_BUILDER_ERROR_IO,
					 "fork: %s", g_strerror (errno));
		close (pipe_fd[0]);
		close (pipe_fd[1]);
		g_free (clone_dir);
		return NULL;
	}

	if (pid == 0) {
		/* Child: drop privs, fetch sources, print srcinfo */
		close (pipe_fd[0]);
		dup2 (pipe_fd[1], STDOUT_FILENO);
		close (pipe_fd[1]);

		drop_privileges (build_uid);

		if (chdir (clone_dir) != 0) _exit (127);

		/* Fetch sources (for pkgver), then print srcinfo to stdout.
		 * -o: download+extract only, -d: skip dep checks.
		 * Source fetch output goes to /dev/null; only --printsrcinfo
		 * output reaches stdout (the pipe). */
		execlp ("sh", "sh", "-c",
				"makepkg -od --noconfirm >/dev/null 2>&1 && "
				"makepkg --printsrcinfo 2>/dev/null",
				NULL);
		_exit (127);
	}

	g_free (clone_dir);

	/* Parent: read srcinfo output with 30s timeout */
	close (pipe_fd[1]);
	output = g_string_new (NULL);
	timed_out = FALSE;
	{
		struct pollfd pfd;
		int ready;
		gint64 deadline;

		pfd.fd = pipe_fd[0];
		pfd.events = POLLIN;
		deadline = g_get_monotonic_time () + 30 * G_USEC_PER_SEC;

		for (;;) {
			gint64 remaining = deadline - g_get_monotonic_time ();
			if (remaining <= 0) {
				kill (pid, SIGTERM);
				timed_out = TRUE;
				break;
			}
			ready = poll (&pfd, 1, (int)(remaining / 1000));
			if (ready < 0) {
				if (errno == EINTR)
					continue;
				break;
			}
			if (ready == 0) {
				kill (pid, SIGTERM);
				timed_out = TRUE;
				break;
			}
			n = read (pipe_fd[0], buf, sizeof (buf) - 1);
			if (n <= 0)
				break;
			buf[n] = '\0';
			g_string_append (output, buf);
		}
	}
	close (pipe_fd[0]);

	waitpid (pid, &status, 0);

	if (timed_out) {
		g_set_error (error, AUR_BUILDER_ERROR, AUR_BUILDER_ERROR_SRCINFO,
					 "timeout fetching sources for %s", pkgbase);
		g_string_free (output, TRUE);
		return NULL;
	}

	if (!WIFEXITED (status) || WEXITSTATUS (status) != 0 || output->len == 0) {
		g_set_error (error, AUR_BUILDER_ERROR, AUR_BUILDER_ERROR_SRCINFO,
					 "makepkg --printsrcinfo failed for %s", pkgbase);
		g_string_free (output, TRUE);
		return NULL;
	}

	si = aur_srcinfo_parse (output->str, error);
	g_string_free (output, TRUE);
	return si;
}

static gchar **
aur_builder_find_packages (AurBuilder *builder, const gchar *pkgbase)
{
	gchar *clone_dir;
	GDir *dir;
	GPtrArray *paths;
	const gchar *name;

	g_return_val_if_fail (builder != NULL && pkgbase != NULL, NULL);

	clone_dir = g_build_filename (
		builder->clone_base_dir, pkgbase, NULL);

	dir = g_dir_open (clone_dir, 0, NULL);
	if (dir == NULL) {
		g_free (clone_dir);
		return NULL;
	}

	paths = g_ptr_array_new ();
	while ((name = g_dir_read_name (dir)) != NULL) {
		if (g_str_has_suffix (name, ".pkg.tar.zst") ||
			g_str_has_suffix (name, ".pkg.tar.xz") ||
			g_str_has_suffix (name, ".pkg.tar.gz")) {
			g_ptr_array_add (paths, g_build_filename (clone_dir, name, NULL));
		}
	}
	g_dir_close (dir);
	g_free (clone_dir);

	if (paths->len == 0) {
		g_ptr_array_unref (paths);
		return NULL;
	}

	g_ptr_array_add (paths, NULL);
	return (gchar **) g_ptr_array_free (paths, FALSE);
}

void
aur_build_result_free (AurBuildResult *result)
{
	if (result == NULL) return;
	g_strfreev (result->package_paths);
	g_free (result->build_log);
	g_free (result);
}
