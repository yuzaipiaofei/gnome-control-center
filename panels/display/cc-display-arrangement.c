/* -*- mode: c; style: linux -*-
 * 
 * Copyright (C) 2007, 2008, 2017  Red Hat, Inc.
 * Copyright (C) 2013 Intel, Inc.
 *
 * Written by: Benjamin Berg <bberg@redhat.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include <math.h>
#include "cc-display-arrangement.h"
#include "cc-display-config.h"

#define GNOME_DESKTOP_USE_UNSTABLE_API
#include <libgnome-desktop/gnome-bg.h>

struct _CcDisplayArrangement
{
  GtkDrawingArea object;
  CcDisplayConfig *config;

  cairo_matrix_t to_widget;
  cairo_matrix_t to_actual;

  GnomeDesktopThumbnailFactory *thumbnail_factory;

  gboolean          drag_active;
  CcDisplayMonitor *selected_output;
  CcDisplayMonitor *prelit_output;
  /* Starting position of cursor inside the monitor. */
  gdouble           drag_anchor_x;
  gdouble           drag_anchor_y;
};

typedef struct _CcDisplayArrangement CcDisplayArrangement;

enum {
  PROP_0,
  PROP_CONFIG,
  PROP_SELECTED_OUTPUT,
  PROP_LAST
};

enum SnapDirection {
  SNAP_DIR_NONE = 0,
  SNAP_DIR_X    = 1 << 0,
  SNAP_DIR_Y    = 1 << 1,
  SNAP_DIR_BOTH = (SNAP_DIR_X | SNAP_DIR_Y),
};

struct SnapData {
  gdouble            dist_x;
  gdouble            dist_y;
  gint               mon_x;
  gint               mon_y;
  enum SnapDirection snapped;
};

#define MARGIN_PX  0
#define MARGIN_MON  0.66
#define MAX_SNAP_DISTANCE 10

G_DEFINE_TYPE (CcDisplayArrangement, cc_display_arrangement, GTK_TYPE_DRAWING_AREA)

static GParamSpec *props[PROP_LAST];

static gboolean
is_output_useful (CcDisplayMonitor *output)
{
  return (cc_display_monitor_is_active (output) &&
          !g_object_get_data (G_OBJECT (output), "lid-is-closed"));
}

static guint
count_useful_outputs (CcDisplayConfig *config)
{
  GList *outputs, *l;
  guint active = 0;

  outputs = cc_display_config_get_monitors (config);
  for (l = outputs; l != NULL; l = l->next)
    {
      CcDisplayMonitor *output = l->data;
      if (!is_output_useful (output))
        continue;
      else
        active++;
    }
  return active;
}

static void
apply_rotation_to_geometry (CcDisplayMonitor *output,
			    int *w,
			    int *h)
{
  CcDisplayRotation rotation;

  rotation = cc_display_monitor_get_rotation (output);
  if ((rotation == CC_DISPLAY_ROTATION_90) || (rotation == CC_DISPLAY_ROTATION_270))
    {
      int tmp;
      tmp = *h;
      *h = *w;
      *w = tmp;
    }
}

/* get_geometry */
static void
get_scaled_geometry (CcDisplayConfig *config, CcDisplayMonitor *output, int *x, int *y, int *w, int *h)
{
  if (cc_display_monitor_is_active (output))
    {
      cc_display_monitor_get_geometry (output, x, y, w, h);
    }
  else
    {
      cc_display_monitor_get_geometry (output, x, y, NULL, NULL);
      cc_display_mode_get_resolution (cc_display_monitor_get_preferred_mode (output),
                                      w, h);
    }

  if (cc_display_config_is_layout_logical (config))
    {
      double scale = cc_display_monitor_get_scale (output);
      *w /= scale;
      *h /= scale;
    }

  apply_rotation_to_geometry (output, w, h);
}

static void
get_bounding_box (CcDisplayConfig *config,
		  gint            *x1,
		  gint            *y1,
		  gint            *x2,
		  gint            *y2,
		  gint            *max_w,
		  gint            *max_h)
{
  GList *outputs, *l;

  g_assert (x1 && y1 && x2 && y2);

  *x1 = *y1 = G_MAXINT;
  *x2 = *y2 = G_MININT;
  *max_w = 0;
  *max_h = 0;

  outputs = cc_display_config_get_monitors (config);
  for (l = outputs; l != NULL; l = l->next)
    {
      CcDisplayMonitor *output = l->data;
      int x, y, w, h;

      if (!is_output_useful (output))
        continue;

      get_scaled_geometry (config, output, &x, &y, &w, &h);

      *x1 = MIN(*x1, x);
      *y1 = MIN(*y1, y);
      *x2 = MAX(*x2, x + w);
      *y2 = MAX(*y2, y + h);
      *max_w = MAX(*max_w, w);
      *max_h = MAX(*max_h, h);
    }
}

static void
monitor_get_drawing_rect (CcDisplayArrangement *arr,
			  CcDisplayMonitor     *output,
			  gint                 *x1,
			  gint                 *y1,
			  gint                 *x2,
			  gint                 *y2)
{
  gdouble x, y;

  get_scaled_geometry (arr->config, output, x1, y1, x2, y2);

  /* get_scaled_geometry returns the width and height */
  *x2 = *x1 + *x2;
  *y2 = *y1 + *y2;

  x = *x1; y = *y1;
  cairo_matrix_transform_point (&arr->to_widget, &x, &y);
  *x1 = round (x);
  *y1 = round (y);

  x = *x2; y = *y2;
  cairo_matrix_transform_point (&arr->to_widget, &x, &y);
  *x2 = round (x);
  *y2 = round (y);
}


static void
get_snap_distance (CcDisplayArrangement *arr,
		   gint                  mon_x,
		   gint                  mon_y,
		   gint                  new_x,
		   gint                  new_y,
		   gdouble              *dist_x,
		   gdouble              *dist_y)
{
  gdouble _dist_x, _dist_y;

  _dist_x = ABS (mon_x - new_x);
  _dist_y = ABS (mon_y - new_y);

  cairo_matrix_transform_distance (&arr->to_widget, &_dist_x, &_dist_y);

  if (dist_x)
    *dist_x = _dist_x;
  if (dist_y)
    *dist_y = _dist_y;
}

static void
maybe_update_snap (CcDisplayArrangement *arr,
		   struct SnapData      *snap_data,
		   gint                  mon_x,
		   gint                  mon_y,
		   gint                  new_x,
		   gint                  new_y,
		   gboolean              snapped)
{
  enum SnapDirection update_snap = SNAP_DIR_NONE;
  gdouble dist_x, dist_y;
  gdouble dist;

  get_snap_distance (arr, mon_x, mon_y, new_x, new_y, &dist_x, &dist_y);
  dist = MAX (dist_x, dist_y);

  if (dist > MAX_SNAP_DISTANCE)
    return;

  if (snapped == SNAP_DIR_BOTH)
    {
      if (snap_data->snapped == SNAP_DIR_NONE)
        update_snap = SNAP_DIR_BOTH;

      if (dist < MAX(snap_data->dist_x, snap_data->dist_y))
        update_snap = SNAP_DIR_BOTH;

      if (dist == MAX(snap_data->dist_x, snap_data->dist_y) && (dist_x < snap_data->dist_x || dist_y < snap_data->dist_y))
        update_snap = SNAP_DIR_BOTH;
    }
  else if (snapped == SNAP_DIR_X)
    {
      if (dist_x < snap_data->dist_x || (snap_data->snapped & SNAP_DIR_X) == SNAP_DIR_NONE)
        update_snap = SNAP_DIR_X;
    }
  else if (snapped == SNAP_DIR_Y)
    {
      if (dist_y < snap_data->dist_y || (snap_data->snapped & SNAP_DIR_Y) == SNAP_DIR_NONE)
        update_snap = SNAP_DIR_Y;
    }
  else
    {
      g_assert_not_reached ();
    }

  if (update_snap & SNAP_DIR_X)
    {
      snap_data->dist_x = dist_x;
      snap_data->mon_x = new_x;
      snap_data->snapped = snap_data->snapped | SNAP_DIR_X;
    }
  if (update_snap & SNAP_DIR_Y)
    {
      snap_data->dist_y = dist_y;
      snap_data->mon_y = new_y;
      snap_data->snapped = snap_data->snapped | SNAP_DIR_Y;
    }
}

#define OVERLAP(_s1, _s2, _t1, _t2) ((_s1) <= (_t2) && (_t1) <= (_s2))

static void
find_best_snapping (CcDisplayArrangement *arr,
		    CcDisplayConfig      *config,
		    CcDisplayMonitor     *snap_output,
		    struct SnapData      *snap_data)
{
  GList *outputs, *l;
  gint x1, y1, x2, y2;
  gint w, h;

  g_assert (snap_data != NULL);

  get_scaled_geometry (config, snap_output, &x1, &y1, &w, &h);
  x2 = x1 + w;
  y2 = y1 + h;

  outputs = cc_display_config_get_monitors (config);
  for (l = outputs; l; l = l->next)
    {
      CcDisplayMonitor *output = l->data;
      gint _x1, _y1, _x2, _y2, _h, _w;
      gint bottom_snap_pos;
      gint top_snap_pos;
      gint left_snap_pos;
      gint right_snap_pos;
      gdouble dist_x, dist_y;
      gdouble tmp;

      if (output == snap_output)
        continue;

      if (!is_output_useful (output))
        continue;

      get_scaled_geometry (config, output, &_x1, &_y1, &_w, &_h);
      _x2 = _x1 + _w;
      _y2 = _y1 + _h;

      top_snap_pos = _y1 - h;
      bottom_snap_pos = _y2;
      left_snap_pos = _x1 - w;
      right_snap_pos = _x2;

      dist_y = 9999;
      /* overlap on the X axis */
      if (OVERLAP(x1, x2, _x1, _x2))
        {
          get_snap_distance (arr, x1, y1, x1, top_snap_pos, NULL, &dist_y);
          get_snap_distance (arr, x1, y1, x1, bottom_snap_pos, NULL, &tmp);
          dist_y = MIN(dist_y, tmp);
        }

      dist_x = 9999;
      /* overlap on the Y axis */
      if (OVERLAP(y1, y2, _y1, _y2))
        {
          get_snap_distance (arr, x1, y1, left_snap_pos, y1, &dist_x, NULL);
          get_snap_distance (arr, x1, y1, right_snap_pos, y1, &tmp, NULL);
          dist_x = MIN(dist_x, tmp);
        }

      /* We only snap horizontally or vertically to an edge of the same monitor */
      if (dist_y < dist_x)
        {
          maybe_update_snap (arr, snap_data, x1, y1, x1, top_snap_pos, SNAP_DIR_Y);
          maybe_update_snap (arr, snap_data, x1, y1, x1, bottom_snap_pos, SNAP_DIR_Y);
        }
      else if (dist_x < 9999)
        {
          maybe_update_snap (arr, snap_data, x1, y1, left_snap_pos, y1, SNAP_DIR_X);
          maybe_update_snap (arr, snap_data, x1, y1, right_snap_pos, y1, SNAP_DIR_X);
        }

      /* Left/right edge identical on the top */
      maybe_update_snap (arr, snap_data, x1, y1, _x1, top_snap_pos, SNAP_DIR_BOTH);
      maybe_update_snap (arr, snap_data, x1, y1, _x2 - w, top_snap_pos, SNAP_DIR_BOTH);

      /* Left/right edge identical on the bottom */
      maybe_update_snap (arr, snap_data, x1, y1, _x1, bottom_snap_pos, SNAP_DIR_BOTH);
      maybe_update_snap (arr, snap_data, x1, y1, _x2 - w, bottom_snap_pos, SNAP_DIR_BOTH);

      /* Top/bottom edge identical on the left */
      maybe_update_snap (arr, snap_data, x1, y1, left_snap_pos, _y1, SNAP_DIR_BOTH);
      maybe_update_snap (arr, snap_data, x1, y1, left_snap_pos, _y2 - h, SNAP_DIR_BOTH);

      /* Top/bottom edge identical on the right */
      maybe_update_snap (arr, snap_data, x1, y1, right_snap_pos, _y1, SNAP_DIR_BOTH);
      maybe_update_snap (arr, snap_data, x1, y1, right_snap_pos, _y2 - h, SNAP_DIR_BOTH);
    }
}
#undef OVERLAP

static void
cc_display_arrangement_update_matrices (CcDisplayArrangement *arr)
{
  gint x1, y1, x2, y2, max_w, max_h;
  GtkAllocation allocation;
  gdouble scale, scale_h, scale_w;

  g_assert (arr->config);

  /* Do not update the matrices while the user is dragging things around. */
  if (arr->drag_active)
    return;

  get_bounding_box (arr->config, &x1, &y1, &x2, &y2, &max_w, &max_h);
  gtk_widget_get_allocation (GTK_WIDGET (arr), &allocation);

  scale_h = (gdouble) (allocation.width - 2 * MARGIN_PX) / (x2 - x1 + max_w * 2 * MARGIN_MON);
  scale_w = (gdouble) (allocation.height - 2 * MARGIN_PX) / (y2 - y1 + max_h * 2 * MARGIN_MON);

  scale = MIN (scale_h, scale_w);

  cairo_matrix_init_identity (&arr->to_widget);
  cairo_matrix_translate (&arr->to_widget, allocation.width / 2.0, allocation.height / 2.0);
  cairo_matrix_scale (&arr->to_widget, scale, scale);
  cairo_matrix_translate (&arr->to_widget, - (x1 + x2) / 2.0, - (y1 + y2) / 2.0);

  arr->to_actual = arr->to_widget;
  cairo_matrix_invert (&arr->to_actual);
}

static CcDisplayMonitor*
cc_display_arrangement_find_monitor_at (CcDisplayArrangement *arr,
					gint                  x,
					gint                  y)
{
  GList *outputs, *l;

  outputs = g_list_copy (cc_display_config_get_monitors (arr->config));
  if (arr->selected_output)
    outputs = g_list_prepend (outputs, arr->selected_output);

  for (l = outputs; l; l = l->next)
    {
      CcDisplayMonitor *output = l->data;
      gint x1, y1, x2, y2;

      if (!is_output_useful (output))
        continue;

      monitor_get_drawing_rect (arr, output, &x1, &y1, &x2, &y2);
      if (x >= x1 && x <= x2 && y >= y1 && y <= y2)
        return output;
    }

  g_list_free (outputs);

  return NULL;
}

static void
cc_display_arrangement_update_cursor (CcDisplayArrangement *arr,
				      gboolean              dragable)
{
  GdkCursor *cursor;
  GdkWindow *window;

  if (dragable)
    cursor = gdk_cursor_new_for_display (gtk_widget_get_display (GTK_WIDGET (arr)), GDK_FLEUR);
  else
    cursor = NULL;

  window = gtk_widget_get_window (GTK_WIDGET (arr));

  if (window)
    gdk_window_set_cursor (window, cursor);

  g_clear_object (&cursor);
}

static void
output_changed_cb (CcDisplayArrangement *arr, CcDisplayMonitor *output)
{
  gtk_widget_queue_draw (GTK_WIDGET (arr));
}

static void
cc_display_arrangement_set_config (CcDisplayArrangement *arr,
				   CcDisplayConfig      *config)
{
  const gchar *signals[] = { "rotation", "mode", "primary", "active", "scale", "position-changed" };
  GList *outputs, *l;
  guint i;

  g_assert (arr->config == NULL);

  arr->config = g_object_ref (config);

  /* Listen to all the signals */
  if (arr->config)
    {
      outputs = cc_display_config_get_monitors (arr->config);
      for (l = outputs; l; l = l->next)
        {
          CcDisplayMonitor *output = l->data;
          for (i = 0; i < G_N_ELEMENTS (signals); ++i)
            {
              g_signal_connect_object (output, signals[i], G_CALLBACK (output_changed_cb),
                                       arr, G_CONNECT_SWAPPED);
            }
        }
    }

  cc_display_arrangement_set_selected_output (arr, NULL);

  g_object_notify_by_pspec (G_OBJECT (arr), props[PROP_CONFIG]);
}

static gboolean
cc_display_arrangement_draw (GtkWidget *widget,
			     cairo_t *cr)
{
  CcDisplayArrangement *arr = CC_DISPLAY_ARRANGEMENT (widget);
  GtkStyleContext *context = gtk_widget_get_style_context (widget);
  GList *outputs, *l;

  g_return_val_if_fail (arr->config, FALSE);

  cc_display_arrangement_update_matrices (arr);

  gtk_style_context_save (context);
  gtk_style_context_add_class (context, "display-arrangement");

  /* Draw in reverse order so that hit detection matches visual. Also pull
   * the selected output to the back. */
  outputs = g_list_copy (cc_display_config_get_monitors (arr->config));
  outputs = g_list_remove (outputs, arr->selected_output);
  if (arr->selected_output != NULL)
    outputs = g_list_prepend (outputs, arr->selected_output);
  outputs = g_list_reverse (outputs);
  for (l = outputs; l; l = l->next)
    {
      CcDisplayMonitor *output = l->data;
      GtkStateFlags state = GTK_STATE_FLAG_NORMAL;
      GtkBorder border, padding, margin;
      gint x1, y1, x2, y2;
      gint w, h;
      gint num;

      if (!is_output_useful (output))
        continue;

      gtk_style_context_save (context);
      cairo_save (cr);

      gtk_style_context_add_class (context, "monitor");

      if (output == arr->selected_output)
        state |= GTK_STATE_FLAG_SELECTED;
      if (output == arr->prelit_output)
        state |= GTK_STATE_FLAG_PRELIGHT;

      gtk_style_context_set_state (context, state);
      if (cc_display_monitor_is_primary (output) || cc_display_config_is_cloning (arr->config))
        gtk_style_context_add_class (context, "primary");

      /* Set in cc-display-panel.c */
      num = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (output), "ui-number"));

      monitor_get_drawing_rect (arr, output, &x1, &y1, &x2, &y2);
      w = x2 - x1;
      h = y2 - y1;

      cairo_translate (cr, x1, y1);

      gtk_style_context_get_margin (context, state, &margin);

      cairo_translate (cr, margin.left, margin.top);

      w -= margin.left + margin.right;
      h -= margin.top + margin.bottom;

      gtk_render_background (context, cr, 0, 0, w, h);
      gtk_render_frame (context, cr, 0, 0, w, h);

      gtk_style_context_get_border (context, state, &border);
      gtk_style_context_get_padding (context, state, &padding);

      w -= border.left + border.right + padding.left + padding.right;
      h -= border.top + border.bottom + padding.top + padding.bottom;

      cairo_translate (cr, border.left + padding.left, border.top + padding.top);

      if (num > 0)
        {
          PangoLayout *layout;
          PangoFontDescription *font = NULL;
          gchar *number_str;
          PangoRectangle extents;
          GdkRGBA color;
          gdouble text_width, text_padding;

          gtk_style_context_add_class (context, "monitor-label");
          gtk_style_context_remove_class (context, "monitor");

          gtk_style_context_get_border (context, state, &border);
          gtk_style_context_get_padding (context, state, &padding);
          gtk_style_context_get_margin (context, state, &margin);

          cairo_translate (cr, margin.left, margin.top);

          number_str = g_strdup_printf ("%d", num);
          gtk_style_context_get (context, state, "font", &font, NULL);
          layout = gtk_widget_create_pango_layout (GTK_WIDGET (arr), number_str);
          pango_layout_set_font_description (layout, font);
          pango_layout_get_extents (layout, NULL, &extents);
          g_free (number_str);

          h = (extents.height - extents.y) / PANGO_SCALE;
          text_width = (extents.width - extents.x) / PANGO_SCALE;
          w = MAX (text_width, h - padding.left - padding.right);
          text_padding = w - text_width;

          w += border.left + border.right + padding.left + padding.right;
          h += border.top + border.bottom + padding.top + padding.bottom;

          gtk_render_background (context, cr, 0, 0, w, h);
          gtk_render_frame (context, cr, 0, 0, w, h);

          cairo_translate (cr, border.left + padding.left, border.top + padding.top);
          cairo_translate (cr, extents.x + text_padding / 2, 0);

          gtk_style_context_get_color (context, state, &color);
          gdk_cairo_set_source_rgba (cr, &color);

          gtk_render_layout (context, cr, 0, 0, layout);
          g_object_unref (layout);
        }

      gtk_style_context_restore (context);
      cairo_restore (cr);
    }
  g_list_free (outputs);

  gtk_style_context_restore (context);

  return TRUE;
}

static gboolean
cc_display_arrangement_button_press_event (GtkWidget      *widget,
					   GdkEventButton *event)
{
  CcDisplayArrangement *arr = CC_DISPLAY_ARRANGEMENT (widget);
  CcDisplayMonitor *output;
  gdouble event_x, event_y;
  gint mon_x, mon_y;

  /* Only handle normal presses of the left mouse button. */
  if (event->button != 1 || event->type != GDK_BUTTON_PRESS)
    return FALSE;

  g_return_val_if_fail (arr->drag_active == FALSE, FALSE);

  output = cc_display_arrangement_find_monitor_at (arr, event->x, event->y);
  if (!output)
    return FALSE;

  event_x = event->x;
  event_y = event->y;

  cairo_matrix_transform_point (&arr->to_actual, &event_x, &event_y);
  cc_display_monitor_get_geometry (output, &mon_x, &mon_y, NULL, NULL);

  cc_display_arrangement_set_selected_output (arr, output);

  if (count_useful_outputs (arr->config) > 1)
    {
      arr->drag_active = TRUE;
      arr->drag_anchor_x = event_x - mon_x;
      arr->drag_anchor_y = event_y - mon_y;
    }

  return TRUE;
}

static gboolean
cc_display_arrangement_button_release_event (GtkWidget      *widget,
					     GdkEventButton *event)
{
  CcDisplayArrangement *arr = CC_DISPLAY_ARRANGEMENT (widget);
  CcDisplayMonitor *output;

  /* Only handle left mouse button */
  if (event->button != 1)
    return FALSE;

  if (!arr->drag_active)
    return FALSE;

  arr->drag_active = FALSE;

  output = cc_display_arrangement_find_monitor_at (arr, event->x, event->y);
  cc_display_arrangement_update_cursor (arr, output != NULL);

  /* And queue a redraw as the to recenter everything*/
  gtk_widget_queue_draw (widget);

  g_signal_emit_by_name (G_OBJECT (widget), "updated");

  return TRUE;
}

static gboolean
cc_display_arrangement_motion_notify_event (GtkWidget      *widget,
					    GdkEventMotion *event)
{
  CcDisplayArrangement *arr = CC_DISPLAY_ARRANGEMENT (widget);
  gdouble event_x, event_y;
  gint mon_x, mon_y;
  struct SnapData snap_data;

  g_return_val_if_fail (arr->config, FALSE);

  if (count_useful_outputs (arr->config) <= 1)
    return FALSE;

  if (!arr->drag_active)
    {
      CcDisplayMonitor *output;
      output = cc_display_arrangement_find_monitor_at (arr, event->x, event->y);

      cc_display_arrangement_update_cursor (arr, output != NULL);
      if (arr->prelit_output != output)
        gtk_widget_queue_draw (widget);

      arr->prelit_output = output;

      return FALSE;
    }

  g_assert (arr->selected_output);

  event_x = event->x;
  event_y = event->y;

  cairo_matrix_transform_point (&arr->to_actual, &event_x, &event_y);

  mon_x = round(event_x - arr->drag_anchor_x);
  mon_y = round(event_y - arr->drag_anchor_y);

  /* The monitor is now at the location as if there was no snapping whatsoever. */
  snap_data.snapped = SNAP_DIR_NONE;
  snap_data.mon_x = mon_x;
  snap_data.mon_y = mon_y;
  snap_data.dist_x = 0;
  snap_data.dist_y = 0;

  cc_display_monitor_set_position (arr->selected_output, mon_x, mon_y);

  find_best_snapping (arr, arr->config, arr->selected_output, &snap_data);

  cc_display_monitor_set_position (arr->selected_output, snap_data.mon_x, snap_data.mon_y);

  return TRUE;
}

static void
cc_display_arrangement_get_property (GObject    *object,
				     guint       prop_id,
				     GValue     *value,
				     GParamSpec *pspec)
{
  CcDisplayArrangement *arr = CC_DISPLAY_ARRANGEMENT (object);

  switch (prop_id)
    {
    case PROP_CONFIG:
      g_value_set_object (value, arr->config);
      break;

    case PROP_SELECTED_OUTPUT:
      g_value_set_object (value, arr->selected_output);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
cc_display_arrangement_set_property (GObject      *object,
				     guint         prop_id,
				     const GValue *value,
				     GParamSpec   *pspec)
{
  CcDisplayArrangement *obj = CC_DISPLAY_ARRANGEMENT (object);

  switch (prop_id)
    {
    case PROP_CONFIG:
      cc_display_arrangement_set_config (obj, g_value_get_object (value));
      break;

    case PROP_SELECTED_OUTPUT:
      cc_display_arrangement_set_selected_output (obj, g_value_get_object (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
cc_display_arrangement_finalize (GObject *object)
{
  CcDisplayArrangement *arr = CC_DISPLAY_ARRANGEMENT (object);

  g_clear_object (&arr->config);
  g_clear_object (&arr->thumbnail_factory);
}

static void
cc_display_arrangement_class_init (CcDisplayArrangementClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  gobject_class->finalize = cc_display_arrangement_finalize;
  gobject_class->get_property = cc_display_arrangement_get_property;
  gobject_class->set_property = cc_display_arrangement_set_property;

  widget_class->draw = cc_display_arrangement_draw;
  widget_class->button_press_event = cc_display_arrangement_button_press_event;
  widget_class->button_release_event = cc_display_arrangement_button_release_event;
  widget_class->motion_notify_event = cc_display_arrangement_motion_notify_event;

  props[PROP_CONFIG] = g_param_spec_object ("config", "Display Config",
					    "The display configuration to work with",
					    CC_TYPE_DISPLAY_CONFIG,
					    G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);

  props[PROP_SELECTED_OUTPUT] = g_param_spec_object ("selected-output", "Selected Output",
						     "The output that is currently selected on the configuration",
						     CC_TYPE_DISPLAY_MONITOR,
						     G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (gobject_class,
				     PROP_LAST,
				     props);

  g_signal_new ("updated",
                CC_TYPE_DISPLAY_ARRANGEMENT,
                G_SIGNAL_RUN_LAST,
                0, NULL, NULL, NULL,
                G_TYPE_NONE, 0);
}

static void
cc_display_arrangement_init (CcDisplayArrangement *arr)
{
  /* XXX: Do we need to listen to touch events here? */
  gtk_widget_add_events (GTK_WIDGET (arr),
			 GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_POINTER_MOTION_MASK);
}

CcDisplayArrangement*
cc_display_arrangement_new (CcDisplayConfig *config)
{
  return g_object_new (CC_TYPE_DISPLAY_ARRANGEMENT, "config", config, NULL);
}

CcDisplayMonitor*
cc_display_arrangement_get_selected_output (CcDisplayArrangement *arr)
{
  return arr->selected_output;
}

void
cc_display_arrangement_set_selected_output (CcDisplayArrangement *arr,
					    CcDisplayMonitor     *output)
{
  g_return_if_fail (arr->drag_active == FALSE);

  /* XXX: Could check that it actually belongs to the right config object. */
  arr->selected_output = output;

  g_object_notify_by_pspec (G_OBJECT (arr), props[PROP_SELECTED_OUTPUT]);
}