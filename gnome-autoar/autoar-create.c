/* vim: set sw=2 ts=2 sts=2 et: */
/* -*- Mode: C; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/*
 * autoar-create.c
 * Automatically create archives in some GNOME programs
 *
 * Copyright (C) 2013  Ting-Wei Lan
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA  02110-1301, USA.
 *
 */

#include "config.h"

#include "autoar-create.h"
#include "autoar-common.h"
#include "autoar-pref.h"

#include <archive.h>
#include <archive_entry.h>
#include <gio/gio.h>
#include <glib.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

G_DEFINE_TYPE (AutoarCreate, autoar_create, G_TYPE_OBJECT)
G_DEFINE_QUARK (autoar-create, autoar_create)

#define AUTOAR_CREATE_GET_PRIVATE(o) \
  (G_TYPE_INSTANCE_GET_PRIVATE ((o), AUTOAR_TYPE_CREATE, AutoarCreatePrivate))

#define BUFFER_SIZE (64 * 1024)
#define ARCHIVE_WRITE_RETRY_TIMES 5

#define INVALID_FORMAT 1
#define INVALID_FILTER 2

struct _AutoarCreatePrivate
{
  char **source;
  char  *output;

  GPtrArray *source_file; /* An array of GFile, not terminated by NULL */
  GFile     *output_file;

  int output_is_dest : 1;

  guint64 size; /* This field is currently unused */
  guint64 completed_size;

  guint files;
  guint completed_files;

  AutoarPref *arpref;

  GOutputStream *ostream;
  void          *buffer;
  gssize         buffer_size;
  GError        *error;

  GCancellable *cancellable;

  struct archive                    *a;
  struct archive_entry              *entry;
  struct archive_entry_linkresolver *resolver;
  GFile                             *dest;
  GHashTable                        *pathname_to_g_file;
  char                              *source_basename_noext;
  char                              *extension;

  int in_thread        : 1;
  int prepend_basename : 1;
};

enum
{
  DECIDE_DEST,
  PROGRESS,
  CANCELLED,
  COMPLETED,
  ERROR,
  LAST_SIGNAL
};

enum
{
  PROP_0,
  PROP_SOURCE,
  PROP_SOURCE_FILE,
  PROP_OUTPUT,
  PROP_OUTPUT_FILE,
  PROP_SIZE, /* This property is currently unused */
  PROP_COMPLETED_SIZE,
  PROP_FILES,
  PROP_COMPLETED_FILES,
  PROP_OUTPUT_IS_DEST
};

static guint autoar_create_signals[LAST_SIGNAL] = { 0 };

static void
autoar_create_get_property (GObject    *object,
                            guint       property_id,
                            GValue     *value,
                            GParamSpec *pspec)
{
  AutoarCreate *arcreate;
  AutoarCreatePrivate *priv;

  arcreate = AUTOAR_CREATE (object);
  priv = arcreate->priv;

  switch (property_id) {
    case PROP_SOURCE:
      g_value_set_boxed (value, priv->source);
      break;
    case PROP_SOURCE_FILE:
      g_value_set_boxed (value, priv->source_file);
      break;
    case PROP_OUTPUT:
      g_value_set_string (value, priv->output);
      break;
    case PROP_OUTPUT_FILE:
      g_value_set_object (value, priv->output_file);
      break;
    case PROP_SIZE:
      g_value_set_uint64 (value, priv->size);
      break;
    case PROP_COMPLETED_SIZE:
      g_value_set_uint64 (value, priv->completed_size);
      break;
    case PROP_FILES:
      g_value_set_uint (value, priv->files);
      break;
    case PROP_COMPLETED_FILES:
      g_value_set_uint (value, priv->completed_files);
      break;
    case PROP_OUTPUT_IS_DEST:
      g_value_set_boolean (value, priv->output_is_dest);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}

static void
autoar_create_set_property (GObject      *object,
                            guint         property_id,
                            const GValue *value,
                            GParamSpec   *pspec)
{
  AutoarCreate *arcreate;
  AutoarCreatePrivate *priv;

  arcreate = AUTOAR_CREATE (object);
  priv = arcreate->priv;

  switch (property_id) {
    case PROP_SIZE:
      autoar_create_set_size (arcreate, g_value_get_uint64 (value));
      break;
    case PROP_COMPLETED_SIZE:
      autoar_create_set_completed_size (arcreate, g_value_get_uint64 (value));
      break;
    case PROP_FILES:
      autoar_create_set_files (arcreate, g_value_get_uint (value));
      break;
    case PROP_COMPLETED_FILES:
      autoar_create_set_completed_files (arcreate, g_value_get_uint (value));
      break;
    case PROP_SOURCE:
      g_strfreev (priv->source);
      priv->source = g_value_dup_boxed (value);
      break;
    case PROP_SOURCE_FILE:
      if (priv->source_file != NULL)
        g_ptr_array_unref (priv->source_file);
      priv->source_file = g_value_dup_boxed (value);
      break;
    case PROP_OUTPUT:
      g_free (priv->output);
      priv->output = g_value_dup_string (value);
      break;
    case PROP_OUTPUT_FILE:
      autoar_common_g_object_unref (priv->output_file);
      priv->output_file = g_object_ref (g_value_get_object (value));
      break;
    case PROP_OUTPUT_IS_DEST:
      priv->output_is_dest = g_value_get_boolean (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}

char**
autoar_create_get_source (AutoarCreate *arcreate)
{
  g_return_val_if_fail (AUTOAR_IS_CREATE (arcreate), NULL);
  return arcreate->priv->source;
}

GPtrArray*
autoar_create_get_source_file (AutoarCreate *arcreate)
{
  g_return_val_if_fail (AUTOAR_IS_CREATE (arcreate), NULL);
  return arcreate->priv->source_file;
}

char*
autoar_create_get_output (AutoarCreate *arcreate)
{
  g_return_val_if_fail (AUTOAR_IS_CREATE (arcreate), NULL);
  return arcreate->priv->output;
}

GFile*
autoar_create_get_output_file (AutoarCreate *arcreate)
{
  g_return_val_if_fail (AUTOAR_IS_CREATE (arcreate), NULL);
  return arcreate->priv->output_file;
}

guint64
autoar_create_get_size (AutoarCreate *arcreate)
{
  g_return_val_if_fail (AUTOAR_IS_CREATE (arcreate), 0);
  return arcreate->priv->size;
}

guint64
autoar_create_get_completed_size (AutoarCreate *arcreate)
{
  g_return_val_if_fail (AUTOAR_IS_CREATE (arcreate), 0);
  return arcreate->priv->completed_size;
}

guint
autoar_create_get_files (AutoarCreate *arcreate)
{
  g_return_val_if_fail (AUTOAR_IS_CREATE (arcreate), 0);
  return arcreate->priv->files;
}

guint
autoar_create_get_completed_files (AutoarCreate *arcreate)
{
  g_return_val_if_fail (AUTOAR_IS_CREATE (arcreate), 0);
  return arcreate->priv->completed_files;
}

gboolean
autoar_create_get_output_is_dest (AutoarCreate *arcreate)
{
  g_return_val_if_fail (AUTOAR_IS_CREATE (arcreate), 0);
  return arcreate->priv->output_is_dest;
}

void
autoar_create_set_size (AutoarCreate *arcreate,
                        guint64 size)
{
  g_return_if_fail (AUTOAR_IS_CREATE (arcreate));
  arcreate->priv->size = size;
}

void
autoar_create_set_completed_size (AutoarCreate *arcreate,
                                  guint64 completed_size)
{
  g_return_if_fail (AUTOAR_IS_CREATE (arcreate));
  g_return_if_fail (completed_size >= arcreate->priv->completed_size);
  arcreate->priv->completed_size = completed_size;
}

void
autoar_create_set_files (AutoarCreate *arcreate,
                         guint files)
{
  g_return_if_fail (AUTOAR_IS_CREATE (arcreate));
  arcreate->priv->files = files;
}

void
autoar_create_set_completed_files (AutoarCreate *arcreate,
                                   guint completed_files)
{
  g_return_if_fail (AUTOAR_IS_CREATE (arcreate));
  g_return_if_fail (completed_files >= arcreate->priv->completed_files);
  arcreate->priv->completed_files = completed_files;
}

void
autoar_create_set_output_is_dest (AutoarCreate *arcreate,
                                  gboolean output_is_dest)
{
  g_return_if_fail (AUTOAR_IS_CREATE (arcreate));
  arcreate->priv->output_is_dest = output_is_dest;
}

static void
autoar_create_dispose (GObject *object)
{
  AutoarCreate *arcreate;
  AutoarCreatePrivate *priv;

  arcreate = AUTOAR_CREATE (object);
  priv = arcreate->priv;

  g_debug ("AutoarCreate: dispose");

  if (priv->ostream != NULL) {
    if (!g_output_stream_is_closed (priv->ostream)) {
      g_output_stream_close (priv->ostream, priv->cancellable, NULL);
    }
    g_object_unref (priv->ostream);
    priv->ostream = NULL;
  }

  g_clear_object (&(priv->dest));
  g_clear_object (&(priv->arpref));
  g_clear_object (&(priv->cancellable));
  g_clear_object (&(priv->output_file));

  if (priv->pathname_to_g_file != NULL) {
    g_hash_table_unref (priv->pathname_to_g_file);
    priv->pathname_to_g_file = NULL;
  }

  if (priv->source_file != NULL) {
    g_ptr_array_unref (priv->source_file);
    priv->source_file = NULL;
  }

  G_OBJECT_CLASS (autoar_create_parent_class)->dispose (object);
}

static void
autoar_create_finalize (GObject *object)
{
  AutoarCreate *arcreate;
  AutoarCreatePrivate *priv;

  arcreate = AUTOAR_CREATE (object);
  priv = arcreate->priv;

  g_debug ("AutoarCreate: finalize");

  g_strfreev (priv->source);
  priv->source = NULL;

  g_free (priv->output);
  priv->output = NULL;

  g_free (priv->buffer);
  priv->buffer = NULL;

  /* If priv->error == NULL, no errors occurs. Therefore, we can safely free
   * libarchive objects because it will not call the callbacks during the
   * the process of freeing.
   * If priv->error != NULL, we must free libarchive objects beforce freeing
   * priv->error in order to prevent libarchive callbacks from accessing
   * freed private objects and buffers. */
  if (priv->a != NULL) {
    archive_write_free (priv->a);
    priv->a = NULL;
  }

  if (priv->entry != NULL) {
    archive_entry_free (priv->entry);
    priv->entry = NULL;
  }

  if (priv->resolver != NULL) {
    archive_entry_linkresolver_free (priv->resolver);
    priv->resolver = NULL;
  }

  if (priv->error != NULL) {
    g_error_free (priv->error);
    priv->error = NULL;
  }

  g_free (priv->source_basename_noext);
  priv->source_basename_noext = NULL;

  g_free (priv->extension);
  priv->extension = NULL;

  G_OBJECT_CLASS (autoar_create_parent_class)->finalize (object);
}

static int
libarchive_write_open_cb (struct archive *ar_write,
                          void *client_data)
{
  AutoarCreate *arcreate;

  g_debug ("libarchive_write_open_cb: called");

  arcreate = (AutoarCreate*)client_data;
  if (arcreate->priv->error != NULL) {
    return ARCHIVE_FATAL;
  }

  arcreate->priv->ostream = (GOutputStream*)g_file_create (arcreate->priv->dest,
                                                           G_FILE_CREATE_NONE,
                                                           arcreate->priv->cancellable,
                                                           &(arcreate->priv->error));
  if (arcreate->priv->error != NULL) {
    g_debug ("libarchive_write_open_cb: ARCHIVE_FATAL");
    return ARCHIVE_FATAL;
  }

  g_debug ("libarchive_write_open_cb: ARCHIVE_OK");
  return ARCHIVE_OK;
}

static int
libarchive_write_close_cb (struct archive *ar_write,
                           void *client_data)
{
  AutoarCreate *arcreate;

  g_debug ("libarchive_write_close_cb: called");

  arcreate = (AutoarCreate*)client_data;
  if (arcreate->priv->error != NULL) {
    return ARCHIVE_FATAL;
  }

  if (arcreate->priv->ostream != NULL) {
    g_output_stream_close (arcreate->priv->ostream, arcreate->priv->cancellable, &(arcreate->priv->error));
    g_object_unref (arcreate->priv->ostream);
    arcreate->priv->ostream = NULL;
  }

  if (arcreate->priv->error != NULL) {
    g_debug ("libarchive_write_close_cb: ARCHIVE_FATAL");
    return ARCHIVE_FATAL;
  }

  g_debug ("libarchive_write_close_cb: ARCHIVE_OK");
  return ARCHIVE_OK;
}

static ssize_t
libarchive_write_write_cb (struct archive *ar_write,
                           void *client_data,
                           const void *buffer,
                           size_t length)
{
  AutoarCreate *arcreate;
  gssize write_size;

  g_debug ("libarchive_write_write_cb: called");

  arcreate = (AutoarCreate*)client_data;
  if (arcreate->priv->error != NULL) {
    return -1;
  }

  write_size = g_output_stream_write (arcreate->priv->ostream,
                                      buffer,
                                      length,
                                      arcreate->priv->cancellable,
                                      &(arcreate->priv->error));
  if (arcreate->priv->error != NULL)
    return -1;

  g_debug ("libarchive_write_write_cb: %lu", write_size);
  return write_size;
}

static inline void
autoar_create_signal_decide_dest (AutoarCreate *arcreate)
{
  autoar_common_g_signal_emit (arcreate, arcreate->priv->in_thread,
                               autoar_create_signals[DECIDE_DEST], 0,
                               arcreate->priv->dest);
}

static inline void
autoar_create_signal_progress (AutoarCreate *arcreate)
{
  autoar_common_g_signal_emit (arcreate, arcreate->priv->in_thread,
                               autoar_create_signals[PROGRESS], 0,
                               arcreate->priv->completed_size,
                               arcreate->priv->completed_files);
}

static inline void
autoar_create_signal_cancelled (AutoarCreate *arcreate)
{
  autoar_common_g_signal_emit (arcreate, arcreate->priv->in_thread,
                               autoar_create_signals[CANCELLED], 0);

}

static inline void
autoar_create_signal_completed (AutoarCreate *arcreate)
{
  autoar_common_g_signal_emit (arcreate, arcreate->priv->in_thread,
                               autoar_create_signals[COMPLETED], 0);

}

static inline void
autoar_create_signal_error (AutoarCreate *arcreate)
{
  if (arcreate->priv->error != NULL) {
    if (arcreate->priv->error->domain == G_IO_ERROR &&
        arcreate->priv->error->code == G_IO_ERROR_CANCELLED) {
      g_error_free (arcreate->priv->error);
      arcreate->priv->error = NULL;
      autoar_create_signal_cancelled (arcreate);
    } else {
      autoar_common_g_signal_emit (arcreate, arcreate->priv->in_thread,
                                   autoar_create_signals[ERROR], 0,
                                   arcreate->priv->error);
    }
  }
}

static void
autoar_create_do_write_data (AutoarCreate *arcreate,
                             struct archive_entry *entry,
                             GFile *file)
{
  int r;
  AutoarCreatePrivate *priv;

  g_debug ("autoar_create_do_write_data: called");

  if (arcreate->priv->error != NULL)
    return;

  if (g_cancellable_is_cancelled (arcreate->priv->cancellable))
    return;

  priv = arcreate->priv;

  while ((r = archive_write_header (priv->a, entry)) == ARCHIVE_RETRY);
  if (r == ARCHIVE_FATAL) {
    if (priv->error == NULL)
      priv->error = autoar_common_g_error_new_a_entry (priv->a, entry);
    return;
  }

  g_debug ("autoar_create_do_write_data: write header OK");

  /* Non-regular files have no content to write */
  if (archive_entry_size (entry) > 0 && archive_entry_filetype (entry) == AE_IFREG) {
    GInputStream *istream;
    ssize_t read_actual, written_actual, written_acc;
    int written_try;

    g_debug ("autoar_create_do_write_data: entry size is %"G_GUINT64_FORMAT,
             archive_entry_size (entry));

    written_actual = 0;
    written_try = 0;

    istream = (GInputStream*)g_file_read (file, priv->cancellable, &(priv->error));
    if (istream == NULL)
      return;

    priv->completed_files++;

    do {
      read_actual = g_input_stream_read (istream,
                                         priv->buffer,
                                         priv->buffer_size,
                                         priv->cancellable,
                                         &(priv->error));
      priv->completed_size += read_actual > 0 ? read_actual : 0;
      autoar_create_signal_progress (arcreate);
      if (read_actual > 0) {
        written_acc = 0;
        written_try = 0;
        do {
          written_actual = archive_write_data (priv->a, (const char*)(priv->buffer) + written_acc, read_actual);
          written_acc += written_actual > 0 ? written_actual : 0;
          written_try = written_actual ? 0 : written_try + 1;
          /* archive_write_data may return zero, so we have to limit the
           * retry times to prevent infinite loop */
        } while (written_acc < read_actual && written_actual >= 0 && written_try < ARCHIVE_WRITE_RETRY_TIMES);
      }
    } while (read_actual > 0 && written_actual >= 0);


    g_input_stream_close (istream, priv->cancellable, NULL);
    g_object_unref (istream);

    if (read_actual < 0)
      return;

    if (written_actual < 0 || written_try >= ARCHIVE_WRITE_RETRY_TIMES) {
      if (priv->error == NULL)
        priv->error = autoar_common_g_error_new_a_entry (priv->a, entry);
      return;
    }
    g_debug ("autoar_create_do_write_data: write data OK");
  } else {
    g_debug ("autoar_create_do_write_data: no data, return now!");
    priv->completed_files++;
    autoar_create_signal_progress (arcreate);
  }
}

static void
autoar_create_do_add_to_archive (AutoarCreate *arcreate,
                                 GFile *root,
                                 GFile *file)
{
  AutoarCreatePrivate *priv;
  GFileInfo *info;
  GFileType  filetype;

  priv = arcreate->priv;

  if (priv->error != NULL)
    return;

  if (g_cancellable_is_cancelled (priv->cancellable))
    return;

  archive_entry_clear (priv->entry);
  info = g_file_query_info (file, "*", G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                            priv->cancellable, &(priv->error));
  if (info == NULL)
    return;

  filetype = g_file_info_get_file_type (info);
  switch (archive_format (priv->a)) {
    case ARCHIVE_FORMAT_AR:
    case ARCHIVE_FORMAT_AR_GNU:
    case ARCHIVE_FORMAT_AR_BSD:
      if (filetype == G_FILE_TYPE_DIRECTORY ||
          filetype == G_FILE_TYPE_SYMBOLIC_LINK ||
          filetype == G_FILE_TYPE_SPECIAL) {
        /* ar only support regular files, so we abort this operation to
         * prevent producing a malformed archive. */
        g_object_unref (info);
        return;
      }
      break;

    case ARCHIVE_FORMAT_ZIP:
      if (filetype == G_FILE_TYPE_SPECIAL) {
        /* Add special files to zip archives cause unknown fatal error
         * in libarchive. */
        g_object_unref (info);
        return;
      }
      break;
  }

  {
    char *root_basename;
    char *pathname_relative;
    char *pathname;

    switch (archive_format (priv->a)) {
      /* ar format does not support directories */
      case ARCHIVE_FORMAT_AR:
      case ARCHIVE_FORMAT_AR_GNU:
      case ARCHIVE_FORMAT_AR_BSD:
        pathname = g_file_get_basename (file);
        archive_entry_set_pathname (priv->entry, pathname);
        g_free (pathname);
        break;

      default:
        root_basename = g_file_get_basename (root);
        pathname_relative = g_file_get_relative_path (root, file);
        pathname = g_strconcat (priv->prepend_basename ? priv->source_basename_noext : "",
                                priv->prepend_basename ? "/" : "",
                                root_basename,
                                pathname_relative != NULL ? "/" : "",
                                pathname_relative != NULL ? pathname_relative : "",
                                NULL);
        archive_entry_set_pathname (priv->entry, pathname);
        g_free (root_basename);
        g_free (pathname_relative);
        g_free (pathname);
    }
  }

  g_debug ("autoar_create_do_add_to_archive: %s", archive_entry_pathname (priv->entry));

  {
    time_t atime, btime, ctime, mtime;
    long atimeu, btimeu, ctimeu, mtimeu;

    atime = g_file_info_get_attribute_uint64 (info, G_FILE_ATTRIBUTE_TIME_ACCESS);
    btime = g_file_info_get_attribute_uint64 (info, G_FILE_ATTRIBUTE_TIME_CREATED);
    ctime = g_file_info_get_attribute_uint64 (info, G_FILE_ATTRIBUTE_TIME_CHANGED);
    mtime = g_file_info_get_attribute_uint64 (info, G_FILE_ATTRIBUTE_TIME_MODIFIED);

    atimeu = g_file_info_get_attribute_uint32 (info, G_FILE_ATTRIBUTE_TIME_ACCESS_USEC);
    btimeu = g_file_info_get_attribute_uint32 (info, G_FILE_ATTRIBUTE_TIME_CREATED_USEC);
    ctimeu = g_file_info_get_attribute_uint32 (info, G_FILE_ATTRIBUTE_TIME_CHANGED_USEC);
    mtimeu = g_file_info_get_attribute_uint32 (info, G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC);

    archive_entry_set_atime (priv->entry, atime, atimeu * 1000);
    archive_entry_set_birthtime (priv->entry, btime, btimeu * 1000);
    archive_entry_set_ctime (priv->entry, ctime, ctimeu * 1000);
    archive_entry_set_mtime (priv->entry, mtime, mtimeu * 1000);

    archive_entry_set_uid (priv->entry, g_file_info_get_attribute_uint32 (info, G_FILE_ATTRIBUTE_UNIX_UID));
    archive_entry_set_gid (priv->entry, g_file_info_get_attribute_uint32 (info, G_FILE_ATTRIBUTE_UNIX_GID));
    archive_entry_set_uname (priv->entry, g_file_info_get_attribute_string (info, G_FILE_ATTRIBUTE_OWNER_USER));
    archive_entry_set_gname (priv->entry, g_file_info_get_attribute_string (info, G_FILE_ATTRIBUTE_OWNER_GROUP));
    archive_entry_set_mode (priv->entry, g_file_info_get_attribute_uint32 (info, G_FILE_ATTRIBUTE_UNIX_MODE));
  }

  archive_entry_set_size (priv->entry, g_file_info_get_attribute_uint64 (info, G_FILE_ATTRIBUTE_STANDARD_SIZE));
  archive_entry_set_dev (priv->entry, g_file_info_get_attribute_uint32 (info, G_FILE_ATTRIBUTE_UNIX_DEVICE));
  archive_entry_set_ino64 (priv->entry, g_file_info_get_attribute_uint64 (info, G_FILE_ATTRIBUTE_UNIX_INODE));
  archive_entry_set_nlink (priv->entry, g_file_info_get_attribute_uint32 (info, G_FILE_ATTRIBUTE_UNIX_NLINK));
  archive_entry_set_rdev (priv->entry, g_file_info_get_attribute_uint32 (info, G_FILE_ATTRIBUTE_UNIX_RDEV));

  switch (filetype) {
    case G_FILE_TYPE_DIRECTORY:
      g_debug ("autoar_create_do_add_to_archive: file type set to DIR");
      archive_entry_set_filetype (priv->entry, AE_IFDIR);
      break;

    case G_FILE_TYPE_SYMBOLIC_LINK:
      g_debug ("autoar_create_do_add_to_archive: file type set to SYMLINK");
      archive_entry_set_filetype (priv->entry, AE_IFLNK);
      archive_entry_set_symlink (priv->entry, g_file_info_get_attribute_byte_string (info, G_FILE_ATTRIBUTE_STANDARD_SYMLINK_TARGET));
      break;

    case G_FILE_TYPE_SPECIAL:
#ifdef HAVE_STAT
      {
        struct stat filestat;
        char *local_pathname;

        local_pathname = g_file_get_path (file);
        if (local_pathname != NULL && stat (local_pathname, &filestat) >= 0) {
          if (S_ISBLK (filestat.st_mode)) {
            g_debug ("autoar_create_do_add_to_archive: file type set to BLOCK");
            archive_entry_set_filetype (priv->entry, AE_IFBLK);
          } else if (S_ISSOCK (filestat.st_mode)) {
            g_debug ("autoar_create_do_add_to_archive: file type set to SOCKET");
            archive_entry_set_filetype (priv->entry, AE_IFSOCK);
          } else if (S_ISCHR (filestat.st_mode)) {
            g_debug ("autoar_create_do_add_to_archive: file type set to CHAR");
            archive_entry_set_filetype (priv->entry, AE_IFCHR);
          } else if (S_ISFIFO (filestat.st_mode)) {
            g_debug ("autoar_create_do_add_to_archive: file type set to FIFO");
            archive_entry_set_filetype (priv->entry, AE_IFIFO);
          } else {
            g_debug ("autoar_create_do_add_to_archive: file type set to REGULAR");
            archive_entry_set_filetype (priv->entry, AE_IFREG);
          }
          g_free (local_pathname);
        } else {
          g_debug ("autoar_create_do_add_to_archive: file type set to REGULAR");
          archive_entry_set_filetype (priv->entry, AE_IFREG);
        }
      }
      break;

#endif
    case G_FILE_TYPE_UNKNOWN:
    case G_FILE_TYPE_SHORTCUT:
    case G_FILE_TYPE_MOUNTABLE:
    case G_FILE_TYPE_REGULAR:
    default:
      g_debug ("autoar_create_do_add_to_archive: file type set to REGULAR");
      archive_entry_set_filetype (priv->entry, AE_IFREG);
      break;
  }

  g_hash_table_insert (priv->pathname_to_g_file,
                       g_strdup (archive_entry_pathname (priv->entry)),
                       g_object_ref (file));

  {
    struct archive_entry *entry, *sparse;

    entry = priv->entry;
    archive_entry_linkify (priv->resolver, &entry, &sparse);

    if (entry != NULL) {
      GFile *file_to_read;
      const char *pathname_in_entry;
      pathname_in_entry = archive_entry_pathname (entry);
      file_to_read = g_hash_table_lookup (priv->pathname_to_g_file, pathname_in_entry);
      autoar_create_do_write_data (arcreate, entry, file_to_read);
      g_hash_table_remove (priv->pathname_to_g_file, pathname_in_entry);
      /* We have registered g_object_unref function to free the GFile object,
       * so we do not have to unref it here. */
    }

    if (sparse != NULL) {
      GFile *file_to_read;
      const char *pathname_in_entry;
      pathname_in_entry = archive_entry_pathname (entry);
      file_to_read = g_hash_table_lookup (priv->pathname_to_g_file, pathname_in_entry);
      autoar_create_do_write_data (arcreate, sparse, file_to_read);
      g_hash_table_remove (priv->pathname_to_g_file, pathname_in_entry);
    }
  }

  g_object_unref (info);
}

static void
autoar_create_do_recursive_read (AutoarCreate *arcreate,
                                 GFile *root,
                                 GFile *file)
{
  GFileEnumerator *enumerator;
  GFileInfo *info;
  GFile *thisfile;
  const char *thisname;

  AutoarCreatePrivate *priv;

  priv = arcreate->priv;
  enumerator = g_file_enumerate_children (file,
                                          "standard::*",
                                          G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                          priv->cancellable,
                                          &(priv->error));
  if (enumerator == NULL)
    return;

  while ((info = g_file_enumerator_next_file (enumerator, priv->cancellable, &(priv->error))) != NULL) {
    thisname = g_file_info_get_name (info);
    thisfile = g_file_get_child (file, thisname);
    autoar_create_do_add_to_archive (arcreate, root, thisfile);
    if (priv->error != NULL) {
      g_object_unref (thisfile);
      g_object_unref (info);
      break;
    }

    if (g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY)
      autoar_create_do_recursive_read (arcreate, root, thisfile);
    g_object_unref (thisfile);
    g_object_unref (info);

    if (priv->error != NULL)
      break;
    if (g_cancellable_is_cancelled (priv->cancellable))
      break;
  }

  g_object_unref (enumerator);
}

static void
autoar_create_class_init (AutoarCreateClass *klass)
{
  GObjectClass *object_class;
  GType type;

  object_class = G_OBJECT_CLASS (klass);
  type = G_TYPE_FROM_CLASS (klass);

  g_type_class_add_private (klass, sizeof (AutoarCreatePrivate));

  object_class->get_property = autoar_create_get_property;
  object_class->set_property = autoar_create_set_property;
  object_class->dispose = autoar_create_dispose;
  object_class->finalize = autoar_create_finalize;

  g_object_class_install_property (object_class, PROP_SOURCE,
                                   g_param_spec_boxed ("source",
                                                       "Source archive",
                                                       "The source files and directories to be compressed",
                                                       G_TYPE_STRV,
                                                       G_PARAM_READWRITE |
                                                       G_PARAM_CONSTRUCT_ONLY |
                                                       G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (object_class, PROP_SOURCE_FILE,
                                   g_param_spec_boxed ("source-file",
                                                       "Source archive GFile",
                                                       "The source GFiles to be compressed",
                                                       G_TYPE_PTR_ARRAY,
                                                       G_PARAM_READWRITE |
                                                       G_PARAM_CONSTRUCT_ONLY |
                                                       G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (object_class, PROP_OUTPUT,
                                   g_param_spec_string ("output",
                                                        "Output directory",
                                                        "Output directory of created archive",
                                                        NULL,
                                                        G_PARAM_READWRITE |
                                                        G_PARAM_CONSTRUCT_ONLY |
                                                        G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (object_class, PROP_OUTPUT_FILE,
                                   g_param_spec_object ("output-file",
                                                        "Output directory GFile",
                                                        "Output directory GFile of created archive",
                                                        G_TYPE_FILE,
                                                        G_PARAM_READWRITE |
                                                        G_PARAM_CONSTRUCT_ONLY |
                                                        G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (object_class, PROP_SIZE, /* This propery is unused! */
                                   g_param_spec_uint64 ("size",
                                                        "Size",
                                                        "Total bytes will be read from disk",
                                                        0, G_MAXUINT64, 0,
                                                        G_PARAM_READWRITE |
                                                        G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (object_class, PROP_COMPLETED_SIZE,
                                   g_param_spec_uint64 ("completed-size",
                                                        "Read file size",
                                                        "Bytes read from disk",
                                                        0, G_MAXUINT64, 0,
                                                        G_PARAM_READWRITE |
                                                        G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (object_class, PROP_FILES,
                                   g_param_spec_uint ("files",
                                                      "Files",
                                                      "Number of files to be compressed",
                                                      0, G_MAXUINT32, 0,
                                                      G_PARAM_READWRITE |
                                                      G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (object_class, PROP_COMPLETED_FILES,
                                   g_param_spec_uint ("completed-files",
                                                      "Read files",
                                                      "Number of files has been read",
                                                      0, G_MAXUINT32, 0,
                                                      G_PARAM_READWRITE |
                                                      G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (object_class, PROP_OUTPUT_IS_DEST,
                                   g_param_spec_boolean ("output-is-dest",
                                                         "Output is destination",
                                                         "Whether output file is used as destination",
                                                         FALSE,
                                                         G_PARAM_READWRITE |
                                                         G_PARAM_STATIC_STRINGS));

  autoar_create_signals[DECIDE_DEST] =
    g_signal_new ("decide-dest",
                  type,
                  G_SIGNAL_RUN_LAST,
                  G_STRUCT_OFFSET (AutoarCreateClass, decide_dest),
                  NULL, NULL,
                  g_cclosure_marshal_generic,
                  G_TYPE_NONE,
                  1,
                  G_TYPE_FILE);

  autoar_create_signals[PROGRESS] =
    g_signal_new ("progress",
                  type,
                  G_SIGNAL_RUN_LAST,
                  G_STRUCT_OFFSET (AutoarCreateClass, progress),
                  NULL, NULL,
                  g_cclosure_marshal_generic,
                  G_TYPE_NONE,
                  2,
                  G_TYPE_UINT64,
                  G_TYPE_UINT);

  autoar_create_signals[CANCELLED] =
    g_signal_new ("cancelled",
                  type,
                  G_SIGNAL_RUN_LAST,
                  G_STRUCT_OFFSET (AutoarCreateClass, cancelled),
                  NULL, NULL,
                  g_cclosure_marshal_VOID__VOID,
                  G_TYPE_NONE,
                  0);

  autoar_create_signals[COMPLETED] =
    g_signal_new ("completed",
                  type,
                  G_SIGNAL_RUN_LAST,
                  G_STRUCT_OFFSET (AutoarCreateClass, completed),
                  NULL, NULL,
                  g_cclosure_marshal_VOID__VOID,
                  G_TYPE_NONE,
                  0);

  autoar_create_signals[ERROR] =
    g_signal_new ("error",
                  type,
                  G_SIGNAL_RUN_LAST,
                  G_STRUCT_OFFSET (AutoarCreateClass, error),
                  NULL, NULL,
                  g_cclosure_marshal_VOID__POINTER,
                  G_TYPE_NONE,
                  1,
                  G_TYPE_POINTER);
}

static void
autoar_create_init (AutoarCreate *arcreate)
{
  AutoarCreatePrivate *priv;

  priv = AUTOAR_CREATE_GET_PRIVATE (arcreate);
  arcreate->priv = priv;

  priv->source = NULL;
  priv->output = NULL;

  priv->source_file = NULL;
  priv->output_file = NULL;

  priv->size = 0;
  priv->completed_size = 0;

  priv->files = 0;
  priv->completed_files = 0;

  priv->arpref = NULL;

  priv->ostream = NULL;
  priv->buffer_size = BUFFER_SIZE;
  priv->buffer = g_new (char, priv->buffer_size);
  priv->error = NULL;

  priv->cancellable = NULL;

  priv->a = archive_write_new ();
  priv->entry = archive_entry_new ();
  priv->resolver = archive_entry_linkresolver_new ();;
  priv->pathname_to_g_file = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_object_unref);
  priv->source_basename_noext = NULL;
  priv->extension = NULL;

  priv->in_thread = FALSE;
  priv->prepend_basename = FALSE;
  priv->output_is_dest = FALSE;
}

static AutoarCreate*
autoar_create_new_full (const char **source,
                        GFile **source_file,
                        const char *output,
                        GFile *output_file,
                        gboolean output_is_dest,
                        AutoarPref *arpref)
{
  AutoarCreate *arcreate;
  const char **gen_source;
  char *gen_output;
  GPtrArray *gen_source_file;
  GFile *gen_output_file;
  int i;

  gen_source      = NULL;
  gen_source_file = NULL;
  gen_output      = NULL;
  gen_output_file = NULL;

  if (source == NULL) {
    GPtrArray *strv;
    strv = g_ptr_array_new ();
    for (i = 0; source_file[i] != NULL; i++)
      g_ptr_array_add (strv, autoar_common_g_file_get_name (source_file[i]));
    g_ptr_array_add (strv, NULL);
    gen_source = (const char**) g_ptr_array_free (strv, FALSE);
  }

  gen_source_file = g_ptr_array_new_with_free_func (g_object_unref);
  if (source_file == NULL) {
    for (i = 0; source[i] != NULL; i++)
      g_ptr_array_add (gen_source_file, g_file_new_for_commandline_arg (source[i]));
  } else {
    for (i = 0; source_file[i] != NULL; i++)
      g_ptr_array_add (gen_source_file, g_object_ref (source_file[i]));
  }

  if (output == NULL)
    gen_output = autoar_common_g_file_get_name (output_file);
  if (output_file == NULL)
    gen_output_file = g_file_new_for_commandline_arg (output);

  arcreate =
    g_object_new (AUTOAR_TYPE_CREATE,
                  "source",         source      != NULL ? source      : gen_source,
                  "source-file",                                        gen_source_file,
                  "output",         output      != NULL ? output      : gen_output,
                  "output-file",    output_file != NULL ? output_file : gen_output_file,
                  "output-is-dest", output_is_dest, NULL);
  arcreate->priv->arpref = g_object_ref (arpref);

  g_strfreev ((char**)gen_source);
  g_free (gen_output);

  if (gen_source_file != NULL)
    g_ptr_array_unref (gen_source_file);
  if (gen_output_file != NULL)
    g_object_unref (gen_output_file);

  return arcreate;
}

AutoarCreate*
autoar_create_new (AutoarPref *arpref,
                   const char *output,
                   ...)
{
  AutoarCreate *arcreate;
  char *str;
  va_list ap;
  GPtrArray *strv;

  g_return_val_if_fail (output != NULL, NULL);

  va_start (ap, output);
  strv = g_ptr_array_new_with_free_func (NULL);
  while ((str = va_arg (ap, char*)) != NULL)
    g_ptr_array_add (strv, str);
  g_ptr_array_add (strv, NULL);
  va_end (ap);

  arcreate = autoar_create_new_full ((const char**) strv->pdata, NULL,
                                     output, NULL,
                                     FALSE, arpref);
  g_ptr_array_unref (strv);
  return arcreate;
}

AutoarCreate*
autoar_create_new_file (AutoarPref *arpref,
                        GFile      *output_file,
                        ...)
{
  AutoarCreate *arcreate;
  GFile *file;
  va_list ap;
  GPtrArray *filev;

  g_return_val_if_fail (output_file != NULL, NULL);

  va_start (ap, output_file);
  filev = g_ptr_array_new_with_free_func (NULL);
  while ((file = va_arg (ap, GFile*)) != NULL)
    g_ptr_array_add (filev, file);
  g_ptr_array_add (filev, NULL);
  va_end (ap);

  arcreate = autoar_create_new_full (NULL, (GFile**)filev->pdata,
                                     NULL, output_file,
                                     FALSE, arpref);
  g_ptr_array_unref (filev);
  return arcreate;
}

AutoarCreate*
autoar_create_newv (AutoarPref  *arpref,
                    const char  *output,
                    const char **source)
{
  g_return_val_if_fail (source != NULL, NULL);
  g_return_val_if_fail (*source != NULL, NULL);
  g_return_val_if_fail (output != NULL, NULL);

  return autoar_create_new_full (source, NULL,
                                 output, NULL,
                                 FALSE, arpref);

}

AutoarCreate*
autoar_create_new_filev (AutoarPref  *arpref,
                         GFile       *output_file,
                         GFile      **source_file)
{
  g_return_val_if_fail (source_file != NULL, NULL);
  g_return_val_if_fail (*source_file != NULL, NULL);
  g_return_val_if_fail (output_file != NULL, NULL);

  return autoar_create_new_full (NULL, source_file,
                                 NULL, output_file,
                                 FALSE, arpref);
}

static void
autoar_create_step_initialize_object (AutoarCreate *arcreate)
{
  /* Step 0: Setup the libarchive object and the file name extension */

  AutoarFormat format;
  AutoarFilter filter;

  AutoarCreatePrivate *priv;

  priv = arcreate->priv;

  format = autoar_pref_get_default_format (priv->arpref);
  if (!autoar_format_is_valid (format)) {
    priv->error = g_error_new (AUTOAR_CREATE_ERROR, INVALID_FORMAT,
                               "Format %d is invalid", format);
    return;
  }

  filter = autoar_pref_get_default_filter (priv->arpref);
  if (!autoar_filter_is_valid (filter)) {
    priv->error = g_error_new (AUTOAR_CREATE_ERROR, INVALID_FILTER,
                               "Filter %d is invalid", filter);
    return;
  }

  priv->extension = autoar_format_filter_get_extension (format, filter);

  archive_write_set_bytes_in_last_block (priv->a, 1);
  archive_write_add_filter (priv->a, autoar_filter_get_filter_libarchive (filter));
  archive_write_set_format (priv->a, autoar_format_get_format_libarchive (format));
}

static void
autoar_create_step_decide_dest (AutoarCreate *arcreate)
{
  /* Step 1: Set the destination file name
   * Use the first source file name */

  AutoarCreatePrivate *priv;

  g_debug ("autoar_create_step_decide_dest: called");

  priv = arcreate->priv;

  {
    GFile *file_source; /* Do not unref */
    GFileInfo *source_info;
    char *source_basename;

    file_source = g_ptr_array_index (priv->source_file, 0);
    source_info = g_file_query_info (file_source,
                                     G_FILE_ATTRIBUTE_STANDARD_TYPE,
                                     G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                     priv->cancellable,
                                     &(priv->error));
    if (source_info == NULL)
      return;

    source_basename = g_file_get_basename (file_source);
    if (g_file_info_get_file_type (source_info) == G_FILE_TYPE_REGULAR)
      priv->source_basename_noext = autoar_common_get_basename_remove_extension (source_basename);
    else
      priv->source_basename_noext = g_strdup (source_basename);

    g_object_unref (source_info);
    g_free (source_basename);
  }

  {
    char *dest_basename;
    int i;

    dest_basename = g_strconcat (priv->source_basename_noext,
                                 priv->extension, NULL);
    priv->dest = g_file_get_child (priv->output_file, dest_basename);

    for (i = 1; g_file_query_exists (priv->dest, priv->cancellable); i++) {
      g_free (dest_basename);
      g_object_unref (priv->dest);

      if (g_cancellable_is_cancelled (priv->cancellable))
        return;

      dest_basename = g_strdup_printf ("%s(%d)%s",
                                       priv->source_basename_noext,
                                       i, priv->extension);
      priv->dest = g_file_get_child (priv->output_file, dest_basename);
    }

    g_free (dest_basename);
  }

  if (!g_file_query_exists (priv->output_file, priv->cancellable)) {
    g_file_make_directory_with_parents (priv->output_file,
                                        priv->cancellable,
                                        &(priv->error));
    if (priv->error != NULL)
      return;
  }

  autoar_create_signal_decide_dest (arcreate);
}

static void
autoar_create_step_decide_dest_already (AutoarCreate *arcreate)
{
  /* Alternative step 1: Output is destination */
  arcreate->priv->dest = g_object_ref (arcreate->priv->output_file);
  autoar_create_signal_decide_dest (arcreate);
}

static void
autoar_create_step_create (AutoarCreate *arcreate)
{
  /* Step 2: Create and open the new archive file */
  AutoarCreatePrivate *priv;
  int i, r;

  g_debug ("autoar_create_step_create: called");

  priv = arcreate->priv;

  r = archive_write_open (priv->a, arcreate,
                          libarchive_write_open_cb,
                          libarchive_write_write_cb,
                          libarchive_write_close_cb);
  if (r != ARCHIVE_OK) {
    if (priv->error == NULL)
      priv->error = autoar_common_g_error_new_a (priv->a, NULL);
    return;
  }

  /* Check whether we have multiple source files */
  if (priv->source[1] == NULL)
    priv->prepend_basename = FALSE;
  else
    priv->prepend_basename = TRUE;

  archive_entry_linkresolver_set_strategy (priv->resolver, archive_format (priv->a));

  for (i = 0; i < priv->source_file->len; i++) {
    GFile *file; /* Do not unref */
    GFileType filetype;
    GFileInfo *fileinfo;

    g_debug ("autoar_create_step_create: source[%d] (%s)", i, priv->source[i]);
    file = g_ptr_array_index (priv->source_file, i);
    fileinfo = g_file_query_info (file,
                                  G_FILE_ATTRIBUTE_STANDARD_TYPE,
                                  G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                  priv->cancellable,
                                  &(priv->error));
    if (priv->error != NULL)
      return;

    filetype = g_file_info_get_file_type (fileinfo);
    g_object_unref (fileinfo);

    autoar_create_do_add_to_archive (arcreate, file, file);

    if (filetype == G_FILE_TYPE_DIRECTORY)
      autoar_create_do_recursive_read (arcreate, file, file);

    if (priv->error != NULL)
      return;

    if (g_cancellable_is_cancelled (priv->cancellable))
      return;
  }

  /* Process the final entry */
  {
    struct archive_entry *entry, *sparse;
    entry = NULL;
    archive_entry_linkify (priv->resolver, &entry, &sparse);
    if (entry != NULL) {
      GFile *file_to_read;
      const char *pathname_in_entry;
      pathname_in_entry = archive_entry_pathname (entry);
      file_to_read = g_hash_table_lookup (priv->pathname_to_g_file, pathname_in_entry);
      autoar_create_do_write_data (arcreate, entry, file_to_read);
      /* I think we do not have to remove the entry in the hash table now
       * because we are going to free the entire hash table. */
    }
  }
}

static void
autoar_create_step_cleanup (AutoarCreate *arcreate)
{
  /* Step 3: Close the libarchive object. We do not have to do other cleanup
   * because they are handled in dispose and finalize functions. */
  AutoarCreatePrivate *priv;
  priv = arcreate->priv;
  if (archive_write_close (priv->a) != ARCHIVE_OK) {
    if (priv->error == NULL)
      priv->error = autoar_common_g_error_new_a (priv->a, priv->output);
    return;
  }
}

static void
autoar_create_run (AutoarCreate *arcreate)
{
  /* Numbers of steps.
   * The array size must be modified if more steps are added. */
  void (*steps[5])(AutoarCreate*);

  AutoarCreatePrivate *priv;
  int i;

  g_return_if_fail (AUTOAR_IS_CREATE (arcreate));
  priv = arcreate->priv;

  g_return_if_fail (priv->source_file != NULL);
  g_return_if_fail (priv->output_file != NULL);

  /* A GFile* array without a GFile* is not allowed */
  g_return_if_fail (*((GFile**)(priv->source_file->pdata)) != NULL);

  if (g_cancellable_is_cancelled (priv->cancellable)) {
    autoar_create_signal_cancelled (arcreate);
    return;
  }

  i = 0;
  steps[i++] = autoar_create_step_initialize_object;
  steps[i++] = priv->output_is_dest ?
               autoar_create_step_decide_dest_already :
               autoar_create_step_decide_dest;
  steps[i++] = autoar_create_step_create;
  steps[i++] = autoar_create_step_cleanup;
  steps[i++] = NULL;

  for (i = 0; steps[i] != NULL; i++) {
    g_debug ("autoar_create_run: Step %d Begin", i);
    (*steps[i])(arcreate);
    g_debug ("autoar_create_run: Step %d End", i);
    if (priv->error != NULL) {
      autoar_create_signal_error (arcreate);
      return;
    }
    if (g_cancellable_is_cancelled (priv->cancellable)) {
      autoar_create_signal_cancelled (arcreate);
      return;
    }
  }

  autoar_create_signal_completed (arcreate);
}

void
autoar_create_start (AutoarCreate *arcreate,
                     GCancellable *cancellable)
{
  if (cancellable != NULL)
    g_object_ref (cancellable);
  arcreate->priv->cancellable = cancellable;
  arcreate->priv->in_thread = FALSE;
  autoar_create_run (arcreate);
}

static void
autoar_create_start_async_thread (GTask *task,
                                  gpointer source_object,
                                  gpointer task_data,
                                  GCancellable *cancellable)
{
  AutoarCreate *arcreate = source_object;
  autoar_create_run (arcreate);
  g_task_return_pointer (task, NULL, g_free);
  g_object_unref (arcreate);
  g_object_unref (task);
}

void
autoar_create_start_async (AutoarCreate *arcreate,
                           GCancellable *cancellable)
{
  GTask *task;

  g_object_ref (arcreate);
  if (cancellable != NULL)
    g_object_ref (cancellable);
  arcreate->priv->cancellable = cancellable;
  arcreate->priv->in_thread = TRUE;

  task = g_task_new (arcreate, NULL, NULL, NULL);
  g_task_set_task_data (task, NULL, NULL);
  g_task_run_in_thread (task, autoar_create_start_async_thread);
}
