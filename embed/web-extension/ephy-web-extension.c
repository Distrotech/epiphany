/* -*- Mode: C; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set sw=2 ts=2 sts=2 et: */
/*
 *  Copyright © 2012 Igalia S.L.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#include "config.h"
#include "ephy-web-extension.h"

#include "ephy-debug.h"
#include "ephy-embed-form-auth.h"
#include "ephy-form-auth-data.h"
#include "ephy-prefs.h"
#include "ephy-settings.h"
#include "ephy-web-dom-utils.h"
#include "uri-tester.h"

#include <gio/gio.h>
#include <gtk/gtk.h>
#include <libsoup/soup.h>
#include <webkit2/webkit-web-extension.h>


/* FIXME: These global variables should be freed somehow. */
static UriTester *uri_tester;
static EphyFormAuthDataCache *form_auth_data_cache;
static GDBusConnection *dbus_connection;

static const char introspection_xml[] =
  "<node>"
  " <interface name='org.gnome.Epiphany.WebExtension'>"
  "  <method name='HasModifiedForms'>"
  "   <arg type='t' name='page_id' direction='in'/>"
  "   <arg type='b' name='has_modified_forms' direction='out'/>"
  "  </method>"
  "  <method name='GetWebAppTitle'>"
  "   <arg type='t' name='page_id' direction='in'/>"
  "   <arg type='s' name='title' direction='out'/>"
  "  </method>"
  "  <method name='GetBestWebAppIcon'>"
  "   <arg type='t' name='page_id' direction='in'/>"
  "   <arg type='s' name='base_uri' direction='in'/>"
  "   <arg type='b' name='result' direction='out'/>"
  "   <arg type='s' name='uri' direction='out'/>"
  "   <arg type='s' name='color' direction='out'/>"
  "  </method>"
  "  <signal name='FormAuthDataSaveConfirmationRequired'>"
  "   <arg type='u' name='request_id' direction='out'/>"
  "   <arg type='t' name='page_id' direction='out'/>"
  "   <arg type='s' name='hostname' direction='out'/>"
  "   <arg type='s' name='username' direction='out'/>"
  "  </signal>"
  "  <method name='FormAuthDataSaveConfirmationResponse'>"
  "   <arg type='u' name='request_id' direction='in'/>"
  "   <arg type='b' name='should_store' direction='in'/>"
  "  </method>"
  " </interface>"
  "</node>";


static gboolean
web_page_send_request (WebKitWebPage *web_page,
                       WebKitURIRequest *request,
                       WebKitURIResponse *redirected_response,
                       gpointer user_data)
{
  const char *request_uri;
  const char *page_uri;

  if (g_settings_get_boolean (EPHY_SETTINGS_WEB, EPHY_PREFS_WEB_DO_NOT_TRACK)) {
    SoupMessageHeaders *headers;

    headers = webkit_uri_request_get_http_headers (request);
    if (headers) {
      /* Do Not Track header. '1' means 'opt-out'. See:
       * http://tools.ietf.org/id/draft-mayer-do-not-track-00.txt */
      soup_message_headers_append (headers, "DNT", "1");
    }
  }

  if (!g_settings_get_boolean (EPHY_SETTINGS_WEB, EPHY_PREFS_WEB_ENABLE_ADBLOCK))
      return FALSE;

  request_uri = webkit_uri_request_get_uri (request);
  page_uri = webkit_web_page_get_uri (web_page);

  /* Always load the main resource. */
  if (g_strcmp0 (request_uri, page_uri) == 0)
    return FALSE;

  /* Always load data requests, as uri_tester won't do any good here. */
  if (g_str_has_prefix (request_uri, SOUP_URI_SCHEME_DATA))
      return FALSE;

  return uri_tester_test_uri (uri_tester, request_uri, page_uri, AD_URI_CHECK_TYPE_OTHER);
}

static GHashTable *
get_form_auth_data_save_requests (void)
{
  static GHashTable *form_auth_data_save_requests = NULL;

  if (!form_auth_data_save_requests) {
    form_auth_data_save_requests =
      g_hash_table_new_full (g_direct_hash,
                             g_direct_equal,
                             NULL,
                             (GDestroyNotify)g_object_unref);
  }

  return form_auth_data_save_requests;
}

static guint
form_auth_data_save_request_new_id (void)
{
  static guint form_auth_data_save_request_id = 0;

  return ++form_auth_data_save_request_id;
}

static void
store_password (EphyEmbedFormAuth *form_auth)
{
  SoupURI *uri;
  char *uri_str;
  char *username_field_name = NULL;
  char *username_field_value = NULL;
  char *password_field_name = NULL;
  char *password_field_value = NULL;

  g_object_get (ephy_embed_form_auth_get_username_node (form_auth),
                "name", &username_field_name,
                "value", &username_field_value,
                NULL);
  g_object_get (ephy_embed_form_auth_get_password_node (form_auth),
                "name", &password_field_name,
                "value", &password_field_value,
                NULL);

  uri = ephy_embed_form_auth_get_uri (form_auth);
  uri_str = soup_uri_to_string (uri, FALSE);
  ephy_form_auth_data_store (uri_str,
                             username_field_name,
                             password_field_name,
                             username_field_value,
                             password_field_value,
                             NULL, NULL);
  g_free (uri_str);

  /* Update internal caching */
  ephy_form_auth_data_cache_add (form_auth_data_cache,
                                 uri->host,
                                 username_field_name,
                                 password_field_name,
                                 username_field_value);

  g_free (username_field_name);
  g_free (username_field_value);
  g_free (password_field_name);
  g_free (password_field_value);
}

static void
request_decision_on_storing (EphyEmbedFormAuth *form_auth)
{
  char *username_field_value = NULL;
  guint request_id;
  SoupURI *uri;
  GError *error = NULL;

  if (!dbus_connection) {
    g_object_unref (form_auth);
    return;
  }

  request_id = form_auth_data_save_request_new_id ();
  uri = ephy_embed_form_auth_get_uri (form_auth);
  g_object_get (ephy_embed_form_auth_get_username_node (form_auth),
                "value", &username_field_value, NULL);

  g_dbus_connection_emit_signal (dbus_connection,
                                 NULL,
                                 EPHY_WEB_EXTENSION_OBJECT_PATH,
                                 EPHY_WEB_EXTENSION_INTERFACE,
                                 "FormAuthDataSaveConfirmationRequired",
                                 g_variant_new ("(utss)",
                                                request_id,
                                                ephy_embed_form_auth_get_page_id (form_auth),
                                                uri ? uri->host : "",
                                                username_field_value ? username_field_value : ""),
                                 &error);
  if (error) {
    g_warning ("Error emitting signal FormAuthDataSaveConfirmationRequired: %s\n", error->message);
    g_error_free (error);
  } else {
    g_hash_table_insert (get_form_auth_data_save_requests (),
                         GINT_TO_POINTER (request_id),
                         g_object_ref (form_auth));
  }

  g_free (username_field_value);
  g_object_unref (form_auth);
}

static void
should_store_cb (const char *username,
                 const char *password,
                 gpointer user_data)
{
  EphyEmbedFormAuth *form_auth = EPHY_EMBED_FORM_AUTH (user_data);

  if (username && password) {
    char *username_field_value = NULL;
    char *password_field_value = NULL;

    g_object_get (ephy_embed_form_auth_get_username_node (form_auth),
                  "value", &username_field_value, NULL);
    g_object_get (ephy_embed_form_auth_get_password_node (form_auth),
                  "value", &password_field_value, NULL);

    /* FIXME: We use only the first result, for now; We need to do
     * something smarter here */
    if (g_str_equal (username, username_field_value) &&
        g_str_equal (password, password_field_value)) {
      LOG ("User/password already stored. Not asking about storing.");
    } else {
      LOG ("User/password not yet stored. Asking about storing.");
      request_decision_on_storing (g_object_ref (form_auth));
    }

    g_free (username_field_value);
    g_free (password_field_value);
  } else {
    LOG ("No result on query; asking whether we should store.");
    request_decision_on_storing (g_object_ref (form_auth));
  }
}

static gboolean
form_submitted_cb (WebKitDOMHTMLFormElement *dom_form,
                   WebKitDOMEvent *dom_event,
                   WebKitWebPage *web_page)
{
  EphyEmbedFormAuth *form_auth;
  SoupURI *uri;
  WebKitDOMNode *username_node = NULL;
  WebKitDOMNode *password_node = NULL;
  char *username_field_name = NULL;
  char *username_field_value = NULL;
  char *password_field_name = NULL;
  char *uri_str;

  if (!ephy_web_dom_utils_find_form_auth_elements (dom_form, &username_node, &password_node))
    return TRUE;

  g_object_get (username_node,
                "value", &username_field_value,
                NULL);

  /* EphyEmbedFormAuth takes ownership of the nodes */
  form_auth = ephy_embed_form_auth_new (web_page, username_node, password_node, username_field_value);
  uri = ephy_embed_form_auth_get_uri (form_auth);
  soup_uri_set_query (uri, NULL);

  g_object_get (username_node, "name", &username_field_name, NULL);
  g_object_get (password_node, "name", &password_field_name, NULL);
  uri_str = soup_uri_to_string (uri, FALSE);

  ephy_form_auth_data_query (uri_str,
                             username_field_name,
                             password_field_name,
                             username_field_value,
                             should_store_cb,
                             form_auth,
                             (GDestroyNotify)g_object_unref);

  g_free (username_field_name);
  g_free (username_field_value);
  g_free (password_field_name);
  g_free (uri_str);

  return TRUE;
}

static void
fill_form_cb (const char *username,
              const char *password,
              gpointer user_data)
{
  EphyEmbedFormAuth *form_auth = EPHY_EMBED_FORM_AUTH (user_data);

  if (username == NULL && password == NULL) {
    LOG ("No result");
    return;
  }

  LOG ("Found: user %s pass (hidden)", username);
  g_object_set (ephy_embed_form_auth_get_username_node (form_auth),
                "value", username, NULL);
  g_object_set (ephy_embed_form_auth_get_password_node (form_auth),
                "value", password, NULL);
}

static gint
ephy_form_auth_data_compare (EphyFormAuthData *form_data,
                             EphyEmbedFormAuth *form_auth)
{
  char *username_field_name;
  char *password_field_name;
  gboolean retval;

  g_object_get (ephy_embed_form_auth_get_username_node (form_auth),
                "name", &username_field_name, NULL);
  g_object_get (ephy_embed_form_auth_get_password_node (form_auth),
                "name", &password_field_name, NULL);

  retval = g_strcmp0 (username_field_name, form_data->form_username) == 0 &&
    g_strcmp0 (password_field_name, form_data->form_password) == 0;

  g_free (username_field_name);
  g_free (password_field_name);

  return retval ? 0 : 1;
}

static void
pre_fill_form (EphyEmbedFormAuth *form_auth)
{
  GSList *form_auth_data_list;
  GSList *l;
  EphyFormAuthData *form_data;
  SoupURI *uri;
  char *uri_str;
  char *username;

  uri = ephy_embed_form_auth_get_uri (form_auth);
  if (!uri)
    return;

  form_auth_data_list = ephy_form_auth_data_cache_get_list (form_auth_data_cache, uri->host);
  l = g_slist_find_custom (form_auth_data_list, form_auth, (GCompareFunc)ephy_form_auth_data_compare);
  if (!l)
    return;

  form_data = (EphyFormAuthData *)l->data;
  uri_str = soup_uri_to_string (uri, FALSE);

  g_object_get (ephy_embed_form_auth_get_username_node (form_auth),
                "value", &username,
                NULL);

  /* The username node is empty, so pre-fill with the default. */
  if (g_str_equal (username, ""))
    g_clear_pointer (&username, g_free);

  ephy_form_auth_data_query (uri_str,
                             form_data->form_username,
                             form_data->form_password,
                             username,
                             fill_form_cb,
                             g_object_ref (form_auth),
                             (GDestroyNotify) g_object_unref);

  g_free (username);
  g_free (uri_str);
}

static void
remove_user_choices (WebKitDOMDocument *document)
{
  WebKitDOMHTMLElement *body;
  WebKitDOMElement *user_choices;

  body = webkit_dom_document_get_body (document);

  user_choices = webkit_dom_document_get_element_by_id (document, "ephy-user-choices-container");
  if (user_choices) {
    webkit_dom_node_remove_child (WEBKIT_DOM_NODE (body),
                                  WEBKIT_DOM_NODE (user_choices),
                                  NULL);
  }
}

static gboolean
username_changed_cb (WebKitDOMNode *username_node,
                     WebKitDOMEvent *dom_event,
                     EphyEmbedFormAuth *form_auth)
{
  pre_fill_form (form_auth);
  return TRUE;
}

static gboolean
user_chosen_cb (WebKitDOMNode  *li,
                WebKitDOMEvent *dom_event,
                WebKitDOMNode  *username_node)
{
  WebKitDOMElement *anchor;
  const char* username;

  anchor = webkit_dom_element_get_first_element_child (WEBKIT_DOM_ELEMENT (li));

  username = webkit_dom_node_get_text_content (WEBKIT_DOM_NODE (anchor));
  webkit_dom_html_input_element_set_value (WEBKIT_DOM_HTML_INPUT_ELEMENT (username_node), username);

  remove_user_choices (webkit_dom_node_get_owner_document (li));

  return TRUE;
}

GtkStyleContext *global_entry_context = NULL;
static GtkStyleContext*
get_entry_style_context ()
{
  GtkWidgetPath *path;

  if (global_entry_context)
    return global_entry_context;

  path = gtk_widget_path_new ();
  gtk_widget_path_append_type (path, GTK_TYPE_ENTRY);
  gtk_widget_path_iter_add_class (path, 0, GTK_STYLE_CLASS_ENTRY);

  global_entry_context = gtk_style_context_new ();
  gtk_style_context_set_path (global_entry_context, path);
  gtk_widget_path_free (path);

  return global_entry_context;
}

static char*
get_selected_bgcolor ()
{
  GdkRGBA color;
  gtk_style_context_get_background_color (get_entry_style_context (),
                                          GTK_STATE_FLAG_SELECTED,
                                          &color);
  return gdk_rgba_to_string (&color);
}

static char*
get_selected_fgcolor ()
{
  GdkRGBA color;
  gtk_style_context_get_color (get_entry_style_context (),
                               GTK_STATE_FLAG_SELECTED,
                               &color);
  return gdk_rgba_to_string (&color);
}

static char*
get_bgcolor ()
{
  GdkRGBA color;
  gtk_style_context_get_background_color (get_entry_style_context (),
                                          GTK_STATE_FLAG_NORMAL,
                                          &color);
  return gdk_rgba_to_string (&color);
}

static char*
get_fgcolor ()
{
  GdkRGBA color;
  gtk_style_context_get_color (get_entry_style_context (),
                               GTK_STATE_FLAG_NORMAL,
                               &color);
  return gdk_rgba_to_string (&color);
}

static char*
get_user_choice_style (gboolean selected)
{
  char *style_attribute;
  char *color;


  color = selected ? get_selected_bgcolor () : get_bgcolor ();

  style_attribute = g_strdup_printf ("list-style-type: none ! important;"
                                     "background-image: none ! important;"
                                     "padding: 3px 6px ! important;"
                                     "margin: 0px;"
                                     "background-color: %s;", color);

  g_free (color);

  return style_attribute;
}

static char*
get_user_choice_anchor_style (gboolean selected)
{
  char *style_attribute;
  char *color;

  color = selected ? get_selected_fgcolor () : get_fgcolor ();

  style_attribute = g_strdup_printf ("font-weight: normal ! important;"
                                     "font-family: sans ! important;"
                                     "text-decoration: none ! important;"
                                     "-webkit-user-modify: read-only ! important;"
                                     "color: %s;", color);

  g_free (color);

  return style_attribute;
}

void
show_user_choices (WebKitDOMDocument *document,
                   WebKitDOMNode     *username_node)
{
  WebKitDOMNode *body;
  WebKitDOMElement *main_div;
  WebKitDOMElement *ul;
  GSList *iter;
  GSList *auth_data_list;
  gboolean username_node_ever_edited;
  long x, y;
  long input_width;
  char *style_attribute;
  const char* username;

  g_object_get (username_node,
                "value", &username,
                "offset-width", &input_width,
                NULL);

  main_div = webkit_dom_document_create_element (document, "div", NULL);
  webkit_dom_element_set_attribute (main_div, "id", "ephy-user-choices-container", NULL);

  ephy_web_dom_utils_get_absolute_bottom_for_element (WEBKIT_DOM_ELEMENT (username_node), &x, &y);

  /* 2147483647 is the maximum value browsers will take for z-index.
   * See http://stackoverflow.com/questions/8565821/css-max-z-index-value
   */
  style_attribute = g_strdup_printf ("position: absolute; z-index: 2147483647;"
                                     "cursor: default;"
                                     "width: %ldpx;"
                                     "background-color: white;"
                                     "box-shadow: 5px 5px 5px black;"
                                     "border-top: 0;"
                                     "border-radius: 8px;"
                                     "-webkit-user-modify: read-only ! important;"
                                     "left: %ldpx; top: %ldpx;",
                                     input_width, x, y);

  webkit_dom_element_set_attribute (main_div, "style", style_attribute, NULL);
  g_free (style_attribute);

  ul = webkit_dom_document_create_element (document, "ul", NULL);
  webkit_dom_element_set_attribute (ul, "tabindex", "-1", NULL);
  webkit_dom_node_append_child (WEBKIT_DOM_NODE (main_div),
                                WEBKIT_DOM_NODE (ul),
                                NULL);

  webkit_dom_element_set_attribute (ul, "style",
                                    "margin: 0;"
                                    "padding: 0;",
                                    NULL);

  auth_data_list = (GSList*)g_object_get_data (G_OBJECT (username_node),
                                               "ephy-auth-data-list");

  username_node_ever_edited =
    GPOINTER_TO_INT (g_object_get_data (G_OBJECT (username_node),
                                        "ephy-user-ever-edited"));

  for (iter = auth_data_list; iter; iter = iter->next) {
    EphyFormAuthData *data;
    WebKitDOMElement *li;
    WebKitDOMElement *anchor;
    char *child_style;
    gboolean is_selected;

    data = (EphyFormAuthData*)iter->data;

    /* Filter out the available names that do not match, but show all options in
     * case we have been triggered by something other than the user editing the
     * input.
     */
    if (username_node_ever_edited && !g_str_has_prefix(data->username, username))
      continue;

    is_selected = !g_strcmp0 (username, data->username);

    li = webkit_dom_document_create_element (document, "li", NULL);
    webkit_dom_element_set_attribute (li, "tabindex", "-1", NULL);
    webkit_dom_node_append_child (WEBKIT_DOM_NODE (ul),
                                  WEBKIT_DOM_NODE (li),
                                  NULL);

    child_style = get_user_choice_style (is_selected);
    webkit_dom_element_set_attribute (li, "style", child_style, NULL);
    g_free (child_style);

    /* Store the selected node, if any for ease of querying which user
     * is currently selected.
     */
    if (is_selected)
      g_object_set_data (G_OBJECT (main_div), "ephy-user-selected", li);

    anchor = webkit_dom_document_create_element (document, "a", NULL);
    webkit_dom_node_append_child (WEBKIT_DOM_NODE (li),
                                  WEBKIT_DOM_NODE (anchor),
                                  NULL);

    child_style = get_user_choice_anchor_style (is_selected);
    webkit_dom_element_set_attribute (anchor, "style", child_style, NULL);
    g_free (child_style);

    webkit_dom_event_target_add_event_listener (WEBKIT_DOM_EVENT_TARGET (li), "mousedown",
                                                G_CALLBACK (user_chosen_cb), TRUE,
                                                username_node);

    webkit_dom_node_set_text_content (WEBKIT_DOM_NODE (anchor),
                                      data->username,
                                      NULL);
  }

  body = WEBKIT_DOM_NODE (webkit_dom_document_get_body (document));
  webkit_dom_node_append_child (WEBKIT_DOM_NODE (body),
                                WEBKIT_DOM_NODE (main_div),
                                NULL);
}

static gboolean
username_node_changed_cb (WebKitDOMNode  *username_node,
                          WebKitDOMEvent *dom_event,
                          WebKitWebPage  *web_page)
{
  WebKitDOMDocument *document;

  document = webkit_web_page_get_dom_document (web_page);
  remove_user_choices (document);

  return TRUE;
}

static gboolean
username_node_clicked_cb (WebKitDOMNode  *username_node,
                          WebKitDOMEvent *dom_event,
                          WebKitWebPage  *web_page)
{
  WebKitDOMDocument *document;

  document = webkit_web_page_get_dom_document (web_page);
  if (webkit_dom_document_get_element_by_id (document, "ephy-user-choices-container"))
    return TRUE;

  show_user_choices (document, username_node);

  return TRUE;
}

static void
clear_password_field (WebKitDOMNode *username_node)
{
  EphyEmbedFormAuth *form_auth;
  WebKitDOMNode *password_node;

  form_auth = (EphyEmbedFormAuth*)g_object_get_data (G_OBJECT (username_node),
                                                     "ephy-form-auth");

  password_node = ephy_embed_form_auth_get_password_node (form_auth);
  webkit_dom_html_input_element_set_value (WEBKIT_DOM_HTML_INPUT_ELEMENT (password_node), "");
}

static void
pre_fill_password (WebKitDOMNode *username_node)
{
  EphyEmbedFormAuth *form_auth;

  form_auth = (EphyEmbedFormAuth*)g_object_get_data (G_OBJECT (username_node),
                                                     "ephy-form-auth");

  pre_fill_form (form_auth);
}

static gboolean
username_node_keydown_cb (WebKitDOMNode  *username_node,
                          WebKitDOMEvent *dom_event,
                          WebKitWebPage  *web_page)
{
  WebKitDOMDocument *document;
  WebKitDOMElement *main_div;
  WebKitDOMElement *container;
  WebKitDOMElement *selected= NULL;
  WebKitDOMElement *to_select = NULL;
  WebKitDOMElement *anchor;
  WebKitDOMKeyboardEvent *keyboard_event;
  guint keyval = GDK_KEY_VoidSymbol;
  char *li_style_attribute;
  char *anchor_style_attribute;
  const char *username;

  keyboard_event = WEBKIT_DOM_KEYBOARD_EVENT (dom_event);
  document = webkit_web_page_get_dom_document (web_page);

  /* U+001B means the Esc key here; we should find a better way of testing which
   * key has been pressed.
   */
  if (!g_strcmp0 (webkit_dom_keyboard_event_get_key_identifier (keyboard_event), "Up"))
    keyval = GDK_KEY_Up;
  else if (!g_strcmp0 (webkit_dom_keyboard_event_get_key_identifier (keyboard_event), "Down"))
    keyval = GDK_KEY_Down;
  else if (!g_strcmp0 (webkit_dom_keyboard_event_get_key_identifier (keyboard_event), "U+001B")) {
    remove_user_choices (document);
    return TRUE;
  } else
    return TRUE;

  main_div = webkit_dom_document_get_element_by_id (document, "ephy-user-choices-container");

  if (!main_div) {
    show_user_choices (document, username_node);
    return TRUE;
  }

  /* Grab the selected node. */
  selected = WEBKIT_DOM_ELEMENT (g_object_get_data (G_OBJECT (main_div), "ephy-user-selected"));

  /* Fetch the ul. */
  container = webkit_dom_element_get_first_element_child (main_div);

  /* We have a previous selection already, so perform any selection relative to
   * it.
   */
  if (selected) {
    if (keyval == GDK_KEY_Up)
      to_select = webkit_dom_element_get_previous_element_sibling (selected);
    else if (keyval == GDK_KEY_Down)
      to_select = webkit_dom_element_get_next_element_sibling (selected);
  }

  if (!to_select) {
    if (keyval == GDK_KEY_Up)
      to_select = webkit_dom_element_get_last_element_child (container);
    else if (keyval == GDK_KEY_Down)
      to_select = webkit_dom_element_get_first_element_child (container);
  }

  /* Unselect the selected node. */
  if (selected) {
    li_style_attribute = get_user_choice_style (FALSE);
    webkit_dom_element_set_attribute (selected, "style", li_style_attribute, NULL);
    g_free (li_style_attribute);

    anchor = webkit_dom_element_get_first_element_child (selected);

    anchor_style_attribute = get_user_choice_anchor_style (FALSE);
    webkit_dom_element_set_attribute (anchor, "style", anchor_style_attribute, NULL);
    g_free (anchor_style_attribute);
  }

  /* Selected the new node. */
  if (to_select) {
    g_object_set_data (G_OBJECT (main_div), "ephy-user-selected", to_select);

    li_style_attribute = get_user_choice_style (TRUE);
    webkit_dom_element_set_attribute (to_select, "style", li_style_attribute, NULL);
    g_free (li_style_attribute);

    anchor = webkit_dom_element_get_first_element_child (to_select);

    anchor_style_attribute = get_user_choice_anchor_style (TRUE);
    webkit_dom_element_set_attribute (anchor, "style", anchor_style_attribute, NULL);
    g_free (anchor_style_attribute);

    username = webkit_dom_node_get_text_content (WEBKIT_DOM_NODE (anchor));
    webkit_dom_html_input_element_set_value (WEBKIT_DOM_HTML_INPUT_ELEMENT (username_node), username);

    pre_fill_password (username_node);
  } else
    clear_password_field (username_node);

  webkit_dom_event_prevent_default (dom_event);

  return TRUE;
}

static gboolean
username_node_input_cb (WebKitDOMNode  *username_node,
                        WebKitDOMEvent *dom_event,
                        WebKitWebPage  *web_page)
{
  WebKitDOMDocument *document;
  WebKitDOMElement *main_div;

  g_object_set_data (G_OBJECT (username_node), "ephy-user-ever-edited", GINT_TO_POINTER(TRUE));
  document = webkit_web_page_get_dom_document (web_page);
  remove_user_choices (document);
  show_user_choices (document, username_node);

  /* Check if a username has been selected, otherwise clear password field. */
  main_div = webkit_dom_document_get_element_by_id (document, "ephy-user-choices-container");
  if (g_object_get_data (G_OBJECT (main_div), "ephy-user-selected"))
    pre_fill_password (username_node);
  else
    clear_password_field (username_node);

  return TRUE;
}

static void
form_destroyed_cb (gpointer form_auth, GObject *form)
{
  g_object_unref (form_auth);
}

static void
web_page_document_loaded (WebKitWebPage *web_page,
                          gpointer user_data)
{
  WebKitDOMHTMLCollection *forms = NULL;
  WebKitDOMDocument *document = NULL;
  gulong forms_n;
  int i;

  if (!form_auth_data_cache ||
      !g_settings_get_boolean (EPHY_SETTINGS_MAIN, EPHY_PREFS_REMEMBER_PASSWORDS))
    return;

  document = webkit_web_page_get_dom_document (web_page);
  forms = webkit_dom_document_get_forms (document);
  forms_n = webkit_dom_html_collection_get_length (forms);

  if (forms_n == 0) {
    LOG ("No forms found.");
    g_object_unref(forms);
    return;
  }

  for (i = 0; i < forms_n; i++) {
    WebKitDOMHTMLFormElement *form;
    WebKitDOMNode *username_node = NULL;
    WebKitDOMNode *password_node = NULL;

    form = WEBKIT_DOM_HTML_FORM_ELEMENT (webkit_dom_html_collection_item (forms, i));

    /* We have a field that may be the user, and one for a password. */
    if (ephy_web_dom_utils_find_form_auth_elements (form, &username_node, &password_node)) {
      EphyEmbedFormAuth *form_auth;
      GSList *auth_data_list;
      const char *uri_string;
      SoupURI *uri;

      LOG ("Hooking and pre-filling a form");

      /* EphyEmbedFormAuth takes ownership of the nodes */
      form_auth = ephy_embed_form_auth_new (web_page, username_node, password_node, NULL);
      webkit_dom_event_target_add_event_listener (WEBKIT_DOM_EVENT_TARGET (form), "submit",
                                                  G_CALLBACK (form_submitted_cb), FALSE,
                                                  web_page);
      webkit_dom_event_target_add_event_listener (WEBKIT_DOM_EVENT_TARGET (username_node), "blur",
                                                  G_CALLBACK (username_changed_cb), FALSE,
                                                  form_auth);

      /* Plug in the user autocomplete */
      uri_string = webkit_web_page_get_uri (web_page);
      uri = soup_uri_new (uri_string);

      auth_data_list = ephy_form_auth_data_cache_get_list (form_auth_data_cache, uri->host);

      soup_uri_free (uri);

      if (auth_data_list && auth_data_list->next) {
        LOG ("More than 1 password saved, hooking menu for choosing which on focus");
        g_object_set_data (G_OBJECT (username_node), "ephy-auth-data-list", auth_data_list);
        g_object_set_data (G_OBJECT (username_node), "ephy-form-auth", form_auth);
        g_object_set_data (G_OBJECT (username_node), "ephy-document", document);
        webkit_dom_event_target_add_event_listener (WEBKIT_DOM_EVENT_TARGET (username_node), "input",
                                                    G_CALLBACK (username_node_input_cb), TRUE,
                                                    web_page);
        webkit_dom_event_target_add_event_listener (WEBKIT_DOM_EVENT_TARGET (username_node), "keydown",
                                                    G_CALLBACK (username_node_keydown_cb), FALSE,
                                                    web_page);
        webkit_dom_event_target_add_event_listener (WEBKIT_DOM_EVENT_TARGET (username_node), "mouseup",
                                                    G_CALLBACK (username_node_clicked_cb), FALSE,
                                                    web_page);
        webkit_dom_event_target_add_event_listener (WEBKIT_DOM_EVENT_TARGET (username_node), "change",
                                                    G_CALLBACK (username_node_changed_cb), FALSE,
                                                    web_page);
        webkit_dom_event_target_add_event_listener (WEBKIT_DOM_EVENT_TARGET (username_node), "blur",
                                                    G_CALLBACK (username_node_changed_cb), FALSE,
                                                    web_page);
      } else
        LOG ("No items or a single item in auth_data_list, not hooking menu for choosing.");

      pre_fill_form (form_auth);

      g_object_weak_ref (G_OBJECT (form), form_destroyed_cb, form_auth);
    } else
      LOG ("No pre-fillable/hookable form found");
  }

  g_object_unref(forms);
}

static void
web_page_created_callback (WebKitWebExtension *extension,
                           WebKitWebPage *web_page,
                           gpointer user_data)
{
  g_signal_connect_object (web_page, "send-request",
                           G_CALLBACK (web_page_send_request),
                           NULL, 0);
  g_signal_connect_object (web_page, "document-loaded",
                           G_CALLBACK (web_page_document_loaded),
                           NULL, 0);
}

static WebKitWebPage *
get_webkit_web_page_or_return_dbus_error (GDBusMethodInvocation *invocation,
                                          WebKitWebExtension *web_extension,
                                          guint64 page_id)
{
  WebKitWebPage *web_page = webkit_web_extension_get_page (web_extension, page_id);
  if (!web_page) {
    g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                                           "Invalid page ID: %"G_GUINT64_FORMAT, page_id);
  }
  return web_page;
}

static void
handle_method_call (GDBusConnection *connection,
                    const char *sender,
                    const char *object_path,
                    const char *interface_name,
                    const char *method_name,
                    GVariant *parameters,
                    GDBusMethodInvocation *invocation,
                    gpointer user_data)
{
  WebKitWebExtension *web_extension = WEBKIT_WEB_EXTENSION (user_data);

  if (g_strcmp0 (interface_name, EPHY_WEB_EXTENSION_INTERFACE) != 0)
    return;

  if (g_strcmp0 (method_name, "HasModifiedForms") == 0) {
    WebKitWebPage *web_page;
    WebKitDOMDocument *document;
    guint64 page_id;
    gboolean has_modifed_forms;

    g_variant_get (parameters, "(t)", &page_id);
    web_page = get_webkit_web_page_or_return_dbus_error (invocation, web_extension, page_id);
    if (!web_page)
      return;

    document = webkit_web_page_get_dom_document (web_page);
    has_modifed_forms = ephy_web_dom_utils_has_modified_forms (document);

    g_dbus_method_invocation_return_value (invocation, g_variant_new ("(b)", has_modifed_forms));
  } else if (g_strcmp0 (method_name, "GetWebAppTitle") == 0) {
    WebKitWebPage *web_page;
    WebKitDOMDocument *document;
    char *title = NULL;
    guint64 page_id;

    g_variant_get (parameters, "(t)", &page_id);
    web_page = get_webkit_web_page_or_return_dbus_error (invocation, web_extension, page_id);
    if (!web_page)
      return;

    document = webkit_web_page_get_dom_document (web_page);
    title = ephy_web_dom_utils_get_application_title (document);

    g_dbus_method_invocation_return_value (invocation, g_variant_new ("(s)", title ? title : ""));
  } else if (g_strcmp0 (method_name, "GetBestWebAppIcon") == 0) {
    WebKitWebPage *web_page;
    WebKitDOMDocument *document;
    char *base_uri = NULL;
    char *uri = NULL;
    char *color = NULL;
    guint64 page_id;
    gboolean result;

    g_variant_get (parameters, "(ts)", &page_id, &base_uri);
    web_page = get_webkit_web_page_or_return_dbus_error (invocation, web_extension, page_id);
    if (!web_page)
      return;

    if (base_uri == NULL || base_uri == '\0') {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                                             "Base URI cannot be NULL or empty");
      return;
    }

    document= webkit_web_page_get_dom_document (web_page);
    result = ephy_web_dom_utils_get_best_icon (document, base_uri, &uri, &color);

    g_dbus_method_invocation_return_value (invocation,
                                           g_variant_new ("(bss)", result, uri ? uri : "", color ? color : ""));
  } else if (g_strcmp0 (method_name, "FormAuthDataSaveConfirmationResponse") == 0) {
    EphyEmbedFormAuth *form_auth;
    guint request_id;
    gboolean should_store;
    GHashTable *requests = get_form_auth_data_save_requests ();

    g_variant_get (parameters, "(ub)", &request_id, &should_store);

    form_auth = g_hash_table_lookup (requests, GINT_TO_POINTER (request_id));
    if (!form_auth)
      return;

    if (should_store)
      store_password (form_auth);
    g_hash_table_remove (requests, GINT_TO_POINTER (request_id));
  }

}

static const GDBusInterfaceVTable interface_vtable = {
  handle_method_call,
  NULL,
  NULL
};

static void
bus_acquired_cb (GDBusConnection *connection,
                 const char *name,
                 gpointer user_data)
{
  guint registration_id;
  GError *error = NULL;
  static GDBusNodeInfo *introspection_data = NULL;

  if (!introspection_data)
    introspection_data = g_dbus_node_info_new_for_xml (introspection_xml, NULL);

  registration_id = g_dbus_connection_register_object (connection,
                                                       EPHY_WEB_EXTENSION_OBJECT_PATH,
                                                       introspection_data->interfaces[0],
                                                       &interface_vtable,
                                                       g_object_ref (user_data),
                                                       (GDestroyNotify)g_object_unref,
                                                       &error);
  if (!registration_id) {
    g_warning ("Failed to register object: %s\n", error->message);
    g_error_free (error);
  } else {
    dbus_connection = connection;
    g_object_add_weak_pointer (G_OBJECT (connection), (gpointer *)&dbus_connection);
  }
}

G_MODULE_EXPORT void
webkit_web_extension_initialize (WebKitWebExtension *extension)
{
  char *service_name;

  ephy_debug_init ();
  uri_tester = uri_tester_new (g_getenv ("EPHY_DOT_DIR"));
  if (!g_getenv ("EPHY_PRIVATE_PROFILE"))
    form_auth_data_cache = ephy_form_auth_data_cache_new ();

  g_signal_connect (extension, "page-created",
                    G_CALLBACK (web_page_created_callback),
                    NULL);

  service_name = g_strdup_printf ("%s-%s", EPHY_WEB_EXTENSION_SERVICE_NAME, g_getenv ("EPHY_WEB_EXTENSION_ID"));
  g_bus_own_name (G_BUS_TYPE_SESSION,
                  service_name,
                  G_BUS_NAME_OWNER_FLAGS_NONE,
                  bus_acquired_cb,
                  NULL, NULL,
                  g_object_ref (extension),
                  (GDestroyNotify)g_object_unref);
  g_free (service_name);
}
