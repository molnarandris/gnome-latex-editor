/* editor-source-view.c
 *
 * Copyright 2021 Christian Hergert <chergert@redhat.com>
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

#include "config.h"

#include "editor-document-private.h"
#include "editor-source-view.h"
#include "editor-joined-menu-private.h"
#include "editor-spell-menu.h"

struct _EditorSourceView
{
  GtkSourceView parent_instance;
  GMenuModel *spelling_menu;
};

G_DEFINE_TYPE (EditorSourceView, editor_source_view, GTK_SOURCE_TYPE_VIEW)

static gboolean
on_key_pressed_cb (GtkEventControllerKey *key,
                   guint                  keyval,
                   guint                  keycode,
                   GdkModifierType        state,
                   GtkWidget             *widget)
{
  /* This seems to be the easiest way to reliably override the keybindings
   * from GtkTextView into something we want (which is to use them for moving
   * through the tabs.
   */

  if ((state & GDK_CONTROL_MASK) == 0)
    return FALSE;

  if (state & ~(GDK_CONTROL_MASK|GDK_SHIFT_MASK))
    return FALSE;

  switch (keyval)
    {
    case GDK_KEY_Page_Up:
    case GDK_KEY_KP_Page_Up:
      if (state & GDK_SHIFT_MASK)
        gtk_widget_activate_action (widget, "page.move-left", NULL);
      else
        gtk_widget_activate_action (widget, "win.focus-neighbor", "i", -1);
      return TRUE;

    case GDK_KEY_Page_Down:
    case GDK_KEY_KP_Page_Down:
      if (state & GDK_SHIFT_MASK)
        gtk_widget_activate_action (widget, "page.move-right", NULL);
      else
        gtk_widget_activate_action (widget, "win.focus-neighbor", "i", 1);
      return TRUE;

    default:
      break;
    }

  return FALSE;
}

static void
tweak_gutter_spacing (GtkSourceView *view)
{
  GtkSourceGutter *gutter;
  GtkWidget *child;
  guint n = 0;

  g_assert (GTK_SOURCE_IS_VIEW (view));

  /* Ensure we have a line gutter renderer to tweak */
  gutter = gtk_source_view_get_gutter (view, GTK_TEXT_WINDOW_LEFT);
  gtk_source_view_set_show_line_numbers (view, TRUE);

  /* Add margin to first gutter renderer */
  for (child = gtk_widget_get_first_child (GTK_WIDGET (gutter));
       child != NULL;
       child = gtk_widget_get_next_sibling (child), n++)
    {
      if (GTK_SOURCE_IS_GUTTER_RENDERER (child))
        gtk_widget_set_margin_start (child, n == 0 ? 4 : 0);
    }
}

static void
on_click_pressed_cb (GtkGestureClick  *click,
                     int               n_press,
                     double            x,
                     double            y,
                     EditorSourceView *self)
{
  GdkEventSequence *sequence;
  GdkEvent *event;

  g_assert (EDITOR_IS_SOURCE_VIEW (self));
  g_assert (GTK_IS_GESTURE_CLICK (click));

  sequence = gtk_gesture_single_get_current_sequence (GTK_GESTURE_SINGLE (click));
  event = gtk_gesture_get_last_event (GTK_GESTURE (click), sequence);

  if (n_press == 1 && gdk_event_triggers_context_menu (event))
    {
      g_autofree char *word = NULL;
      GtkTextBuffer *buffer;
      GtkTextIter iter, begin, end;
      int buf_x, buf_y;

      editor_spell_menu_set_corrections (self->spelling_menu, NULL);

      /* Move the cursor position to where the click occurred so that
       * the context menu will be useful for the click location.
       */
      buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (self));
      if (gtk_text_buffer_get_selection_bounds (buffer, &begin, &end))
        return;

      gtk_text_view_window_to_buffer_coords (GTK_TEXT_VIEW (self),
                                             GTK_TEXT_WINDOW_WIDGET,
                                             x, y, &buf_x, &buf_y);
      gtk_text_view_get_iter_at_location (GTK_TEXT_VIEW (self), &iter, buf_x, buf_y);
      gtk_text_buffer_select_range (buffer, &iter, &iter);

      /* Get the word under the cursor */
      gtk_text_buffer_get_iter_at_mark (buffer, &iter, gtk_text_buffer_get_insert (buffer));
      begin = iter;
      if (!gtk_text_iter_starts_word (&begin))
        gtk_text_iter_backward_word_start (&begin);
      end = begin;
      if (!gtk_text_iter_ends_word (&end))
        gtk_text_iter_forward_word_end (&end);
      if (!gtk_text_iter_equal (&begin, &end) &&
          gtk_text_iter_compare (&begin, &iter) <= 0 &&
          gtk_text_iter_compare (&iter, &end) <= 0)
        {
          word = gtk_text_iter_get_slice (&begin, &end);

          if (!_editor_document_check_spelling (EDITOR_DOCUMENT (buffer), word))
            {
              g_auto(GStrv) corrections = _editor_document_list_corrections (EDITOR_DOCUMENT (buffer), word);
#if 0
              gtk_text_buffer_select_range (buffer, &begin, &end);
#endif
              editor_spell_menu_set_corrections (self->spelling_menu,
                                                 (const char * const *)corrections);
            }
        }
    }
}

static void
on_notify_buffer_cb (EditorSourceView *self,
                     GParamSpec       *pspec,
                     gpointer          unused)
{
  GtkTextBuffer *buffer;

  g_assert (EDITOR_IS_SOURCE_VIEW (self));

  buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (self));

  if (EDITOR_IS_DOCUMENT (buffer))
    _editor_document_attach_actions (EDITOR_DOCUMENT (buffer), GTK_WIDGET (self));
}

static void
editor_source_view_finalize (GObject *object)
{
  EditorSourceView *self = (EditorSourceView *)object;

  g_clear_object (&self->spelling_menu);

  G_OBJECT_CLASS (editor_source_view_parent_class)->finalize (object);
}

static void
editor_source_view_class_init (EditorSourceViewClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = editor_source_view_finalize;
}

static void
editor_source_view_init (EditorSourceView *self)
{
  g_autoptr(EditorJoinedMenu) joined = NULL;
  GtkEventController *controller;
  GMenuModel *extra_menu;

  g_signal_connect (self,
                    "notify::buffer",
                    G_CALLBACK (on_notify_buffer_cb),
                    NULL);

  controller = gtk_event_controller_key_new ();
  g_signal_connect (controller,
                    "key-pressed",
                    G_CALLBACK (on_key_pressed_cb),
                    self);
  gtk_widget_add_controller (GTK_WIDGET (self), controller);

  controller = GTK_EVENT_CONTROLLER (gtk_gesture_click_new ());
  gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (controller), 0);
  g_signal_connect (controller,
                    "pressed",
                    G_CALLBACK (on_click_pressed_cb),
                    self);
  gtk_widget_add_controller (GTK_WIDGET (self), controller);

  tweak_gutter_spacing (GTK_SOURCE_VIEW (self));

  joined = editor_joined_menu_new ();
  extra_menu = gtk_text_view_get_extra_menu (GTK_TEXT_VIEW (self));
  editor_joined_menu_append_menu (joined, extra_menu);

  self->spelling_menu = editor_spell_menu_new ();
  editor_joined_menu_append_menu (joined, self->spelling_menu);

  gtk_text_view_set_extra_menu (GTK_TEXT_VIEW (self), G_MENU_MODEL (joined));
}
