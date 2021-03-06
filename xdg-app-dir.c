#include "config.h"

#include <string.h>
#include <fcntl.h>
#include <stdio.h>

#include <gio/gio.h>
#include "libgsystem.h"

#include "xdg-app-dir.h"
#include "xdg-app-utils.h"

#include "errno.h"

struct XdgAppDir {
  GObject parent;

  gboolean user;
  GFile *basedir;
  OstreeRepo *repo;
};

typedef struct {
  GObjectClass parent_class;
} XdgAppDirClass;

G_DEFINE_TYPE (XdgAppDir, xdg_app_dir, G_TYPE_OBJECT)

G_DEFINE_QUARK (xdg-app-dir-error-quark, xdg_app_dir_error)

enum {
  PROP_0,

  PROP_USER,
  PROP_PATH
};

#define OSTREE_GIO_FAST_QUERYINFO ("standard::name,standard::type,standard::size,standard::is-symlink,standard::symlink-target," \
                                   "unix::device,unix::inode,unix::mode,unix::uid,unix::gid,unix::rdev")

GFile *
xdg_app_get_system_base_dir_location (void)
{
  return g_file_new_for_path (XDG_APP_SYSTEMDIR);
}

GFile *
xdg_app_get_user_base_dir_location (void)
{
  gs_free char *base = g_build_filename (g_get_user_data_dir (), "xdg-app", NULL);
  return g_file_new_for_path (base);
}

static void
xdg_app_dir_finalize (GObject *object)
{
  XdgAppDir *self = XDG_APP_DIR (object);

  g_clear_object (&self->repo);
  g_clear_object (&self->basedir);

  G_OBJECT_CLASS (xdg_app_dir_parent_class)->finalize (object);
}

static void
xdg_app_dir_set_property(GObject         *object,
                         guint            prop_id,
                         const GValue    *value,
                         GParamSpec      *pspec)
{
  XdgAppDir *self = XDG_APP_DIR (object);

  switch (prop_id)
    {
    case PROP_PATH:
      /* Canonicalize */
      self->basedir = g_file_new_for_path (gs_file_get_path_cached (g_value_get_object (value)));
      break;
    case PROP_USER:
      self->user = g_value_get_boolean (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
xdg_app_dir_get_property(GObject         *object,
                         guint            prop_id,
                         GValue          *value,
                         GParamSpec      *pspec)
{
  XdgAppDir *self = XDG_APP_DIR (object);

  switch (prop_id)
    {
    case PROP_PATH:
      g_value_set_object (value, self->basedir);
      break;
    case PROP_USER:
      g_value_set_boolean (value, self->user);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
xdg_app_dir_class_init (XdgAppDirClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->get_property = xdg_app_dir_get_property;
  object_class->set_property = xdg_app_dir_set_property;
  object_class->finalize = xdg_app_dir_finalize;

  g_object_class_install_property (object_class,
                                   PROP_USER,
                                   g_param_spec_boolean ("user",
                                                         "",
                                                         "",
                                                         FALSE,
                                                         G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));
  g_object_class_install_property (object_class,
                                   PROP_PATH,
                                   g_param_spec_object ("path",
                                                        "",
                                                        "",
                                                        G_TYPE_FILE,
                                                        G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));
}

static void
xdg_app_dir_init (XdgAppDir *self)
{
}

gboolean
xdg_app_dir_is_user (XdgAppDir *self)
{
  return self->user;
}

GFile *
xdg_app_dir_get_path (XdgAppDir *self)
{
  return self->basedir;
}

GFile *
xdg_app_dir_get_deploy_dir (XdgAppDir     *self,
                            const char    *ref)
{
  return g_file_resolve_relative_path (self->basedir, ref);
}

GFile *
xdg_app_dir_get_exports_dir (XdgAppDir     *self)
{
  return g_file_get_child (self->basedir, "exports");
}

GFile *
xdg_app_dir_get_removed_dir (XdgAppDir     *self)
{
  return g_file_get_child (self->basedir, ".removed");
}

GFile *
xdg_app_dir_get_app_data (XdgAppDir     *self,
                          const char    *app)
{
  gs_free char *partial_ref = NULL;

  partial_ref = g_build_filename ("app", app, "data", NULL);
  return g_file_resolve_relative_path (self->basedir, partial_ref);
}

OstreeRepo *
xdg_app_dir_get_repo (XdgAppDir *self)
{
  return self->repo;
}

gboolean
xdg_app_dir_ensure_path (XdgAppDir     *self,
                         GCancellable  *cancellable,
                         GError       **error)
{
  return gs_file_ensure_directory (self->basedir, TRUE, cancellable, error);
}

gboolean
xdg_app_dir_ensure_repo (XdgAppDir *self,
                         GCancellable *cancellable,
                         GError **error)
{
  gboolean ret = FALSE;
  gs_unref_object GFile *repodir = NULL;
  gs_unref_object OstreeRepo *repo = NULL;

  if (self->repo == NULL)
    {
      if (!xdg_app_dir_ensure_path (self, cancellable, error))
        goto out;

      repodir = g_file_get_child (self->basedir, "repo");
      repo = ostree_repo_new (repodir);

      if (!g_file_query_exists (repodir, cancellable))
        {
          if (!ostree_repo_create (repo,
                                   self->user ? OSTREE_REPO_MODE_BARE_USER : OSTREE_REPO_MODE_BARE,
                                   cancellable, error))
            {
              gs_shutil_rm_rf (repodir, cancellable, NULL);
              goto out;
            }
        }
      else
        {
          if (!ostree_repo_open (repo, cancellable, error))
            {
              gs_free char *repopath = NULL;

              repopath = g_file_get_path (repodir);
              g_prefix_error (error, "While opening repository %s: ", repopath);
              goto out;
            }
        }

      self->repo = g_object_ref (repo);
    }

  ret = TRUE;
 out:
  return ret;
}

gboolean
xdg_app_dir_pull (XdgAppDir *self,
                  const char *repository,
                  const char *ref,
                  GCancellable *cancellable,
                  GError **error)
{
  gboolean ret = FALSE;
  GSConsole *console = NULL;
  gs_unref_object OstreeAsyncProgress *progress = NULL;
  const char *refs[2];

  if (!xdg_app_dir_ensure_repo (self, cancellable, error))
    goto out;

  console = gs_console_get ();
  if (console)
    {
      gs_console_begin_status_line (console, "", NULL, NULL);
      progress = ostree_async_progress_new_and_connect (ostree_repo_pull_default_console_progress_changed, console);
    }

  refs[0] = ref;
  refs[1] = NULL;
  if (!ostree_repo_pull (self->repo, repository,
                         (char **)refs, OSTREE_REPO_PULL_FLAGS_NONE,
                         progress,
                         cancellable, error))
    {
      g_prefix_error (error, "While pulling %s from remote %s: ", ref, repository);
      goto out;
    }

  if (console)
    gs_console_end_status_line (console, NULL, NULL);

  ret = TRUE;
 out:
  return ret;
}

char *
xdg_app_dir_read_active (XdgAppDir *self,
                         const char *ref,
                         GCancellable *cancellable)
{
  gs_unref_object GFile *deploy_base = NULL;
  gs_unref_object GFile *active_link = NULL;
  gs_unref_object GFileInfo *file_info = NULL;

  deploy_base = xdg_app_dir_get_deploy_dir (self, ref);
  active_link = g_file_get_child (deploy_base, "active");

  file_info = g_file_query_info (active_link, OSTREE_GIO_FAST_QUERYINFO,
                                 G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                 cancellable, NULL);
  if (file_info == NULL)
    return NULL;

  return g_strdup (g_file_info_get_symlink_target (file_info));
}

gboolean
xdg_app_dir_set_active (XdgAppDir *self,
                        const char *ref,
                        const char *checksum,
                        GCancellable *cancellable,
                        GError **error)
{
  gboolean ret = FALSE;
  gs_unref_object GFile *deploy_base = NULL;
  gs_free char *tmpname = NULL;
  gs_unref_object GFile *active_tmp_link = NULL;
  gs_unref_object GFile *active_link = NULL;
  gs_free_error GError *my_error = NULL;

  deploy_base = xdg_app_dir_get_deploy_dir (self, ref);
  active_link = g_file_get_child (deploy_base, "active");

  if (checksum != NULL)
    {
      tmpname = gs_fileutil_gen_tmp_name (".active-", NULL);
      active_tmp_link = g_file_get_child (deploy_base, tmpname);
      if (!g_file_make_symbolic_link (active_tmp_link, checksum, cancellable, error))
        goto out;

      if (!gs_file_rename (active_tmp_link,
                           active_link,
                           cancellable, error))
        goto out;
    }
  else
    {
      if (!g_file_delete (active_link, cancellable, &my_error) &&
          !g_error_matches (my_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        {
          g_propagate_error (error, my_error);
          my_error = NULL;
          goto out;
        }
    }

  ret = TRUE;
 out:
  return ret;
}


gboolean
xdg_app_dir_run_triggers (XdgAppDir *self,
			  GCancellable *cancellable,
			  GError **error)
{
  gboolean ret = FALSE;
  gs_unref_object GFileEnumerator *dir_enum = NULL;
  gs_unref_object GFileInfo *child_info = NULL;
  gs_unref_object GFile *triggersdir = NULL;
  gs_unref_object GFile *exports = NULL;
  GError *temp_error = NULL;

  g_debug ("running triggers");

  exports = xdg_app_dir_get_exports_dir (self);

  triggersdir = g_file_new_for_path (XDG_APP_TRIGGERDIR);

  dir_enum = g_file_enumerate_children (triggersdir, "standard::type,standard::name",
                                        0, cancellable, error);
  if (!dir_enum)
    goto out;

  while ((child_info = g_file_enumerator_next_file (dir_enum, cancellable, &temp_error)) != NULL)
    {
      gs_unref_object GFile *child = NULL;
      const char *name;
      GError *trigger_error = NULL;

      name = g_file_info_get_name (child_info);

      child = g_file_get_child (triggersdir, name);

      if (g_file_info_get_file_type (child_info) == G_FILE_TYPE_REGULAR &&
	  g_str_has_suffix (name, ".trigger"))
	{
	  gs_unref_ptrarray GPtrArray *argv_array = NULL;

	  g_debug ("running trigger %s", name);

	  argv_array = g_ptr_array_new_with_free_func (g_free);
	  g_ptr_array_add (argv_array, g_strdup (HELPER));
	  g_ptr_array_add (argv_array, g_strdup ("-a"));
	  g_ptr_array_add (argv_array, g_file_get_path (self->basedir));
	  g_ptr_array_add (argv_array, g_strdup ("-e"));
	  g_ptr_array_add (argv_array, g_strdup ("-F"));
	  g_ptr_array_add (argv_array, g_strdup ("/usr"));
	  g_ptr_array_add (argv_array, g_file_get_path (child));
	  g_ptr_array_add (argv_array, NULL);

	  if (!g_spawn_sync ("/",
			     (char **)argv_array->pdata,
			     NULL,
			     G_SPAWN_DEFAULT,
			     NULL, NULL,
			     NULL, NULL,
			     NULL, &trigger_error))
	    {
	      g_warning ("Error running trigger %s: %s", name, trigger_error->message);
	      g_clear_error (&trigger_error);
	    }
	}

      g_clear_object (&child_info);
    }

  if (temp_error != NULL)
    {
      g_propagate_error (error, temp_error);
      goto out;
    }

  ret = TRUE;
 out:
  return ret;
}

static gboolean
read_fd (int           fd,
         struct stat  *stat_buf,
         gchar       **contents,
         gsize        *length,
         GError      **error)
{
  gchar *buf;
  gsize bytes_read;
  gsize size;
  gsize alloc_size;

  size = stat_buf->st_size;

  alloc_size = size + 1;
  buf = g_try_malloc (alloc_size);

  if (buf == NULL)
    {
      g_set_error (error,
                   G_FILE_ERROR,
                   G_FILE_ERROR_NOMEM,
                   "not enough memory");
      return FALSE;
    }

  bytes_read = 0;
  while (bytes_read < size)
    {
      gssize rc;

      rc = read (fd, buf + bytes_read, size - bytes_read);

      if (rc < 0)
        {
          if (errno != EINTR)
            {
              int save_errno = errno;

              g_free (buf);
              g_set_error (error,
                           G_FILE_ERROR,
                           g_file_error_from_errno (save_errno),
                           "Failed to read from exported file");
              return FALSE;
            }
        }
      else if (rc == 0)
        break;
      else
        bytes_read += rc;
    }

  buf[bytes_read] = '\0';

  if (length)
    *length = bytes_read;

  *contents = buf;

  return TRUE;
}

static gboolean
export_desktop_file (const char    *app,
                     const char    *branch,
                     const char    *arch,
                     int            parent_fd,
                     const char    *name,
                     struct stat   *stat_buf,
                     char         **target,
                     GCancellable  *cancellable,
                     GError       **error)
{
  gboolean ret = FALSE;
  gs_fd_close int desktop_fd = -1;
  gs_free char *tmpfile_name = NULL;
  gs_unref_object GOutputStream *out_stream = NULL;
  gs_free gchar *data = NULL;
  gsize data_len;
  gs_free gchar *new_data = NULL;
  gsize new_data_len;
  gs_unref_keyfile GKeyFile *keyfile = NULL;
  gs_free gchar *old_exec = NULL;
  gint old_argc;
  gs_strfreev gchar **old_argv = NULL;
  gs_strfreev gchar **groups = NULL;
  GString *new_exec = NULL;
  gs_free char *escaped_app = g_shell_quote (app);
  int i;

  if (!gs_file_openat_noatime (parent_fd, name, &desktop_fd, cancellable, error))
    goto out;

  if (!read_fd (desktop_fd, stat_buf, &data, &data_len, error))
    goto out;

  keyfile = g_key_file_new ();
  if (!g_key_file_load_from_data (keyfile, data, data_len, G_KEY_FILE_KEEP_TRANSLATIONS, error))
    goto out;

  if (g_str_has_suffix (name, ".service"))
    {
      gs_free gchar *dbus_name = NULL;
      gs_free gchar *expected_dbus_name = g_strndup (name, strlen (name) - strlen (".service"));

      dbus_name = g_key_file_get_string (keyfile, "D-BUS Service", "Name", NULL);

      if (dbus_name == NULL || strcmp (dbus_name, expected_dbus_name) != 0)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "dbus service file %s has wrong name", name);
          return FALSE;
        }
    }

  groups = g_key_file_get_groups (keyfile, NULL);

  for (i = 0; groups[i] != NULL; i++)
    {
      g_key_file_remove_key (keyfile, groups[i], "TryExec", NULL);

      /* Remove this to make sure nothing tries to execute it outside the sandbox*/
      g_key_file_remove_key (keyfile, groups[i], "X-GNOME-Bugzilla-ExtraInfoScript", NULL);

      new_exec = g_string_new ("");
      g_string_append_printf (new_exec, XDG_APP_BINDIR"/xdg-app run --branch='%s' --arch='%s'", branch, arch);

      old_exec = g_key_file_get_string (keyfile, groups[i], "Exec", NULL);
      if (old_exec && g_shell_parse_argv (old_exec, &old_argc, &old_argv, NULL) && old_argc >= 1)
        {
          int i;
          gs_free char *command = g_shell_quote (old_argv[0]);

          g_string_append_printf (new_exec, " --command=%s", command);

          g_string_append (new_exec, " ");
          g_string_append (new_exec, escaped_app);

          for (i = 1; i < old_argc; i++)
            {
              gs_free char *arg = g_shell_quote (old_argv[i]);
              g_string_append (new_exec, " ");
              g_string_append (new_exec, arg);
            }
        }
      else
        {
          g_string_append (new_exec, " ");
          g_string_append (new_exec, escaped_app);
        }

      g_key_file_set_string (keyfile, groups[i], G_KEY_FILE_DESKTOP_KEY_EXEC, new_exec->str);
    }

  new_data = g_key_file_to_data (keyfile, &new_data_len, error);
  if (new_data == NULL)
    goto out;

  if (!gs_file_open_in_tmpdir_at (parent_fd, 0755, &tmpfile_name, &out_stream, cancellable, error))
    goto out;

  if (!g_output_stream_write_all (out_stream, new_data, new_data_len, NULL, cancellable, error))
    goto out;

  if (!g_output_stream_close (out_stream, cancellable, error))
    goto out;

  gs_transfer_out_value (target, &tmpfile_name);

  ret = TRUE;
 out:

  if (new_exec != NULL)
    g_string_free (new_exec, TRUE);

  return ret;
}

static gboolean
export_dir (const char    *app,
            const char    *branch,
            const char    *arch,
            int            source_parent_fd,
            const char    *source_name,
            const char    *source_symlink_prefix,
            const char    *source_relpath,
            int            destination_parent_fd,
            const char    *destination_name,
            GCancellable  *cancellable,
            GError       **error)
{
  gboolean ret = FALSE;
  int res;
  gs_dirfd_iterator_cleanup GSDirFdIterator source_iter;
  gs_fd_close int destination_dfd = -1;
  gs_unref_hashtable GHashTable *visited_children = NULL;
  struct dirent *dent;

  if (!gs_dirfd_iterator_init_at (source_parent_fd, source_name, FALSE, &source_iter, error))
    goto out;

  visited_children = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  do
    res = mkdirat (destination_parent_fd, destination_name, 0777);
  while (G_UNLIKELY (res == -1 && errno == EINTR));
  if (res == -1)
    {
      if (errno != EEXIST)
        {
          gs_set_error_from_errno (error, errno);
          goto out;
        }
    }

  if (!gs_file_open_dir_fd_at (destination_parent_fd, destination_name,
                               &destination_dfd,
                               cancellable, error))
    goto out;

  while (TRUE)
    {
      struct stat stbuf;

      if (!gs_dirfd_iterator_next_dent (&source_iter, &dent, cancellable, error))
        goto out;

      if (dent == NULL)
        break;

      if (g_hash_table_contains (visited_children, dent->d_name))
          continue;

      /* Avoid processing the same file again if it was re-created during an export */
      g_hash_table_insert (visited_children, g_strdup (dent->d_name), GINT_TO_POINTER(1));

      if (fstatat (source_iter.fd, dent->d_name, &stbuf, AT_SYMLINK_NOFOLLOW) == -1)
        {
          int errsv = errno;
          if (errsv == ENOENT)
            continue;
          else
            {
              gs_set_error_from_errno (error, errsv);
              goto out;
            }
        }

      if (S_ISDIR (stbuf.st_mode))
        {
          gs_free gchar *child_symlink_prefix = g_build_filename ("..", source_symlink_prefix, dent->d_name, NULL);
          gs_free gchar *child_relpath = g_strconcat (source_relpath, dent->d_name, "/", NULL);

          if (!export_dir (app, branch, arch,
                           source_iter.fd, dent->d_name, child_symlink_prefix, child_relpath, destination_dfd, dent->d_name,
                           cancellable, error))
            goto out;
        }
      else if (S_ISREG (stbuf.st_mode))
        {
          gs_free gchar *target = NULL;

          if (!xdg_app_has_name_prefix (dent->d_name, app))
            {
              g_warning ("Non-prefixed filename %s in app %s, ignoring.\n", dent->d_name, app);
              continue;
            }

          if (g_str_has_suffix (dent->d_name, ".desktop") || g_str_has_suffix (dent->d_name, ".service"))
            {
              gs_free gchar *new_name = NULL;

              if (!export_desktop_file (app, branch, arch, source_iter.fd, dent->d_name, &stbuf, &new_name, cancellable, error))
                goto out;

              g_hash_table_insert (visited_children, g_strdup (new_name), GINT_TO_POINTER(1));

              if (renameat (source_iter.fd, new_name, source_iter.fd, dent->d_name) != 0)
                {
                  gs_set_error_from_errno (error, errno);
                  goto out;
                }
            }

          target = g_build_filename (source_symlink_prefix, dent->d_name, NULL);

          if (unlinkat (destination_dfd, dent->d_name, 0) != 0 && errno != ENOENT)
            {
              gs_set_error_from_errno (error, errno);
              goto out;
            }

          if (symlinkat (target, destination_dfd, dent->d_name) != 0)
            {
              gs_set_error_from_errno (error, errno);
              goto out;
            }
        }
      else
        {
          g_warning ("Not exporting file %s of unsupported type\n", source_relpath);
        }
    }

  ret = TRUE;
 out:

  return ret;
}

gboolean
xdg_app_export_dir (const char *app,
                    const char *branch,
                    const char *arch,
                    GFile    *source,
                    GFile    *destination,
                    const char *symlink_prefix,
                    GCancellable  *cancellable,
                    GError       **error)
{
  gboolean ret = FALSE;

  if (!gs_file_ensure_directory (destination, TRUE, cancellable, error))
    goto out;

  /* The fds are closed by this call */
  if (!export_dir (app, branch, arch,
                   AT_FDCWD, gs_file_get_path_cached (source), symlink_prefix, "",
                   AT_FDCWD, gs_file_get_path_cached (destination),
                   cancellable, error))
    goto out;

  ret = TRUE;

 out:
  return ret;
}

static gboolean
xdg_app_dir_update_exports (XdgAppDir *self,
                            GCancellable *cancellable,
                            GError **error)
{
  gboolean ret = FALSE;
  gs_unref_object GFile *exports = NULL;

  exports = xdg_app_dir_get_exports_dir (self);

  if (g_file_query_exists (exports, cancellable))
    {
      if (!xdg_app_remove_dangling_symlinks (exports, cancellable, error))
        goto out;

      if (!xdg_app_dir_run_triggers (self, cancellable, error))
        goto out;
    }

  ret = TRUE;

 out:
  return ret;
}

gboolean
xdg_app_dir_deploy (XdgAppDir *self,
                    const char *ref,
                    const char *checksum,
                    GCancellable *cancellable,
                    GError **error)
{
  gboolean ret = FALSE;
  gboolean is_app;
  gs_free char *resolved_ref = NULL;
  gs_unref_object GFile *root = NULL;
  gs_unref_object GFileInfo *file_info = NULL;
  gs_unref_object GFile *deploy_base = NULL;
  gs_unref_object GFile *checkoutdir = NULL;
  gs_unref_object GFile *dotref = NULL;
  gs_unref_object GFile *export = NULL;
  gs_unref_object GFile *exports = NULL;

  if (!xdg_app_dir_ensure_repo (self, cancellable, error))
    goto out;

  deploy_base = xdg_app_dir_get_deploy_dir (self, ref);

  if (checksum == NULL)
    {
      g_debug ("No checksum specified, getting tip of %s", ref);
      if (!ostree_repo_resolve_rev (self->repo, ref, FALSE, &resolved_ref, error))
        {
          g_prefix_error (error, "While trying to resolve ref %s: ", ref);
          goto out;
        }

      checksum = resolved_ref;
    }
  else
    {
      gs_unref_object GFile *root = NULL;
      gs_free char *commit = NULL;

      g_debug ("Looking for checksum %s in local repo", checksum);
      if (!ostree_repo_read_commit (self->repo, checksum, &root, &commit, cancellable, NULL))
        {
           GSConsole *console = NULL;
           gs_unref_object OstreeAsyncProgress *progress = NULL;
           const char *refs[2];
           gs_unref_object GFile *origin = NULL;
           gs_free char *repository = NULL;

           refs[0] = checksum;
           refs[1] = NULL;

           origin = g_file_get_child (deploy_base, "origin");
           if (!g_file_load_contents (origin, cancellable, &repository, NULL, NULL, error))
             goto out;

           g_debug ("Pulling checksum %s from remote %s", checksum, repository);

           console = gs_console_get ();
           if (console)
             {
               gs_console_begin_status_line (console, "", NULL, NULL);
               progress = ostree_async_progress_new_and_connect (ostree_repo_pull_default_console_progress_changed, console);
             }

           if (!ostree_repo_pull (self->repo, repository,
                                  (char **)refs, OSTREE_REPO_PULL_FLAGS_NONE,
                                  progress,
                                  cancellable, error))
             {
               g_prefix_error (error, "Failed to pull %s from remote %s: ", checksum, repository);
               goto out;
             }
        }
    }

  checkoutdir = g_file_get_child (deploy_base, checksum);
  if (g_file_query_exists (checkoutdir, cancellable))
    {
      g_set_error (error, XDG_APP_DIR_ERROR,
                   XDG_APP_DIR_ERROR_ALREADY_DEPLOYED,
                   "%s version %s already deployed", ref, checksum);
      goto out;
    }

  if (!ostree_repo_read_commit (self->repo, checksum, &root, NULL, cancellable, error))
    {
      g_prefix_error (error, "Failed to read commit %s: ", checksum);
      goto out;
    }

  file_info = g_file_query_info (root, OSTREE_GIO_FAST_QUERYINFO,
                                 G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                 cancellable, error);
  if (file_info == NULL)
    goto out;

  if (!ostree_repo_checkout_tree (self->repo,
                                  self->user ? OSTREE_REPO_CHECKOUT_MODE_USER : OSTREE_REPO_CHECKOUT_MODE_NONE,
                                  OSTREE_REPO_CHECKOUT_OVERWRITE_NONE,
                                  checkoutdir,
                                  OSTREE_REPO_FILE (root), file_info,
                                  cancellable, error))
    {
      gs_free char *rootpath = NULL;
      gs_free char *checkoutpath = NULL;

      rootpath = g_file_get_path (root);
      checkoutpath = g_file_get_path (checkoutdir);
      g_prefix_error (error, "While trying to checkout %s into %s: ", rootpath, checkoutpath);
      goto out;
    }

  dotref = g_file_resolve_relative_path (checkoutdir, "files/.ref");
  if (!g_file_replace_contents (dotref, "", 0, NULL, FALSE,
                                G_FILE_CREATE_NONE, NULL, cancellable, error))
    goto out;

  is_app = g_str_has_prefix (ref, "app");

  exports = xdg_app_dir_get_exports_dir (self);
  if (is_app)
    {
      export = g_file_get_child (checkoutdir, "export");
      if (g_file_query_exists (export, cancellable))
        {
          gs_free char *relative_path = NULL;
          gs_free char *symlink_prefix = NULL;
          gs_strfreev char **ref_parts = NULL;

          relative_path = g_file_get_relative_path (self->basedir, export);
          symlink_prefix = g_build_filename ("..", relative_path, NULL);

          ref_parts = g_strsplit (ref, "/", -1);

          if (!xdg_app_export_dir (ref_parts[1], ref_parts[3], ref_parts[2], export, exports,
                                   symlink_prefix,
                                   cancellable,
                                   error))
            goto out;
        }
    }

  if (!xdg_app_dir_set_active (self, ref, checksum, cancellable, error))
    goto out;

  if (is_app && !xdg_app_dir_update_exports (self, cancellable, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}

gboolean
xdg_app_dir_collect_deployed_refs (XdgAppDir *self,
				   const char *type,
				   const char *name_prefix,
				   const char *branch,
				   const char *arch,
				   GHashTable *hash,
				   GCancellable *cancellable,
				   GError **error)
{
  gboolean ret = FALSE;
  gs_unref_object GFile *dir = NULL;
  gs_unref_object GFileEnumerator *dir_enum = NULL;
  gs_unref_object GFileInfo *child_info = NULL;
  GError *temp_error = NULL;

  dir = g_file_get_child (self->basedir, type);
  if (!g_file_query_exists (dir, cancellable))
    return TRUE;

  dir_enum = g_file_enumerate_children (dir, OSTREE_GIO_FAST_QUERYINFO,
                                        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                        cancellable,
                                        error);
  if (!dir_enum)
    goto out;

  while ((child_info = g_file_enumerator_next_file (dir_enum, cancellable, &temp_error)) != NULL)
    {
      const char *name = g_file_info_get_name (child_info);

      if (g_file_info_get_file_type (child_info) == G_FILE_TYPE_DIRECTORY &&
          name[0] != '.' && (name_prefix == NULL || g_str_has_prefix (name, name_prefix)))
	{
	  gs_unref_object GFile *child1 = g_file_get_child (dir, name);
	  gs_unref_object GFile *child2 = g_file_get_child (child1, branch);
	  gs_unref_object GFile *child3 = g_file_get_child (child2, arch);
	  gs_unref_object GFile *active = g_file_get_child (child3, "active");

	  if (g_file_query_exists (active, cancellable))
	    g_hash_table_add (hash, g_strdup (name));
	}

      g_clear_object (&child_info);
    }

  if (temp_error != NULL)
    {
      g_propagate_error (error, temp_error);
      goto out;
    }

  ret = TRUE;
 out:
  return ret;
}

gboolean
xdg_app_dir_list_deployed (XdgAppDir *self,
                           const char *ref,
                           char ***deployed_checksums,
                           GCancellable *cancellable,
                           GError **error)
{
  gboolean ret = FALSE;
  gs_unref_object GFile *deploy_base = NULL;
  gs_unref_ptrarray GPtrArray *checksums = NULL;
  GError *temp_error = NULL;
  gs_unref_object GFileEnumerator *dir_enum = NULL;
  gs_unref_object GFile *child = NULL;
  gs_unref_object GFileInfo *child_info = NULL;

  deploy_base = xdg_app_dir_get_deploy_dir (self, ref);

  checksums = g_ptr_array_new_with_free_func (g_free);

  dir_enum = g_file_enumerate_children (deploy_base, OSTREE_GIO_FAST_QUERYINFO,
                                        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                        cancellable,
                                        error);
  if (!dir_enum)
    goto out;

  while ((child_info = g_file_enumerator_next_file (dir_enum, cancellable, &temp_error)) != NULL)
    {
      const char *name;

      name = g_file_info_get_name (child_info);

      g_clear_object (&child);
      child = g_file_get_child (deploy_base, name);

      if (g_file_info_get_file_type (child_info) == G_FILE_TYPE_DIRECTORY &&
          name[0] != '.' &&
          strlen (name) == 64)
        g_ptr_array_add (checksums, g_strdup (name));

      g_clear_object (&child_info);
    }

  if (temp_error != NULL)
    {
      g_propagate_error (error, temp_error);
      goto out;
    }

  g_ptr_array_add (checksums, NULL);
  *deployed_checksums = (char **)g_ptr_array_free (checksums, FALSE);
  checksums = NULL;

  ret = TRUE;
 out:
  return ret;

}

static gboolean
dir_is_locked (GFile *dir)
{
  gs_fd_close int ref_fd = -1;
  struct flock lock = {0};
  gs_unref_object GFile *reffile = NULL;

  reffile = g_file_resolve_relative_path (dir, "files/.ref");

  ref_fd = open (gs_file_get_path_cached (reffile), O_RDWR | O_CLOEXEC);
  if (ref_fd != -1)
    {
      lock.l_type = F_WRLCK;
      lock.l_whence = SEEK_SET;
      lock.l_start = 0;
      lock.l_len = 0;

      if (fcntl (ref_fd, F_GETLK, &lock) == 0)
	return lock.l_type != F_UNLCK;
    }

  return FALSE;
}

gboolean
xdg_app_dir_undeploy (XdgAppDir *self,
                      const char *ref,
                      const char *checksum,
		      gboolean force_remove,
                      GCancellable *cancellable,
                      GError **error)
{
  gboolean ret = FALSE;
  gs_unref_object GFile *deploy_base = NULL;
  gs_unref_object GFile *checkoutdir = NULL;
  gs_unref_object GFile *removed_subdir = NULL;
  gs_unref_object GFile *removed_dir = NULL;
  gs_free char *tmpname = NULL;
  gs_free char *active = NULL;
  gboolean is_app;
  int i;

  g_assert (ref != NULL);
  g_assert (checksum != NULL);

  deploy_base = xdg_app_dir_get_deploy_dir (self, ref);

  checkoutdir = g_file_get_child (deploy_base, checksum);
  if (!g_file_query_exists (checkoutdir, cancellable))
    {
      g_set_error (error, XDG_APP_DIR_ERROR,
                   XDG_APP_DIR_ERROR_ALREADY_UNDEPLOYED,
                   "%s version %s already undeployed", ref, checksum);
      goto out;
    }

  if (!xdg_app_dir_ensure_repo (self, cancellable, error))
    goto out;

  active = xdg_app_dir_read_active (self, ref, cancellable);
  if (active != NULL && strcmp (active, checksum) == 0)
    {
      gs_strfreev char **deployed_checksums = NULL;
      const char *some_deployment;

      /* We're removing the active deployment, start by repointing that
         to another deployment if one exists */

      if (!xdg_app_dir_list_deployed (self, ref,
                                      &deployed_checksums,
                                      cancellable, error))
        goto out;

      some_deployment = NULL;
      for (i = 0; deployed_checksums[i] != NULL; i++)
        {
          if (strcmp (deployed_checksums[i], checksum) == 0)
            continue;

          some_deployment = deployed_checksums[i];
          break;
        }

      if (!xdg_app_dir_set_active (self, ref, some_deployment, cancellable, error))
        goto out;
    }

  removed_dir = xdg_app_dir_get_removed_dir (self);
  if (!gs_file_ensure_directory (removed_dir, TRUE, cancellable, error))
    goto out;

  tmpname = gs_fileutil_gen_tmp_name ("", checksum);
  removed_subdir = g_file_get_child (removed_dir, tmpname);

  if (!gs_file_rename (checkoutdir,
                       removed_subdir,
                       cancellable, error))
    goto out;

  if (force_remove || !dir_is_locked (removed_subdir))
    {
      if (!gs_shutil_rm_rf (removed_subdir, cancellable, error))
	goto out;
    }

  is_app = g_str_has_prefix (ref, "app");

  if (is_app && !xdg_app_dir_update_exports (self, cancellable, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}

gboolean
xdg_app_dir_cleanup_removed (XdgAppDir      *self,
			     GCancellable   *cancellable,
			     GError        **error)
{
  gboolean ret = FALSE;
  gs_unref_object GFile *removed_dir = NULL;
  gs_unref_object GFileEnumerator *dir_enum = NULL;
  gs_unref_object GFileInfo *child_info = NULL;
  GError *temp_error = NULL;

  removed_dir = xdg_app_dir_get_removed_dir (self);
  if (!g_file_query_exists (removed_dir, cancellable))
    return TRUE;

  dir_enum = g_file_enumerate_children (removed_dir, OSTREE_GIO_FAST_QUERYINFO,
                                        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                        cancellable,
                                        error);
  if (!dir_enum)
    goto out;

  while ((child_info = g_file_enumerator_next_file (dir_enum, cancellable, &temp_error)) != NULL)
    {
      const char *name = g_file_info_get_name (child_info);
      gs_unref_object GFile *child = g_file_get_child (removed_dir, name);

      if (g_file_info_get_file_type (child_info) == G_FILE_TYPE_DIRECTORY &&
	  !dir_is_locked (child))
	{
	  gs_shutil_rm_rf (child, cancellable, NULL);
	}

      g_clear_object (&child_info);
    }

  if (temp_error != NULL)
    {
      g_propagate_error (error, temp_error);
      goto out;
    }

  ret = TRUE;
 out:
  return ret;
}


gboolean
xdg_app_dir_prune (XdgAppDir      *self,
                   GCancellable   *cancellable,
                   GError        **error)
{
  gboolean ret = FALSE;
  gint objects_total, objects_pruned;
  guint64 pruned_object_size_total;
  gs_free char *formatted_freed_size = NULL;

  if (!xdg_app_dir_ensure_repo (self, cancellable, error))
    goto out;

  if (!ostree_repo_prune (self->repo,
                          OSTREE_REPO_PRUNE_FLAGS_REFS_ONLY,
                          0,
                          &objects_total,
                          &objects_pruned,
                          &pruned_object_size_total,
                          cancellable, error))
    goto out;

  formatted_freed_size = g_format_size_full (pruned_object_size_total, 0);
  g_debug ("Pruned %d/%d objects, size %s", objects_total, objects_pruned, formatted_freed_size);

  ret = TRUE;
 out:
  return ret;

}

GFile *
xdg_app_dir_get_if_deployed (XdgAppDir     *self,
                             const char    *ref,
                             const char    *checksum,
                             GCancellable  *cancellable)
{
  gs_unref_object GFile *deploy_base = NULL;
  gs_unref_object GFile *deploy_dir = NULL;

  deploy_base = xdg_app_dir_get_deploy_dir (self, ref);
  deploy_dir = g_file_get_child (deploy_base, checksum ? checksum : "active");

  if (g_file_query_file_type (deploy_dir, G_FILE_QUERY_INFO_NONE, cancellable) == G_FILE_TYPE_DIRECTORY)
    return g_object_ref (deploy_dir);
  return NULL;
}

XdgAppDir*
xdg_app_dir_new (GFile *path, gboolean user)
{
  return g_object_new (XDG_APP_TYPE_DIR, "path", path, "user", user, NULL);
}

XdgAppDir *
xdg_app_dir_get_system (void)
{
  static XdgAppDir *system = NULL;

  if (system == NULL)
    {
      gs_unref_object GFile *path = xdg_app_get_system_base_dir_location ();
      system = xdg_app_dir_new (path, FALSE);
    }

  return g_object_ref (system);
}

XdgAppDir *
xdg_app_dir_get_user  (void)
{
  static XdgAppDir *user = NULL;

  if (user == NULL)
    {
      gs_unref_object GFile *path = xdg_app_get_user_base_dir_location ();
      user = xdg_app_dir_new (path, TRUE);
    }

  return g_object_ref (user);
}

XdgAppDir *
xdg_app_dir_get (gboolean user)
{
  if (user)
    return xdg_app_dir_get_user ();
  else
    return xdg_app_dir_get_system ();
}
