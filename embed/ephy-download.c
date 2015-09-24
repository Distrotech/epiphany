/* vim: set sw=2 ts=2 sts=2 et: */
/* -*- Mode: C; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/*
 * ephy-download.c
 * This file is part of Epiphany
 *
 * Copyright © 2011 - Igalia S.L.
 *
 * Epiphany is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Epiphany is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Epiphany; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA  02110-1301  USA
 */

#include "config.h"

#include "ephy-debug.h"
#include "ephy-download.h"
#include "ephy-embed.h"
#include "ephy-embed-shell.h"
#include "ephy-embed-type-builtins.h"
#include "ephy-file-helpers.h"
#include "ephy-prefs.h"
#include "ephy-settings.h"

#include <errno.h>
#include <glib/gi18n.h>
#include <string.h>

struct _EphyDownload
{
  GObject parent;

  WebKitDownload *download;

  char *destination;
  char *source;

  EphyDownloadActionType action;
  guint32 start_time;

  GtkWindow *window;
  GtkWidget *widget;

  guint inhibitor_cookie;
};

G_DEFINE_TYPE (EphyDownload, ephy_download, G_TYPE_OBJECT)

enum
{
  PROP_0,
  PROP_DOWNLOAD,
  PROP_DESTINATION,
  PROP_ACTION,
  PROP_START_TIME,
  PROP_WINDOW,
  PROP_WIDGET,
  LAST_PROP
};

static GParamSpec *obj_properties[LAST_PROP];

static void
ephy_download_get_property (GObject    *object,
                            guint       property_id,
                            GValue     *value,
                            GParamSpec *pspec)
{
  EphyDownload *download;

  download = EPHY_DOWNLOAD (object);

  switch (property_id) {
    case PROP_WIDGET:
      g_value_set_object (value, download->widget);
      break;
    case PROP_WINDOW:
      g_value_set_object (value, download->window);
      break;
    case PROP_DOWNLOAD:
      g_value_set_object (value, ephy_download_get_webkit_download (download));
      break;
    case PROP_DESTINATION:
      g_value_set_string (value, ephy_download_get_destination_uri (download));
      break;
    case PROP_ACTION:
      g_value_set_enum (value, ephy_download_get_action (download));
      break;
    case PROP_START_TIME:
      g_value_set_uint (value, ephy_download_get_start_time (download));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}

static void
ephy_download_set_property (GObject      *object,
                            guint         property_id,
                            const GValue *value,
                            GParamSpec   *pspec)
{
  EphyDownload *download;
  download = EPHY_DOWNLOAD (object);

  switch (property_id) {
    case PROP_DESTINATION:
      ephy_download_set_destination_uri (download, g_value_get_string (value));
      break;
    case PROP_ACTION:
      ephy_download_set_action (download, g_value_get_enum (value));
      break;
    case PROP_WINDOW:
      download->window = g_value_dup_object (value);
      break;
    case PROP_WIDGET:
      ephy_download_set_widget (download, g_value_get_object (value));
      break;
    case PROP_DOWNLOAD:
    case PROP_START_TIME:
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}

/**
 * ephy_download_get_content_type:
 * @download: an #EphyDownload
 *
 * Gets content-type information for @download. If the server didn't
 * provide a content type, the destination file is queried.
 *
 * Returns: content-type for @download, must be freed with g_free()
 **/
char *
ephy_download_get_content_type (EphyDownload *download)
{
  WebKitURIResponse *response;
  const char *destination_uri;
  GFile *destination;
  GFileInfo *info;
  char *content_type = NULL;
  GError *error = NULL;

  response = webkit_download_get_response (download->download);
  if (response) {
    content_type = g_strdup (webkit_uri_response_get_mime_type (response));

    LOG ("ephy_download_get_content_type: WebKit: %s", content_type);

    if (content_type)
      return content_type;
  }

  destination_uri = webkit_download_get_destination (download->download);
  if (!destination_uri)
    return NULL;

  destination = g_file_new_for_uri (destination_uri);
  info = g_file_query_info (destination, G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE,
                            G_FILE_QUERY_INFO_NONE, NULL, &error);
  if (info) {
    content_type = g_strdup (g_file_info_get_content_type (info));
    LOG ("ephy_download_get_content_type: GIO: %s", content_type);
    g_object_unref (info);
  } else {
    LOG ("ephy_download_get_content_type: error getting file "
         "content-type: %s", error->message);
    g_error_free (error);
  }

  g_object_unref (destination);

  return content_type;
}


/* Helper function to decide what EphyDownloadActionType should be the
 * default for the download. This implies that you want something to
 * happen, this function will never return EPHY_DOWNLOAD_ACTION_NONE.
 */
static EphyDownloadActionType
decide_action_from_mime (EphyDownload *ephy_download)
{
  char *content_type;
  GAppInfo *helper_app = NULL;
  EphyDownloadActionType action;

  content_type = ephy_download_get_content_type (ephy_download);
  if (content_type) {
    helper_app = g_app_info_get_default_for_type (content_type, FALSE);
    if (helper_app)
      action = EPHY_DOWNLOAD_ACTION_OPEN;

    g_free (content_type);
  }

  /* Downloads that have no content_type, or no helper_app, are
   * considered unsafe/unable to open. Default them to BROWSE_TO.
   */
  if (helper_app == NULL)
    action = EPHY_DOWNLOAD_ACTION_BROWSE_TO;
  else
    g_object_unref (helper_app);

  return action;
}

/* From the old embed/mozilla/MozDownload.cpp */
static const char*
file_is_compressed (const char *filename)
{
  int i;
  static const char * const compression[] = {".gz", ".bz2", ".Z", ".lz", ".xz", NULL};

  for (i = 0; compression[i] != NULL; i++) {
    if (g_str_has_suffix (filename, compression[i]))
      return compression[i];
  }

  return NULL;
}

static const char*
parse_extension (const char *filename)
{
  const char *compression;
  const char *last_separator;

  compression = file_is_compressed (filename);

  /* if the file is compressed we might have a double extension */
  if (compression != NULL) {
    int i;
    static const char * const extensions[] = {"tar", "ps", "xcf", "dvi", "txt", "text", NULL};

    for (i = 0; extensions[i] != NULL; i++) {
      char *suffix;
      suffix = g_strdup_printf (".%s%s", extensions[i], compression);

      if (g_str_has_suffix (filename, suffix)) {
        char *p;

        p = g_strrstr (filename, suffix);
        g_free (suffix);

        return p;
      }

      g_free (suffix);
    }
  }

  /* no compression, just look for the last dot in the filename */
  last_separator = strrchr (filename, G_DIR_SEPARATOR);
  return strrchr ((last_separator) ? last_separator : filename, '.');
}

static gboolean
set_destination_uri_for_suggested_filename (EphyDownload *download, const char *suggested_filename)
{
  char *dest_dir;
  char *dest_name;
  char *destination_filename;
  char *destination_uri;

  dest_dir = ephy_file_get_downloads_dir ();

  /* Make sure the download directory exists */
  if (g_mkdir_with_parents (dest_dir, 0700) == -1) {
    g_critical ("Could not create downloads directory \"%s\": %s",
                dest_dir, strerror (errno));
    g_free (dest_dir);
    return FALSE;
  }

  if (suggested_filename != NULL) {
    dest_name = ephy_sanitize_filename (g_strdup (suggested_filename));
  } else {
    dest_name = ephy_file_tmp_filename (".ephy-download-XXXXXX", NULL);
  }

  destination_filename = g_build_filename (dest_dir, dest_name, NULL);
  g_free (dest_dir);
  g_free (dest_name);

  /* Append (n) as needed. */
  if (g_file_test (destination_filename, G_FILE_TEST_EXISTS)) {
    int i = 1;
    const char *dot_pos;
    gssize position;
    char *serial = NULL;
    GString *tmp_filename;

    dot_pos = parse_extension (destination_filename);
    if (dot_pos)
      position = dot_pos - destination_filename;
    else
      position = strlen (destination_filename);

    tmp_filename = g_string_new (NULL);

    do {
      serial = g_strdup_printf ("(%d)", i++);

      g_string_assign (tmp_filename, destination_filename);
      g_string_insert (tmp_filename, position, serial);

      g_free (serial);
    } while (g_file_test (tmp_filename->str, G_FILE_TEST_EXISTS));

    destination_filename = g_strdup (tmp_filename->str);
    g_string_free (tmp_filename, TRUE);
  }

  destination_uri = g_filename_to_uri (destination_filename, NULL, NULL);
  g_free (destination_filename);

  g_assert (destination_uri);
  webkit_download_set_destination (download->download, destination_uri);
  g_free (destination_uri);

  return TRUE;
}

/**
 * ephy_download_set_destination_uri:
 * @download: an #EphyDownload
 * @destination: URI where to save @download
 *
 * Sets the destination URI of @download. It must be a proper URI, with a
 * scheme like file:/// or similar.
 **/
void
ephy_download_set_destination_uri (EphyDownload *download,
                                   const char *destination)
{
  g_return_if_fail (EPHY_IS_DOWNLOAD (download));
  g_return_if_fail (destination != NULL);

  webkit_download_set_destination (download->download, destination);
  g_object_notify_by_pspec (G_OBJECT (download), obj_properties[PROP_DESTINATION]);
}

/**
 * ephy_download_set_action:
 * @download: an #EphyDownload
 * @action: #EphyDownloadActionType to execute
 *
 * Sets the @action to be executed when ephy_download_do_download_action () is
 * called on @download or on finish when "Automatically download and open
 * files" is set.
 **/
void
ephy_download_set_action (EphyDownload *download,
                          EphyDownloadActionType action)
{
  g_return_if_fail (EPHY_IS_DOWNLOAD (download));

  download->action = action;
  g_object_notify_by_pspec (G_OBJECT (download), obj_properties[PROP_ACTION]);
}

/**
 * ephy_download_set_widget:
 * @download: an #EphyDownload
 * @widget: a #GtkWidget
 *
 * Sets @widget to be associated with @download as its UI.
 **/
void
ephy_download_set_widget (EphyDownload *download,
                          GtkWidget *widget)
{
  g_return_if_fail (EPHY_IS_DOWNLOAD (download));

  g_clear_object(&download->widget);

  if (widget != NULL)
    download->widget = g_object_ref (widget);

  g_object_notify_by_pspec (G_OBJECT (download), obj_properties[PROP_WIDGET]);
}

/**
 * ephy_download_get_widget:
 * @download: an #EphyDownload
 *
 * Gets the #GtkWidget associated to this download.
 *
 * Returns: (transfer none): a #GtkWidget.
 **/
GtkWidget *
ephy_download_get_widget (EphyDownload *download)
{
  g_return_val_if_fail (EPHY_IS_DOWNLOAD (download), NULL);

  return download->widget;
}

/**
 * ephy_download_get_webkit_download:
 * @download: an #EphyDownload
 *
 * Gets the #WebKitDownload being wrapped by @download.
 *
 * Returns: (transfer none): a #WebKitDownload.
 **/
WebKitDownload *
ephy_download_get_webkit_download (EphyDownload *download)
{
  g_return_val_if_fail (EPHY_IS_DOWNLOAD (download), NULL);

  return download->download;
}

/**
 * ephy_download_get_window:
 * @download: an #EphyDownload
 *
 * Gets the window set as the parent of @download, this can be %NULL if no
 * specific window generated this download.
 *
 * Returns: (transfer none): a #GtkWindow
 **/
GtkWindow *
ephy_download_get_window (EphyDownload *download)
{
  g_return_val_if_fail (EPHY_IS_DOWNLOAD (download), NULL);

  return download->window;
}

/**
 * ephy_download_get_destination_uri:
 * @download: an #EphyDownload
 *
 * Gets the destination URI where the download is being saved.
 *
 * Returns: (transfer none): destination URI.
 **/
const char *
ephy_download_get_destination_uri (EphyDownload *download)
{
  g_return_val_if_fail (EPHY_IS_DOWNLOAD (download), NULL);

  return webkit_download_get_destination (download->download);
}

/**
 * ephy_download_get_action:
 * @download: an #EphyDownload
 *
 * Gets the #EphyDownloadActionType that this download will execute when
 * ephy_download_do_download_action () is called on it. This action is
 * performed automatically is "Automatically download and open files" is
 * enabled.
 *
 * Returns: the #EphyDownloadActionType to be executed
 **/
EphyDownloadActionType
ephy_download_get_action (EphyDownload *download)
{
  g_return_val_if_fail (EPHY_IS_DOWNLOAD (download), EPHY_DOWNLOAD_ACTION_NONE);

  return download->action;
}

/**
 * ephy_download_get_start_time:
 * @download: an #EphyDownload
 *
 * Gets the time (returned by gtk_get_current_event_time ()) when @download was
 * started. Defaults to 0.
 *
 * Returns: the time when @download was started.
 **/
guint32
ephy_download_get_start_time (EphyDownload *download)
{
  g_return_val_if_fail (EPHY_IS_DOWNLOAD (download), 0);

  return download->start_time;
}

static void
acquire_session_inhibitor (EphyDownload *download)
{
  EphyEmbedShell *shell;

  shell = ephy_embed_shell_get_default ();

  if (download->inhibitor_cookie)
    return;

  download->inhibitor_cookie = gtk_application_inhibit (GTK_APPLICATION (shell),
                                                        download->window,
                                                        GTK_APPLICATION_INHIBIT_LOGOUT | GTK_APPLICATION_INHIBIT_SUSPEND,
                                                        "Downloading");
}

static void
release_session_inhibitor (EphyDownload *download)
{
  EphyEmbedShell *shell;

  shell = ephy_embed_shell_get_default ();

  if (!download->inhibitor_cookie)
    return;

  gtk_application_uninhibit (GTK_APPLICATION (shell), download->inhibitor_cookie);
  download->inhibitor_cookie = 0;
}

/**
 * ephy_download_cancel:
 * @download: an #EphyDownload
 *
 * Cancels the wrapped #WebKitDownload.
 **/
void
ephy_download_cancel (EphyDownload *download)
{
  g_return_if_fail (EPHY_IS_DOWNLOAD (download));

  webkit_download_cancel (download->download);
}

/**
 * ephy_download_do_download_action:
 * @download: an #EphyDownload
 * @action: one of #EphyDownloadActionType
 *
 * Executes the given @action for @download, this can be any of
 * #EphyDownloadActionType, including #EPHY_DOWNLOAD_ACTION_AUTO which decides
 * the default action from the mime type of @download.
 *
 * Returns: %TRUE if the action was executed succesfully.
 *
 **/
gboolean
ephy_download_do_download_action (EphyDownload *download,
                                  EphyDownloadActionType action)
{
    GFile *destination;
    const char *destination_uri;
    gboolean ret = FALSE;

    destination_uri = webkit_download_get_destination (download->download);
    destination = g_file_new_for_uri (destination_uri);

    switch ((action ? action : download->action)) {
      case EPHY_DOWNLOAD_ACTION_AUTO:
        LOG ("ephy_download_do_download_action: auto");
        ret = ephy_download_do_download_action (download, decide_action_from_mime (download));
        break;
      case EPHY_DOWNLOAD_ACTION_BROWSE_TO:
        LOG ("ephy_download_do_download_action: browse_to");
        ret = ephy_file_browse_to (destination, download->start_time);
        break;
      case EPHY_DOWNLOAD_ACTION_OPEN:
        LOG ("ephy_download_do_download_action: open");
        ret = ephy_embed_shell_launch_handler (ephy_embed_shell_get_default (), 
                                               destination, NULL, download->start_time);
        break;
      case EPHY_DOWNLOAD_ACTION_NONE:
        LOG ("ephy_download_do_download_action: none");
        ret = TRUE;
        break;
      case EPHY_DOWNLOAD_ACTION_DO_NOTHING:
        LOG ("ephy_download_do_download_action: nothing");
        ret = TRUE;
        break;
      default:
        LOG ("ephy_download_do_download_action: unhandled action");
        ret = FALSE;
        break;
    }
    g_object_unref (destination);

    return ret;
}

static void
ephy_download_dispose (GObject *object)
{
  EphyDownload *download = EPHY_DOWNLOAD (object);

  LOG ("EphyDownload disposed %p", object);

  release_session_inhibitor (download);

  if (download->download)
    g_signal_handlers_disconnect_matched (download->download, G_SIGNAL_MATCH_DATA, 0, 0, 0, 0, download);

  g_clear_object(&download->download);
  g_clear_object(&download->window);
  g_clear_object(&download->widget);

  G_OBJECT_CLASS (ephy_download_parent_class)->dispose (object);
}

static void
ephy_download_class_init (EphyDownloadClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->get_property = ephy_download_get_property;
  object_class->set_property = ephy_download_set_property;
  object_class->dispose = ephy_download_dispose;

  /**
   * EphyDownload::download:
   *
   * Internal WebKitDownload.
   */
  obj_properties[PROP_DOWNLOAD] =
    g_param_spec_object ("download",
                         "Internal WebKitDownload",
                         "The WebKitDownload used internally by EphyDownload",
                         WEBKIT_TYPE_DOWNLOAD,
                         G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  /**
   * EphyDownload::destination:
   *
   * The destination URI where to store the download.
   */
  obj_properties[PROP_DESTINATION] =
    g_param_spec_string ("destination",
                         "Destination",
                         "Destination file URI",
                         NULL,
                         G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  /**
   * EphyDownload::action:
   *
   * Action to take when the download finishes and "Automatically download and
   * open files" is enabled, or when ephy_download_do_download_action () is
   * called.
   */
  obj_properties[PROP_ACTION] =
    g_param_spec_enum ("action",
                       "Download action",
                       "Action to take when download finishes",
                       EPHY_TYPE_DOWNLOAD_ACTION_TYPE,
                       EPHY_DOWNLOAD_ACTION_NONE,
                       G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  /**
   * EphyDownload::start-time:
   *
   * User time when the download started, useful for launching applications
   * aware of focus stealing.
   */
  obj_properties[PROP_START_TIME] =
    g_param_spec_uint ("start-time",
                       "Event start time",
                       "Time for focus-stealing prevention.",
                       0, G_MAXUINT32, 0,
                       G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  /**
   * EphyDownload:window:
   *
   * Window that produced the download, the download will be shown in its
   * parent window.
   */
  obj_properties[PROP_WINDOW] =
    g_param_spec_object ("window",
                         "A GtkWindow",
                         "Window that produced this download.",
                         GTK_TYPE_WINDOW,
                         G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);

  /**
   * EphyDownload::widget:
   *
   * An EphyDownloadWidget -or any other GtkWidget- that is representing this
   * EphyDownload to the user.
   */
  obj_properties[PROP_WIDGET] =
    g_param_spec_object ("widget",
                         "A GtkWidget",
                         "GtkWidget showing this download.",
                         GTK_TYPE_WIDGET,
                         G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, LAST_PROP, obj_properties);

  /**
   * EphyDownload::completed:
   *
   * The ::filename-suggested signal is emitted when we have received the
   * suggested filename from WebKit.
   **/
  g_signal_new ("filename-suggested",
                G_OBJECT_CLASS_TYPE (object_class),
                G_SIGNAL_RUN_LAST,
                NULL, NULL, NULL, NULL,
                G_TYPE_NONE,
                1,
                G_TYPE_STRING | G_SIGNAL_TYPE_STATIC_SCOPE);

  /**
   * EphyDownload::completed:
   *
   * The ::completed signal is emitted when @download has finished downloading.
   **/
  g_signal_new ("completed",
                G_OBJECT_CLASS_TYPE (object_class),
                G_SIGNAL_RUN_LAST,
                NULL, NULL, NULL, NULL,
                G_TYPE_NONE,
                0);
  /**
   * EphyDownload::error:
   *
   * The ::error signal wraps the @download ::error signal.
   **/
  g_signal_new ("error",
                G_OBJECT_CLASS_TYPE (object_class),
                G_SIGNAL_RUN_LAST,
                NULL, NULL, NULL, NULL,
                G_TYPE_NONE,
                0);
}

static void
ephy_download_init (EphyDownload *download)
{
  LOG ("EphyDownload initialising %p", download);

  download->download = NULL;

  download->action = EPHY_DOWNLOAD_ACTION_NONE;

  download->start_time = gtk_get_current_event_time ();

  download->window = NULL;
  download->widget = NULL;
}

static void
download_created_destination_cb (WebKitDownload *wk_download,
                                 const gchar *destination,
                                 EphyDownload *download)
{
  acquire_session_inhibitor (download);
}

static gboolean
download_decide_destination_cb (WebKitDownload *wk_download,
                                const gchar *suggested_filename,
                                EphyDownload *download)
{
  if (webkit_download_get_destination (wk_download))
    return TRUE;

  g_signal_emit_by_name (download, "filename-suggested", suggested_filename);

  if (webkit_download_get_destination (wk_download))
    return TRUE;

  return set_destination_uri_for_suggested_filename (download, suggested_filename);
}

static void
download_finished_cb (WebKitDownload *wk_download,
                      EphyDownload *download)
{
  g_signal_emit_by_name (download, "completed");

  if (g_settings_get_boolean (EPHY_SETTINGS_MAIN, EPHY_PREFS_AUTO_DOWNLOADS) &&
      download->action == EPHY_DOWNLOAD_ACTION_NONE)
    ephy_download_do_download_action (download, EPHY_DOWNLOAD_ACTION_AUTO);
  else
    ephy_download_do_download_action (download, download->action);

  release_session_inhibitor (download);
}

static void
download_failed_cb (WebKitDownload *wk_download,
                    GError *error,
                    EphyDownload *download)
{
  gboolean ret = FALSE;

  g_signal_handlers_disconnect_by_func (wk_download, download_finished_cb, download);

  LOG ("error (%d - %d)! %s", error->code, 0, error->message);
  g_signal_emit_by_name (download, "error", 0, error->code, error->message, &ret);

  release_session_inhibitor (download);
}

/**
 * ephy_download_new:
 * @download: a #WebKitDownload to wrap
 * @parent: the #GtkWindow parent of the download, or %NULL
 *
 * Wraps @download in an #EphyDownload.
 *
 * Returns: an #EphyDownload.
 **/
EphyDownload *
ephy_download_new (WebKitDownload *download,
                   GtkWindow *parent)
{
  EphyDownload *ephy_download;

  g_return_val_if_fail (WEBKIT_IS_DOWNLOAD (download), NULL);

  ephy_download = g_object_new (EPHY_TYPE_DOWNLOAD, "window", parent, NULL);

  g_signal_connect (download, "created-destination",
                    G_CALLBACK (download_created_destination_cb),
                    ephy_download);
  g_signal_connect (download, "decide-destination",
                    G_CALLBACK (download_decide_destination_cb),
                    ephy_download);
  g_signal_connect (download, "finished",
                    G_CALLBACK (download_finished_cb),
                    ephy_download);
  g_signal_connect (download, "failed",
                    G_CALLBACK (download_failed_cb),
                    ephy_download);

  ephy_download->download = g_object_ref (download);
  g_object_set_data (G_OBJECT (download), "ephy-download-set", GINT_TO_POINTER (TRUE));

  return ephy_download;
}

/**
 * ephy_download_new_for_uri:
 * @uri: a source URI from where to download
 * @parent: the #GtkWindow parent of the download, or %NULL
 *
 * Creates an #EphyDownload to download @uri.
 *
 * Returns: an #EphyDownload.
 **/
EphyDownload *
ephy_download_new_for_uri (const char *uri,
                           GtkWindow *parent)
{
  EphyDownload *ephy_download;
  WebKitDownload *download;
  EphyEmbedShell *shell = ephy_embed_shell_get_default ();

  g_return_val_if_fail (uri != NULL, NULL);

  download = webkit_web_context_download_uri (ephy_embed_shell_get_web_context (shell), uri);

  ephy_download = ephy_download_new (download, parent);
  g_object_unref (download);

  return ephy_download;
}
