/* -*- Mode: C; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set sw=2 ts=2 sts=2 et: */
/*
 *  Copyright © 2013, 2014 Yosef Or Boczko <yoseforb@gnome.org>
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
 *
 */

#include "config.h"
#include "ephy-title-box.h"

#include "ephy-certificate-dialog.h"
#include "ephy-debug.h"
#include "ephy-private.h"
#include "ephy-type-builtins.h"

/**
 * SECTION:ephy-title-box
 * @short_description: A #GtkStack shown the title or the address of the page
 * @see_also: #GtkStack
 *
 * #EphyTitleBox displaying or the title & uri of the page or the address bar.
 */

enum {
  PROP_0,
  PROP_WINDOW,
  PROP_MODE,
  N_PROPERTIES
};

static GParamSpec *object_properties[N_PROPERTIES] = { NULL, };

typedef struct
{
  EphyWindow    *window;
  WebKitWebView *web_view;

  EphyTitleBoxMode mode;

  GtkWidget *entry;
  GtkWidget *lock_image;
  GtkWidget *title;
  GtkWidget *uri;

  GBinding *title_binding;
  GBinding *uri_binding;

  guint button_down : 1;
  guint switch_to_entry_timeout_id;

  gulong title_sig_id;
} EphyTitleBoxPrivate;

G_DEFINE_TYPE_WITH_PRIVATE (EphyTitleBox, ephy_title_box, GTK_TYPE_STACK)

static void
ephy_title_box_get_property (GObject    *object,
                             guint       prop_id,
                             GValue     *value,
                             GParamSpec *pspec)
{
  EphyTitleBox *title_box = EPHY_TITLE_BOX (object);

  switch (prop_id)
  {
    case PROP_MODE:
      g_value_set_enum (value, ephy_title_box_get_mode (title_box));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
  }
}

static void
ephy_title_box_set_property (GObject      *object,
                             guint         prop_id,
                             const GValue *value,
                             GParamSpec   *pspec)
{
  EphyTitleBox        *title_box = EPHY_TITLE_BOX (object);
  EphyTitleBoxPrivate *priv = ephy_title_box_get_instance_private (title_box);

  switch (prop_id)
  {
    case PROP_WINDOW:
      priv->window = EPHY_WINDOW (g_value_get_object (value));
      break;
    case PROP_MODE:
      ephy_title_box_set_mode (title_box, g_value_get_enum (value));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
  }
}

static gboolean
ephy_title_box_entry_key_press_cb (GtkWidget   *widget,
                                   GdkEventKey *event,
                                   gpointer     user_data)
{
  EphyTitleBox *title_box = EPHY_TITLE_BOX (user_data);
  guint         state = event->state & gtk_accelerator_get_default_mod_mask ();

  LOG ("key-press-event entry %p event %p title-box %p", widget, event, title_box);

  if (event->keyval == GDK_KEY_Escape && state == 0) {
    ephy_title_box_set_mode (title_box, EPHY_TITLE_BOX_MODE_TITLE);
    /* don't return GDK_EVENT_STOP since we want to cancel the autocompletion popup too */
  }

  return GDK_EVENT_PROPAGATE;
}

static gboolean
ephy_title_box_view_focus_in_cb (GtkWidget     *widget,
                                 GdkEventFocus *event,
                                 gpointer       user_data)
{
  EphyTitleBox        *title_box = EPHY_TITLE_BOX (user_data);
  EphyTitleBoxPrivate *priv = ephy_title_box_get_instance_private (title_box);
  const gchar         *title;

  LOG ("focus-in-event web_view %p event %p title-box %p", widget, event, title_box);

  title = webkit_web_view_get_title (priv->web_view);
  if (title && *title != '\0')
    ephy_title_box_set_mode (title_box, EPHY_TITLE_BOX_MODE_TITLE);

  return GDK_EVENT_PROPAGATE;
}

static void
ephy_title_box_add_address_bar (EphyTitleBox *title_box)
{
  EphyTitleBoxPrivate *priv = ephy_title_box_get_instance_private (title_box);
  GtkActionGroup      *action_group;
  GtkAction           *action;
  GtkWidget           *box;
  GtkWidget           *reload;
  GtkSizeGroup        *size;

  box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_show (box);
  gtk_style_context_add_class (gtk_widget_get_style_context (box), "location-entry");
  gtk_stack_add_named (GTK_STACK (title_box), box, "address-bar");

  priv->entry = ephy_location_entry_new ();
  gtk_widget_show (priv->entry);
  gtk_box_pack_start (GTK_BOX (box), priv->entry, TRUE, TRUE, 0);

  /* Reload/Stop */
  reload = gtk_button_new ();
  gtk_button_set_focus_on_click (GTK_BUTTON (reload), FALSE);
  gtk_widget_show (reload);
  /* FIXME: apparently we need an image inside the button for the action
  * icon to appear. */
  gtk_button_set_image (GTK_BUTTON (reload), gtk_image_new ());
  gtk_widget_set_valign (reload, GTK_ALIGN_CENTER);
  action_group = ephy_window_get_toolbar_action_group (priv->window);
  action = gtk_action_group_get_action (action_group, "ViewCombinedStopReload");
  gtk_activatable_set_related_action (GTK_ACTIVATABLE (reload), action);
  gtk_container_add (GTK_CONTAINER (box), reload);

  size = gtk_size_group_new (GTK_SIZE_GROUP_VERTICAL);
  gtk_size_group_add_widget (size, priv->entry);
  gtk_size_group_add_widget (size, reload);
  g_object_unref (size);
}

static void
ephy_title_box_add_title_bar (EphyTitleBox *title_box)
{
  EphyTitleBoxPrivate *priv = ephy_title_box_get_instance_private (title_box);
  GtkStyleContext     *context;
  GtkWidget           *box;
  GtkWidget           *hbox;

  box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_valign (box, GTK_ALIGN_CENTER);
  gtk_widget_show (box);
  gtk_stack_add_named (GTK_STACK (title_box), box, "title-bar");

  priv->title = gtk_label_new (NULL);
  gtk_widget_show (priv->title);
  context = gtk_widget_get_style_context (priv->title);
  gtk_style_context_add_class (context, "title");
  gtk_label_set_line_wrap (GTK_LABEL (priv->title), FALSE);
  gtk_label_set_single_line_mode (GTK_LABEL (priv->title), TRUE);
  gtk_label_set_ellipsize (GTK_LABEL (priv->title), PANGO_ELLIPSIZE_END);
  gtk_box_pack_start (GTK_BOX (box), priv->title, FALSE, FALSE, 0);

  hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
  context = gtk_widget_get_style_context (hbox);
  gtk_style_context_add_class (context, "subtitle");
  gtk_style_context_add_class (context, "dim-label");
  gtk_widget_set_halign (hbox, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (hbox, GTK_ALIGN_BASELINE);
  gtk_widget_show (hbox);
  gtk_box_pack_start (GTK_BOX (box), hbox, FALSE, FALSE, 0);

  priv->lock_image = gtk_image_new_from_icon_name ("channel-secure-symbolic", GTK_ICON_SIZE_MENU);
  gtk_widget_set_valign (priv->lock_image, GTK_ALIGN_BASELINE);
  gtk_box_pack_start (GTK_BOX (hbox), priv->lock_image, FALSE, FALSE, 0);

  priv->uri = gtk_label_new (NULL);
  gtk_widget_set_valign (priv->uri, GTK_ALIGN_BASELINE);
  gtk_widget_show (priv->uri);
  gtk_label_set_line_wrap (GTK_LABEL (priv->uri), FALSE);
  gtk_label_set_single_line_mode (GTK_LABEL (priv->uri), TRUE);
  gtk_label_set_ellipsize (GTK_LABEL (priv->uri), PANGO_ELLIPSIZE_END);
  gtk_box_pack_start (GTK_BOX (hbox), priv->uri, FALSE, FALSE, 0);
}

static void
ephy_title_box_constructed (GObject *object)
{
  EphyTitleBox *title_box = EPHY_TITLE_BOX (object);

  LOG ("EphyTitleBox constructed");

  G_OBJECT_CLASS (ephy_title_box_parent_class)->constructed (object);

  gtk_widget_add_events (GTK_WIDGET (title_box),
                         GDK_BUTTON_PRESS_MASK |
                         GDK_BUTTON_RELEASE_MASK);

  ephy_title_box_add_address_bar (title_box);
  ephy_title_box_add_title_bar (title_box);
}

static gboolean
ephy_title_box_switch_to_entry_timeout_cb (gpointer user_data)
{
  EphyTitleBox        *title_box = EPHY_TITLE_BOX (user_data);
  EphyTitleBoxPrivate *priv = ephy_title_box_get_instance_private (title_box);

  LOG ("switch_to_entry_timeout_cb title-box %p switch_to_entry_timeout_id %u",
    title_box, priv->switch_to_entry_timeout_id);

  priv->switch_to_entry_timeout_id = 0;
  ephy_title_box_set_mode (title_box, EPHY_TITLE_BOX_MODE_LOCATION_ENTRY);
  gtk_widget_grab_focus (priv->entry);

  return G_SOURCE_REMOVE;
}

static void
ephy_title_box_switch_to_entry_after_double_click_time (EphyTitleBox *title_box)
{
  EphyTitleBoxPrivate *priv = ephy_title_box_get_instance_private (title_box);
  gint                 double_click_time;

  if (priv->switch_to_entry_timeout_id > 0)
    return;

  LOG ("switch_to_entry_after_double_click_time title-box %p switch_to_entry_timeout_id %u",
    title_box, priv->switch_to_entry_timeout_id);

  g_object_get (gtk_widget_get_settings (GTK_WIDGET (title_box)),
                "gtk-double-click-time", &double_click_time,
                NULL);

  /* We don't want to wait the maximum time allowed between two
   * clicks before showing the entry. A quarter of this time is enough.
   */
  priv->switch_to_entry_timeout_id = g_timeout_add (double_click_time / 4,
                                                    ephy_title_box_switch_to_entry_timeout_cb,
                                                    title_box);
}

static void
ephy_title_box_cancel_switch_to_entry_after_double_click_time (EphyTitleBox *title_box)
{
  EphyTitleBoxPrivate *priv = ephy_title_box_get_instance_private (title_box);

  if (priv->switch_to_entry_timeout_id == 0)
    return;

  LOG ("cancel_switch_to_entry_after_double_click_time title-box %p switch_to_entry_timeout_id %u",
    title_box, priv->switch_to_entry_timeout_id);

  g_source_remove (priv->switch_to_entry_timeout_id);
  priv->switch_to_entry_timeout_id = 0;
}

static gboolean
ephy_title_box_button_release_event (GtkWidget      *widget,
                                     GdkEventButton *event)
{
  EphyTitleBox        *title_box = EPHY_TITLE_BOX (widget);
  EphyTitleBoxPrivate *priv = ephy_title_box_get_instance_private (title_box);

  if (priv->mode != EPHY_TITLE_BOX_MODE_TITLE
      || event->button != GDK_BUTTON_PRIMARY
      || !priv->button_down)
    return GDK_EVENT_PROPAGATE;

  LOG ("button-release-event title-box %p event %p", title_box, event);

  ephy_title_box_switch_to_entry_after_double_click_time (title_box);
  priv->button_down = FALSE;

  return GDK_EVENT_PROPAGATE;
}

static gboolean
ephy_title_box_button_press_event (GtkWidget      *widget,
                                   GdkEventButton *event)
{
  EphyTitleBox        *title_box = EPHY_TITLE_BOX (widget);
  EphyTitleBoxPrivate *priv = ephy_title_box_get_instance_private (title_box);
  EphyEmbedShellMode   mode;

  if (priv->mode != EPHY_TITLE_BOX_MODE_TITLE
      || event->button != GDK_BUTTON_PRIMARY)
    return GDK_EVENT_PROPAGATE;

  mode = ephy_embed_shell_get_mode (ephy_embed_shell_get_default ());
  if (mode == EPHY_EMBED_SHELL_MODE_APPLICATION)
    return GDK_EVENT_PROPAGATE;

  LOG ("button-press-event title-box %p event %p", title_box, event);

  if (event->type == GDK_BUTTON_PRESS) {
    priv->button_down = TRUE;
  } else {
    priv->button_down = FALSE;
    ephy_title_box_cancel_switch_to_entry_after_double_click_time (title_box);
  }

  return GDK_EVENT_PROPAGATE;
}

static void
ephy_title_box_dispose (GObject *object)
{
  EphyTitleBox *title_box = EPHY_TITLE_BOX (object);

  LOG ("EphyTitleBox dispose %p", title_box);

  ephy_title_box_cancel_switch_to_entry_after_double_click_time (title_box);

  ephy_title_box_set_web_view (title_box, NULL);

  G_OBJECT_CLASS (ephy_title_box_parent_class)->dispose (object);
}

static void
ephy_title_box_get_preferred_width (GtkWidget *widget,
                                    gint      *minimum_width,
                                    gint      *natural_width)
{
  if (minimum_width)
    *minimum_width = -1;

  if (natural_width)
    *natural_width = 860;
}

static void
ephy_title_box_class_init (EphyTitleBoxClass *klass)
{
  GObjectClass   *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->dispose = ephy_title_box_dispose;
  object_class->get_property = ephy_title_box_get_property;
  object_class->set_property = ephy_title_box_set_property;
  object_class->constructed = ephy_title_box_constructed;
  widget_class->button_press_event = ephy_title_box_button_press_event;
  widget_class->button_release_event = ephy_title_box_button_release_event;
  widget_class->get_preferred_width = ephy_title_box_get_preferred_width;

  /**
   *
   * EphyTitleBox:window:
   *
   * The parent window.
   */
  object_properties[PROP_WINDOW] = g_param_spec_object ("window",
                                                        "Window",
                                                        "The parent window",
                                                        EPHY_TYPE_WINDOW,
                                                        G_PARAM_READWRITE |
                                                        G_PARAM_CONSTRUCT_ONLY |
                                                        G_PARAM_STATIC_STRINGS);

  /**
   * EphyTitleBox:mode:
   *
   * The mode of the title box.
   */
  object_properties[PROP_MODE] = g_param_spec_enum ("mode",
                                                    "Mode",
                                                    "The mode of the title box",
                                                    EPHY_TYPE_TITLE_BOX_MODE,
                                                    EPHY_TITLE_BOX_MODE_NONE,
                                                    G_PARAM_READWRITE |
                                                    G_PARAM_CONSTRUCT |
                                                    G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class,
                                     N_PROPERTIES,
                                     object_properties);
}

static void
ephy_title_box_init (EphyTitleBox *title_box)
{
  LOG ("EphyTitleBox initialising %p", title_box);
}

static void
ephy_title_box_title_changed_cb (GObject    *gobject,
                                 GParamSpec *pspec,
                                 gpointer    user_data)
{
  EphyTitleBox        *title_box = EPHY_TITLE_BOX (user_data);
  EphyTitleBoxPrivate *priv = ephy_title_box_get_instance_private (title_box);
  WebKitWebView       *web_view = WEBKIT_WEB_VIEW (gobject);
  const gchar         *title;

  LOG ("notify::title web_view %p title-box %p\n", web_view, title_box);

  title = webkit_web_view_get_title (web_view);

  if (gtk_widget_is_focus (priv->entry) ||
      !title || *title == '\0') {
    ephy_title_box_set_mode (title_box, EPHY_TITLE_BOX_MODE_LOCATION_ENTRY);
    return;
  }

  ephy_title_box_set_mode (title_box, EPHY_TITLE_BOX_MODE_TITLE);
}

static gboolean
ephy_title_box_transform_uri_to_label (GBinding     *binding,
                                       const GValue *from_value,
                                       GValue       *to_value,
                                       gpointer      user_data)
{
  const gchar *label;
  gboolean     rtl;
  gchar       *uri;

  rtl = gtk_widget_get_default_direction () == GTK_TEXT_DIR_RTL;

  label = g_value_get_string (from_value);
  uri = g_strconcat (rtl ? "▾ " : label, rtl ? label : " ▾", NULL);
  g_value_take_string (to_value, uri);

  return TRUE;
}

/**
 * ephy_title_box_new:
 * @window: an #EphyWindow
 *
 * Creates a new #EphyTitleBox.
 *
 * Returns: a new #EphyTitleBox
 **/
EphyTitleBox *
ephy_title_box_new (EphyWindow *window)
{
  g_return_val_if_fail (EPHY_IS_WINDOW (window), NULL);

  return g_object_new (EPHY_TYPE_TITLE_BOX,
                       "window", window,
                       "margin-start", 54,
                       "margin-end", 54,
                       "transition-duration", 150,
                       "transition-type", GTK_STACK_TRANSITION_TYPE_CROSSFADE,
                       NULL);
}

/**
 * ephy_title_box_set_web_view:
 * @title_box: an #EphyTitleBox
 * @web_view: a #WebKitWebView
 *
 * Sets the web view of the @title_box.
 **/
void
ephy_title_box_set_web_view (EphyTitleBox  *title_box,
                             WebKitWebView *web_view)
{
  EphyTitleBoxPrivate *priv;
  EphyEmbedShellMode   mode;
  const gchar         *title;

  g_return_if_fail (EPHY_IS_TITLE_BOX (title_box));

  priv = ephy_title_box_get_instance_private (title_box);

  if (priv->web_view == web_view)
    return;

  LOG ("ephy_title_box_set_web_view title-box %p web_view %p", title_box, web_view);

  mode = ephy_embed_shell_get_mode (ephy_embed_shell_get_default ());

  if (priv->web_view != NULL) {
    if (mode != EPHY_EMBED_SHELL_MODE_APPLICATION) {
      g_signal_handlers_disconnect_by_func (priv->entry,
                                            G_CALLBACK (ephy_title_box_entry_key_press_cb),
                                            title_box);
      g_signal_handlers_disconnect_by_func (priv->web_view,
                                            G_CALLBACK (ephy_title_box_view_focus_in_cb),
                                            title_box);
      if (priv->title_sig_id > 0)
        g_signal_handler_disconnect (priv->web_view, priv->title_sig_id);
    }

    g_clear_object (&priv->title_binding);
    g_clear_object (&priv->uri_binding);

    g_object_unref (priv->web_view);
  }

  priv->web_view = NULL;

  if (web_view == NULL)
    return;

  priv->web_view = g_object_ref (web_view);

  title = webkit_web_view_get_title (web_view);

  ephy_title_box_set_mode (title_box, title && *title != '\0' ?
                                      EPHY_TITLE_BOX_MODE_TITLE : EPHY_TITLE_BOX_MODE_LOCATION_ENTRY);

  priv->title_binding = g_object_bind_property (priv->web_view, "title",
                                                priv->title, "label",
                                                G_BINDING_SYNC_CREATE);

  priv->uri_binding = g_object_bind_property_full (priv->web_view, "uri",
                                                   priv->uri, "label",
                                                   G_BINDING_SYNC_CREATE,
                                                   ephy_title_box_transform_uri_to_label,
                                                   NULL, NULL, NULL);

  if (mode != EPHY_EMBED_SHELL_MODE_APPLICATION) {
    priv->title_sig_id = g_signal_connect (priv->web_view, "notify::title",
                                           G_CALLBACK (ephy_title_box_title_changed_cb),
                                           title_box);
    g_signal_connect (priv->entry, "key-press-event",
                      G_CALLBACK (ephy_title_box_entry_key_press_cb), title_box);
    g_signal_connect (priv->web_view, "focus-in-event",
                      G_CALLBACK (ephy_title_box_view_focus_in_cb), title_box);
  }
}

/**
 * ephy_title_box_get_mode:
 * @title_box: an #EphyTitleBox
 *
 * Gets the value of the #EphyTitleBox:mode property.
 *
 * Returns: The mode of the @title_box.
 **/
EphyTitleBoxMode
ephy_title_box_get_mode (EphyTitleBox *title_box)
{
  EphyTitleBoxPrivate *priv;

  g_return_val_if_fail (EPHY_IS_TITLE_BOX (title_box), EPHY_TITLE_BOX_MODE_NONE);

  priv = ephy_title_box_get_instance_private (title_box);

  return priv->mode;
}

/**
 * ephy_title_box_set_mode:
 * @title_box: an #EphyTitleBox
 * @mode: an #EphyTitleBoxMode
 *
 * Sets the mode of the @title_box.
 **/
void
ephy_title_box_set_mode (EphyTitleBox    *title_box,
                         EphyTitleBoxMode mode)
{
  EphyTitleBoxPrivate *priv;
  EphyEmbedShellMode   shell_mode;
  const gchar         *uri;

  g_return_if_fail (EPHY_IS_TITLE_BOX (title_box));

  if (mode == EPHY_TITLE_BOX_MODE_NONE)
    return;

  priv = ephy_title_box_get_instance_private (title_box);
  shell_mode = ephy_embed_shell_get_mode (ephy_embed_shell_get_default ());

  uri = priv->web_view ? webkit_web_view_get_uri (priv->web_view) : NULL;

  ephy_title_box_cancel_switch_to_entry_after_double_click_time (title_box);

  if (!uri || g_str_has_prefix (uri, "about:") ||
      g_str_has_prefix (uri, "ephy-about:")) {
    mode = EPHY_TITLE_BOX_MODE_LOCATION_ENTRY;
  }

  if (shell_mode == EPHY_EMBED_SHELL_MODE_APPLICATION)
    mode = EPHY_TITLE_BOX_MODE_TITLE;

  if (priv->mode == mode)
    return;

  LOG ("ephy_title_box_set_mode title-box %p mode %u", title_box, mode);

  priv->mode = mode;

  gtk_stack_set_visible_child_name (GTK_STACK (title_box),
                                    mode == EPHY_TITLE_BOX_MODE_LOCATION_ENTRY ? "address-bar" : "title-bar");

  g_object_notify_by_pspec (G_OBJECT (title_box), object_properties[PROP_MODE]);
}

/**
 * ephy_title_box_set_show_lock:
 * @title_box: an #EphyTitleBox
 * @show_lock: if @title_box should show a lock icon indicating the security level
 * of the page
 *
 * If @show_lock is TRUE, the title will show an icon reflecting the
 * security level of the page, by default it's shown only in secure and
 * insecure pages (insecure meaning secure pages with something broken in such
 * security).
 *
 **/
void
ephy_title_box_set_show_lock (EphyTitleBox *title_box,
                              gboolean      show_lock)
{
  EphyTitleBoxPrivate *priv;

  g_return_if_fail (EPHY_IS_TITLE_BOX (title_box));

  priv = ephy_title_box_get_instance_private (title_box);

  gtk_widget_set_visible (priv->lock_image, show_lock);

  ephy_location_entry_set_show_lock (EPHY_LOCATION_ENTRY (priv->entry), show_lock);
}

/**
 * ephy_title_box_set_lock_state:
 * @title_box: an #EphyTitleBox
 * @mode: an #EphyLocationLockState
 *
 * Set the lock icon to be displayed, to actually show the icon see
 * ephy_title_box_set_show_lock.
 **/
void
ephy_title_box_set_lock_state (EphyTitleBox         *title_box,
                               EphyLocationLockState state)
{
  EphyTitleBoxPrivate *priv;

  g_return_if_fail (EPHY_IS_TITLE_BOX (title_box));

  priv = ephy_title_box_get_instance_private (title_box);

  g_object_set (priv->lock_image, "icon-name", state == EPHY_LOCATION_LOCK_STATE_SECURE ?
                                               "channel-secure-symbolic" : "channel-insecure-symbolic",
                                  NULL);

  ephy_location_entry_set_lock_state (EPHY_LOCATION_ENTRY (priv->entry), state);
}

/**
 * ephy_title_box_get_location_entry:
 * @title_box: an #EphyTitleBox
 *
 * Returns the location entry wrapped by @title_box.
 *
 * Returns: (transfer none): an #EphyLocationEntry
 **/
GtkWidget *
ephy_title_box_get_location_entry (EphyTitleBox *title_box)
{
  EphyTitleBoxPrivate *priv;

  g_return_val_if_fail (EPHY_IS_TITLE_BOX (title_box), NULL);

  priv = ephy_title_box_get_instance_private (title_box);

  return priv->entry;
}