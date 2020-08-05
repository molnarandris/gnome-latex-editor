/* editor-window.c
 *
 * Copyright 2020 Christian Hergert <chergert@redhat.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define G_LOG_DOMAIN "editor-window"

#include "config.h"

#include <glib/gi18n.h>
#include <libhandy-1/handy.h>

#include "editor-application.h"
#include "editor-document.h"
#include "editor-session-private.h"
#include "editor-tab-private.h"
#include "editor-utils-private.h"
#include "editor-window-private.h"

G_DEFINE_TYPE (EditorWindow, editor_window, HDY_TYPE_APPLICATION_WINDOW)

enum {
  PROP_0,
  PROP_VISIBLE_PAGE,
  N_PROPS
};

enum {
  ACTIVATE_ACTION,
  N_SIGNALS
};

static GParamSpec *properties[N_PROPS];
static guint signals[N_SIGNALS];

static void
editor_window_cursor_moved_cb (EditorWindow   *self,
                               EditorDocument *document)
{
  EditorPage *page;

  g_assert (EDITOR_IS_WINDOW (self));
  g_assert (EDITOR_IS_DOCUMENT (document));

  if ((page = editor_window_get_visible_page (self)))
    {
      guint line;
      guint column;

      editor_page_get_visual_position (page, &line, &column);

      _editor_position_label_set_position (self->position_label, line + 1, column + 1);
    }
}

static void
editor_window_page_bind_cb (EditorWindow      *self,
                            EditorPage        *page,
                            EditorSignalGroup *group)
{
  EditorDocument *document;

  g_assert (EDITOR_IS_WINDOW (self));
  g_assert (EDITOR_IS_PAGE (page));
  g_assert (EDITOR_IS_SIGNAL_GROUP (group));

  document = editor_page_get_document (page);

  editor_signal_group_set_target (self->document_signals, document);
  editor_window_cursor_moved_cb (self, document);

  _editor_window_actions_update (self, page);
}

static void
editor_window_page_unbind_cb (EditorWindow      *self,
                              EditorSignalGroup *group)
{
  g_assert (EDITOR_IS_WINDOW (self));
  g_assert (EDITOR_IS_SIGNAL_GROUP (group));

  _editor_window_actions_update (self, NULL);
}

static void
editor_window_update_actions (EditorWindow *self)
{
  g_assert (EDITOR_IS_WINDOW (self));

  _editor_window_actions_update (self, self->visible_page);
}

static void
editor_window_notify_current_page_cb (EditorWindow *self,
                                      GParamSpec   *pspec,
                                      GtkNotebook  *notebook)
{
  EditorPage *page = NULL;
  gint page_num;

  g_assert (EDITOR_IS_WINDOW (self));
  g_assert (GTK_IS_NOTEBOOK (notebook));

  page_num = gtk_notebook_get_current_page (notebook);
  if (page_num > -1)
    page = EDITOR_PAGE (gtk_notebook_get_nth_page (self->notebook, page_num));

  if (self->visible_page == page)
    return;

  g_assert (!page || EDITOR_IS_PAGE (page));

  gtk_label_set_label (self->title, _(PACKAGE_NAME));
  gtk_label_set_label (self->subtitle, NULL);
  gtk_widget_set_visible (GTK_WIDGET (self->is_modified), FALSE);
  gtk_widget_set_visible (GTK_WIDGET (self->subtitle), FALSE);
  gtk_widget_set_visible (GTK_WIDGET (self->position_box),
                          page && g_settings_get_boolean (self->settings, "show-line-numbers"));

  self->visible_page = page;

  editor_binding_group_set_source (self->page_bindings, page);
  editor_signal_group_set_target (self->page_signals, page);

  _editor_window_actions_update (self, page);

  if (page != NULL)
    gtk_widget_grab_focus (GTK_WIDGET (page));

  g_object_notify_by_pspec (G_OBJECT (self), properties [PROP_VISIBLE_PAGE]);
}

static void
editor_window_page_added_cb (EditorWindow *self,
                             EditorPage   *page,
                             guint         page_num,
                             GtkNotebook  *notebook)
{
  gint n_pages;

  g_assert (EDITOR_IS_WINDOW (self));
  g_assert (EDITOR_IS_PAGE (page));
  g_assert (GTK_IS_NOTEBOOK (notebook));

  n_pages = gtk_notebook_get_n_pages (notebook);
  gtk_notebook_set_show_tabs (notebook, n_pages > 1);
  gtk_widget_queue_resize (GTK_WIDGET (notebook));

  gtk_stack_set_visible_child (self->stack, GTK_WIDGET (self->notebook));
}

static void
editor_window_page_removed_cb (EditorWindow *self,
                               EditorPage   *page,
                               guint         page_num,
                               GtkNotebook  *notebook)
{
  gint n_pages;

  g_assert (EDITOR_IS_WINDOW (self));
  g_assert (EDITOR_IS_PAGE (page));
  g_assert (GTK_IS_NOTEBOOK (notebook));

  n_pages = gtk_notebook_get_n_pages (notebook);
  gtk_notebook_set_show_tabs (notebook, n_pages > 1);

  if (n_pages == 0)
    gtk_stack_set_visible_child (self->stack, GTK_WIDGET (self->empty));
}

static void
editor_window_sidebar_toggled_cb (EditorWindow    *self,
                                  GtkToggleButton *button)
{
  g_assert (EDITOR_IS_WINDOW (self));
  g_assert (GTK_IS_TOGGLE_BUTTON (button));

  if (gtk_toggle_button_get_active (button))
    gtk_widget_show (GTK_WIDGET (self->sidebar));
  else
    gtk_widget_hide (GTK_WIDGET (self->sidebar));
}

static void
editor_window_notebook_page_reordered_cb (EditorWindow *self,
                                          GtkWidget    *child,
                                          guint         page_num,
                                          GtkNotebook  *notebook)
{
  EditorPage *page = (EditorPage *)child;

  g_assert (EDITOR_IS_WINDOW (self));
  g_assert (EDITOR_IS_PAGE (page));
  g_assert (GTK_IS_NOTEBOOK (notebook));

  _editor_sidebar_page_reordered (self->sidebar, page, page_num);
}

static gboolean
editor_window_delete_event (GtkWidget   *widget,
                            GdkEventAny *event)
{
  EditorWindow *self = (EditorWindow *)widget;

  g_assert (EDITOR_IS_WINDOW (self));

  g_signal_handlers_disconnect_by_func (self->notebook,
                                        G_CALLBACK (editor_window_page_added_cb),
                                        self);
  g_signal_handlers_disconnect_by_func (self->notebook,
                                        G_CALLBACK (editor_window_page_removed_cb),
                                        self);
  g_signal_handlers_disconnect_by_func (self->notebook,
                                        G_CALLBACK (editor_window_notify_current_page_cb),
                                        self);

  _editor_session_remove_window (EDITOR_SESSION_DEFAULT, self);

  self->visible_page = NULL;

  return GDK_EVENT_PROPAGATE;
}

static void
editor_window_constructed (GObject *object)
{
  EditorWindow *self = (EditorWindow *)object;
  GtkApplication *app;
  GMenu *export_menu;
  GMenu *options_menu;
  GMenu *primary_menu;

  g_assert (EDITOR_IS_WINDOW (self));

  G_OBJECT_CLASS (editor_window_parent_class)->constructed (object);

  app = GTK_APPLICATION (EDITOR_APPLICATION_DEFAULT);

  export_menu = gtk_application_get_menu_by_id (GTK_APPLICATION (app), "export-menu");
  gtk_menu_button_set_menu_model (self->export_menu, G_MENU_MODEL (export_menu));

  primary_menu = gtk_application_get_menu_by_id (GTK_APPLICATION (app), "primary-menu");
  gtk_menu_button_set_menu_model (self->primary_menu, G_MENU_MODEL (primary_menu));

  options_menu = gtk_application_get_menu_by_id (GTK_APPLICATION (app), "options-menu");
  gtk_menu_button_set_menu_model (self->options_menu, G_MENU_MODEL (options_menu));
}

static void
editor_window_dispose (GObject *object)
{
  EditorWindow *self = (EditorWindow *)object;

  g_assert (EDITOR_IS_WINDOW (self));

  _editor_session_remove_window (EDITOR_SESSION_DEFAULT, self);

  g_clear_object (&self->settings);

  editor_binding_group_set_source (self->page_bindings, NULL);
  editor_signal_group_set_target (self->page_signals, NULL);
  editor_signal_group_set_target (self->document_signals, NULL);

  G_OBJECT_CLASS (editor_window_parent_class)->dispose (object);
}

static void
editor_window_finalize (GObject *object)
{
  EditorWindow *self = (EditorWindow *)object;

  g_assert (EDITOR_IS_WINDOW (self));

  g_clear_object (&self->page_bindings);
  g_clear_object (&self->page_signals);
  g_clear_object (&self->document_signals);

  G_OBJECT_CLASS (editor_window_parent_class)->finalize (object);
}

static void
editor_window_get_property (GObject    *object,
                            guint       prop_id,
                            GValue     *value,
                            GParamSpec *pspec)
{
  EditorWindow *self = EDITOR_WINDOW (object);

  switch (prop_id)
    {
    case PROP_VISIBLE_PAGE:
      g_value_set_object (value, editor_window_get_visible_page (self));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
editor_window_set_property (GObject      *object,
                            guint         prop_id,
                            const GValue *value,
                            GParamSpec   *pspec)
{
  EditorWindow *self = EDITOR_WINDOW (object);

  switch (prop_id)
    {
    case PROP_VISIBLE_PAGE:
      editor_window_set_visible_page (self, g_value_get_object (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
editor_window_class_init (EditorWindowClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);
  GtkBindingSet *bindings = gtk_binding_set_by_class (klass);

  object_class->constructed = editor_window_constructed;
  object_class->dispose = editor_window_dispose;
  object_class->finalize = editor_window_finalize;
  object_class->get_property = editor_window_get_property;
  object_class->set_property = editor_window_set_property;

  widget_class->delete_event = editor_window_delete_event;

  properties [PROP_VISIBLE_PAGE] =
    g_param_spec_object ("visible-page",
                         "Visible Page",
                         "The currently visible page",
                         EDITOR_TYPE_PAGE,
                         (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_properties (object_class, N_PROPS, properties);

  signals [ACTIVATE_ACTION] =
    g_signal_new_class_handler ("activate-action",
                                G_TYPE_FROM_CLASS (klass),
                                G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION,
                                G_CALLBACK (_editor_action_with_string),
                                NULL, NULL,
                                NULL,
                                G_TYPE_NONE,
                                3,
                                G_TYPE_STRING,
                                G_TYPE_STRING,
                                G_TYPE_STRING);

  gtk_widget_class_set_template_from_resource (widget_class, "/org/gnome/TextEditor/ui/editor-window.ui");
  gtk_widget_class_bind_template_child (widget_class, EditorWindow, drop_box);
  gtk_widget_class_bind_template_child (widget_class, EditorWindow, empty);
  gtk_widget_class_bind_template_child (widget_class, EditorWindow, export_menu);
  gtk_widget_class_bind_template_child (widget_class, EditorWindow, is_modified);
  gtk_widget_class_bind_template_child (widget_class, EditorWindow, notebook);
  gtk_widget_class_bind_template_child (widget_class, EditorWindow, open_toggle_button);
  gtk_widget_class_bind_template_child (widget_class, EditorWindow, options_menu);
  gtk_widget_class_bind_template_child (widget_class, EditorWindow, paned);
  gtk_widget_class_bind_template_child (widget_class, EditorWindow, position_box);
  gtk_widget_class_bind_template_child (widget_class, EditorWindow, position_label);
  gtk_widget_class_bind_template_child (widget_class, EditorWindow, primary_menu);
  gtk_widget_class_bind_template_child (widget_class, EditorWindow, sidebar);
  gtk_widget_class_bind_template_child (widget_class, EditorWindow, subtitle);
  gtk_widget_class_bind_template_child (widget_class, EditorWindow, title);
  gtk_widget_class_bind_template_child (widget_class, EditorWindow, stack);
  gtk_widget_class_bind_template_callback (widget_class, editor_window_sidebar_toggled_cb);
  gtk_widget_class_bind_template_callback (widget_class, editor_window_notebook_page_reordered_cb);

  gtk_binding_entry_add_signal (bindings, GDK_KEY_w, GDK_CONTROL_MASK, "activate-action", 3, G_TYPE_STRING, "win", G_TYPE_STRING, "close-current-page", G_TYPE_STRING, "");
  gtk_binding_entry_add_signal (bindings, GDK_KEY_o, GDK_CONTROL_MASK, "activate-action", 3, G_TYPE_STRING, "win", G_TYPE_STRING, "open", G_TYPE_STRING, "");
  gtk_binding_entry_add_signal (bindings, GDK_KEY_k, GDK_CONTROL_MASK, "activate-action", 3, G_TYPE_STRING, "win", G_TYPE_STRING, "focus-search", G_TYPE_STRING, "");
  gtk_binding_entry_add_signal (bindings, GDK_KEY_t, GDK_CONTROL_MASK, "activate-action", 3, G_TYPE_STRING, "session", G_TYPE_STRING, "new-draft", G_TYPE_STRING, "");
  gtk_binding_entry_add_signal (bindings, GDK_KEY_n, GDK_CONTROL_MASK, "activate-action", 3, G_TYPE_STRING, "app", G_TYPE_STRING, "new-window", G_TYPE_STRING, "");
  gtk_binding_entry_add_signal (bindings, GDK_KEY_s, GDK_CONTROL_MASK, "activate-action", 3, G_TYPE_STRING, "page", G_TYPE_STRING, "save", G_TYPE_STRING, "");
  gtk_binding_entry_add_signal (bindings, GDK_KEY_s, GDK_CONTROL_MASK | GDK_SHIFT_MASK, "activate-action", 3, G_TYPE_STRING, "page", G_TYPE_STRING, "save-as", G_TYPE_STRING, "");
  gtk_binding_entry_add_signal (bindings, GDK_KEY_p, GDK_CONTROL_MASK, "activate-action", 3, G_TYPE_STRING, "page", G_TYPE_STRING, "print", G_TYPE_STRING, "");
  gtk_binding_entry_add_signal (bindings, GDK_KEY_c, GDK_CONTROL_MASK | GDK_SHIFT_MASK, "activate-action", 3, G_TYPE_STRING, "page", G_TYPE_STRING, "copy-all", G_TYPE_STRING, "");
  gtk_binding_entry_add_signal (bindings, GDK_KEY_1, GDK_MOD1_MASK, "activate-action", 3, G_TYPE_STRING, "page", G_TYPE_STRING, "change", G_TYPE_STRING, "1");
  gtk_binding_entry_add_signal (bindings, GDK_KEY_2, GDK_MOD1_MASK, "activate-action", 3, G_TYPE_STRING, "page", G_TYPE_STRING, "change", G_TYPE_STRING, "2");
  gtk_binding_entry_add_signal (bindings, GDK_KEY_3, GDK_MOD1_MASK, "activate-action", 3, G_TYPE_STRING, "page", G_TYPE_STRING, "change", G_TYPE_STRING, "3");
  gtk_binding_entry_add_signal (bindings, GDK_KEY_4, GDK_MOD1_MASK, "activate-action", 3, G_TYPE_STRING, "page", G_TYPE_STRING, "change", G_TYPE_STRING, "4");
  gtk_binding_entry_add_signal (bindings, GDK_KEY_5, GDK_MOD1_MASK, "activate-action", 3, G_TYPE_STRING, "page", G_TYPE_STRING, "change", G_TYPE_STRING, "5");
  gtk_binding_entry_add_signal (bindings, GDK_KEY_6, GDK_MOD1_MASK, "activate-action", 3, G_TYPE_STRING, "page", G_TYPE_STRING, "change", G_TYPE_STRING, "6");
  gtk_binding_entry_add_signal (bindings, GDK_KEY_7, GDK_MOD1_MASK, "activate-action", 3, G_TYPE_STRING, "page", G_TYPE_STRING, "change", G_TYPE_STRING, "7");
  gtk_binding_entry_add_signal (bindings, GDK_KEY_8, GDK_MOD1_MASK, "activate-action", 3, G_TYPE_STRING, "page", G_TYPE_STRING, "change", G_TYPE_STRING, "8");
  gtk_binding_entry_add_signal (bindings, GDK_KEY_9, GDK_MOD1_MASK, "activate-action", 3, G_TYPE_STRING, "page", G_TYPE_STRING, "change", G_TYPE_STRING, "9");
  gtk_binding_entry_add_signal (bindings, GDK_KEY_Page_Up, GDK_CONTROL_MASK | GDK_MOD1_MASK | GDK_SHIFT_MASK, "activate-action", 3, G_TYPE_STRING, "page", G_TYPE_STRING, "move-left", G_TYPE_STRING, "");
  gtk_binding_entry_add_signal (bindings, GDK_KEY_Page_Down, GDK_CONTROL_MASK | GDK_MOD1_MASK | GDK_SHIFT_MASK, "activate-action", 3, G_TYPE_STRING, "page", G_TYPE_STRING, "move-right", G_TYPE_STRING, "");
  gtk_binding_entry_add_signal (bindings, GDK_KEY_n, GDK_CONTROL_MASK | GDK_SHIFT_MASK, "activate-action", 3, G_TYPE_STRING, "page", G_TYPE_STRING, "move-to-new-window", G_TYPE_STRING, "");
  gtk_binding_entry_add_signal (bindings, GDK_KEY_f, GDK_CONTROL_MASK, "activate-action", 3, G_TYPE_STRING, "page", G_TYPE_STRING, "begin-search", G_TYPE_STRING, "");
  gtk_binding_entry_add_signal (bindings, GDK_KEY_h, GDK_CONTROL_MASK, "activate-action", 3, G_TYPE_STRING, "page", G_TYPE_STRING, "begin-replace", G_TYPE_STRING, "");

  _editor_window_class_actions_init (klass);

  g_type_ensure (EDITOR_TYPE_BIN);
  g_type_ensure (EDITOR_TYPE_POSITION_LABEL);
  g_type_ensure (EDITOR_TYPE_SIDEBAR);
}

static void
editor_window_init (EditorWindow *self)
{
  gtk_widget_init_template (GTK_WIDGET (self));

  gtk_window_set_title (GTK_WINDOW (self), _(PACKAGE_NAME));
  gtk_window_set_default_size (GTK_WINDOW (self), 700, 520);

  self->settings = g_settings_new ("org.gnome.TextEditor");
  g_settings_bind (self->settings, "show-line-numbers",
                   self->position_box, "visible",
                   G_SETTINGS_BIND_GET);

  g_signal_connect_swapped (self->notebook,
                            "page-added",
                            G_CALLBACK (editor_window_page_added_cb),
                            self);
  g_signal_connect_swapped (self->notebook,
                            "page-removed",
                            G_CALLBACK (editor_window_page_removed_cb),
                            self);
  g_signal_connect_swapped (self->notebook,
                            "notify::page",
                            G_CALLBACK (editor_window_notify_current_page_cb),
                            self);

  self->page_signals = editor_signal_group_new (EDITOR_TYPE_PAGE);

  g_signal_connect_object (self->page_signals,
                           "bind",
                           G_CALLBACK (editor_window_page_bind_cb),
                           self,
                           G_CONNECT_SWAPPED);

  g_signal_connect_object (self->page_signals,
                           "unbind",
                           G_CALLBACK (editor_window_page_unbind_cb),
                           self,
                           G_CONNECT_SWAPPED);

  editor_signal_group_connect_object (self->page_signals,
                                      "notify::can-save",
                                      G_CALLBACK (editor_window_update_actions),
                                      self,
                                      G_CONNECT_SWAPPED);

  self->document_signals = editor_signal_group_new (EDITOR_TYPE_DOCUMENT);

  editor_signal_group_connect_object (self->document_signals,
                                      "cursor-moved",
                                      G_CALLBACK (editor_window_cursor_moved_cb),
                                      self,
                                      G_CONNECT_SWAPPED);

  self->page_bindings = editor_binding_group_new ();

  editor_binding_group_bind (self->page_bindings, "title",
                             self->title, "label",
                             G_BINDING_SYNC_CREATE);
  editor_binding_group_bind (self->page_bindings, "subtitle",
                             self->subtitle, "label",
                             G_BINDING_SYNC_CREATE);
  editor_binding_group_bind_full (self->page_bindings, "subtitle",
                                  self->subtitle, "visible",
                                  G_BINDING_SYNC_CREATE,
                                  _editor_gchararray_to_boolean,
                                  NULL, NULL, NULL);
  editor_binding_group_bind (self->page_bindings, "is-modified",
                             self->is_modified, "visible",
                             G_BINDING_SYNC_CREATE);

  _editor_window_dnd_init (self);

  _editor_window_actions_init (self);
}

EditorWindow *
_editor_window_new (void)
{
  return g_object_new (EDITOR_TYPE_WINDOW,
                       "application", EDITOR_APPLICATION_DEFAULT,
                       NULL);
}

GList *
_editor_window_get_pages (EditorWindow *self)
{
  GQueue queue = G_QUEUE_INIT;
  guint n_pages;

  g_return_val_if_fail (EDITOR_IS_WINDOW (self), NULL);

  n_pages = gtk_notebook_get_n_pages (self->notebook);

  for (guint i = 0; i < n_pages; i++)
    {
      GtkWidget *child = gtk_notebook_get_nth_page (self->notebook, i);
      g_queue_push_tail (&queue, EDITOR_PAGE (child));
    }

  return g_steal_pointer (&queue.head);
}

void
_editor_window_add_page (EditorWindow *self,
                         EditorPage   *page)
{
  EditorTab *tab;

  g_return_if_fail (EDITOR_IS_WINDOW (self));
  g_return_if_fail (EDITOR_IS_PAGE (page));

  tab = _editor_tab_new (page);

  gtk_notebook_append_page (self->notebook,
                            GTK_WIDGET (page),
                            GTK_WIDGET (tab));

  gtk_container_child_set (GTK_CONTAINER (self->notebook), GTK_WIDGET (page),
                           "reorderable", TRUE,
                           NULL);
}

void
_editor_window_remove_page (EditorWindow *self,
                            EditorPage   *page)
{
  gint page_num;

  g_return_if_fail (EDITOR_IS_WINDOW (self));
  g_return_if_fail (EDITOR_IS_PAGE (page));

  page_num = gtk_notebook_page_num (self->notebook, GTK_WIDGET (page));
  if (page_num >= 0)
    gtk_notebook_remove_page (self->notebook, page_num);

  if (self->visible_page == page)
    {
      editor_window_notify_current_page_cb (self, NULL, self->notebook);

      if (self->visible_page != NULL)
        gtk_widget_grab_focus (GTK_WIDGET (self->visible_page));
    }
}

/**
 * editor_window_get_visible_page:
 * @self: a #EditorWindow
 *
 * Gets the visible page for the window.
 *
 * Returns: (transfer none) (nullable): an #EditorPage or %NULL
 */
EditorPage *
editor_window_get_visible_page (EditorWindow *self)
{
  g_return_val_if_fail (EDITOR_IS_WINDOW (self), NULL);

  return self->visible_page;
}

/**
 * editor_window_set_visible_page:
 * @self: an #EditorWindow
 * @page: an #EditorPage
 *
 * Sets the currently visible page.
 */
void
editor_window_set_visible_page (EditorWindow *self,
                                EditorPage   *page)
{
  gint page_num;

  g_return_if_fail (EDITOR_IS_WINDOW (self));
  g_return_if_fail (EDITOR_IS_PAGE (page));

  page_num = gtk_notebook_page_num (self->notebook, GTK_WIDGET (page));

  if (page_num >= 0)
    gtk_notebook_set_current_page (self->notebook, page_num);
}

/**
 * editor_window_get_nth_page:
 * @self: a #EditorWindow
 * @nth: the page number starting from 0
 *
 * Gets the @nth page from the window.
 *
 * Returns: (transfer none) (nullable): an #EditorPage or %NULL
 */
EditorPage *
editor_window_get_nth_page (EditorWindow *self,
                            guint         nth)
{
  g_return_val_if_fail (EDITOR_IS_WINDOW (self), NULL);

  if (nth >= gtk_notebook_get_n_pages (self->notebook))
    return NULL;

  return EDITOR_PAGE (gtk_notebook_get_nth_page (self->notebook, nth));
}

/**
 * editor_window_get_n_pages:
 * @self: a #EditorWindow
 *
 * Gets the number of pages in @self.
 *
 * Returns: the number of pages in the window
 */
guint
editor_window_get_n_pages (EditorWindow *self)
{
  g_return_val_if_fail (EDITOR_IS_WINDOW (self), 0);

  return gtk_notebook_get_n_pages (self->notebook);
}

gboolean
_editor_window_get_sidebar_revealed (EditorWindow *self)
{
  g_return_val_if_fail (EDITOR_IS_WINDOW (self), FALSE);

  return gtk_toggle_button_get_active (self->open_toggle_button);
}

void
_editor_window_set_sidebar_revealed (EditorWindow *self,
                                     gboolean      sidebar_revealed)
{
  g_return_if_fail (EDITOR_IS_WINDOW (self));

  gtk_toggle_button_set_active (self->open_toggle_button, sidebar_revealed);
}

void
_editor_window_focus_search (EditorWindow *self)
{
  g_return_if_fail (EDITOR_IS_WINDOW (self));

  gtk_toggle_button_set_active (self->open_toggle_button, TRUE);
  editor_sidebar_focus_search (self->sidebar);
}
