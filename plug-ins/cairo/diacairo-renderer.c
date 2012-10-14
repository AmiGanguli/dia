/* Dia -- an diagram creation/manipulation program
 * Copyright (C) 1998 Alexander Larsson
 *
 * diacairo.c -- Cairo based export plugin for dia
 * Copyright (C) 2004, Hans Breuer, <Hans@Breuer.Org>
 *   based on wpg.c 
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#include <config.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include <errno.h>
#define G_LOG_DOMAIN "DiaCairo"
#include <glib.h>
#include <glib/gstdio.h>

#ifdef HAVE_PANGOCAIRO_H
#include <pango/pangocairo.h>
#endif

#ifdef HAVE_CAIRO
#  include <cairo.h>
/* some backend headers, win32 missing in official Cairo */
#  ifdef CAIRO_HAS_PNG_SURFACE_FEATURE
#  include <cairo-png.h>
#  endif
#  ifdef  CAIRO_HAS_PS_SURFACE
#  include <cairo-ps.h>
#  endif
#  ifdef  CAIRO_HAS_PDF_SURFACE
#  include <cairo-pdf.h>
#  endif
#  ifdef CAIRO_HAS_SVG_SURFACE
#  include <cairo-svg.h>
#  endif
#endif

#include "intl.h"
#include "message.h"
#include "geometry.h"
#include "dia_image.h"
#include "diarenderer.h"
#include "filter.h"
#include "plug-ins.h"
#include "object.h" /* only for object->ops->draw */

#include "diacairo.h"

static void ensure_minimum_one_device_unit(DiaCairoRenderer *renderer, real *value);

/* 
 * render functions 
 */ 
static void
begin_render(DiaRenderer *self, const Rectangle *update)
{
  DiaCairoRenderer *renderer = DIA_CAIRO_RENDERER (self);
  real onedu = 0.0;
  real lmargin = 0.0, tmargin = 0.0;
  gboolean paginated = renderer->surface && /* only with our own pagination, not GtkPrint */
    cairo_surface_get_type (renderer->surface) == CAIRO_SURFACE_TYPE_PDF && !renderer->skip_show_page;

  if (renderer->surface && !renderer->cr)
    renderer->cr = cairo_create (renderer->surface);
  else
    g_assert (renderer->cr);

  /* remember current state, so we can start from new with every page */
  cairo_save (renderer->cr);

  if (paginated && renderer->dia) {
    DiagramData *data = renderer->dia;
    /* Dia's paper.width already contains the scale, cairo needs it without 
     * Similar for margins, Dia's without, but cairo wants them?
     */
    real width = (data->paper.lmargin + data->paper.width * data->paper.scaling + data->paper.rmargin)
          * (72.0 / 2.54) + 0.5;
    real height = (data->paper.tmargin + data->paper.height * data->paper.scaling + data->paper.bmargin)
           * (72.0 / 2.54) + 0.5;
    /* "Changes the size of a PDF surface for the current (and
     * subsequent) pages." Pagination setup? */
    cairo_pdf_surface_set_size (renderer->surface, width, height);
    lmargin = data->paper.lmargin / data->paper.scaling;
    tmargin = data->paper.tmargin / data->paper.scaling;
  }

  cairo_scale (renderer->cr, renderer->scale, renderer->scale);
  /* to ensure no clipping at top/left we need some extra gymnastics,
   * otherwise a box with a line witdh one one pixel might loose the
   * top/left border as in bug #147386 */
  ensure_minimum_one_device_unit (renderer, &onedu);

  if (update && paginated) {
    cairo_rectangle (renderer->cr, lmargin, tmargin,
                     update->right - update->left, update->bottom - update->top);
    cairo_clip (renderer->cr);
    cairo_translate (renderer->cr, -update->left + lmargin, -update->top + tmargin);
  } else
    cairo_translate (renderer->cr, -renderer->dia->extents.left + onedu, -renderer->dia->extents.top + onedu);
  /* no more blurred UML diagrams */
  cairo_set_antialias (renderer->cr, CAIRO_ANTIALIAS_NONE);

  /* clear background */
  if (renderer->with_alpha)
    {
      cairo_set_operator (renderer->cr, CAIRO_OPERATOR_SOURCE);
      cairo_set_source_rgba (renderer->cr,
                             renderer->dia->bg_color.red, 
                             renderer->dia->bg_color.green, 
                             renderer->dia->bg_color.blue,
                             0.0);
    }
  else
    {
      cairo_set_source_rgba (renderer->cr,
                             renderer->dia->bg_color.red, 
                             renderer->dia->bg_color.green, 
                             renderer->dia->bg_color.blue,
                             1.0);
    }
  cairo_paint (renderer->cr);
  if (renderer->with_alpha)
    {
      /* restore to default drawing */
      cairo_set_operator (renderer->cr, CAIRO_OPERATOR_OVER);
      cairo_set_source_rgba (renderer->cr,
                             renderer->dia->bg_color.red, 
                             renderer->dia->bg_color.green, 
                             renderer->dia->bg_color.blue,
                             1.0);
    }
#ifdef HAVE_PANGOCAIRO_H
  if (!renderer->layout)
    renderer->layout = pango_cairo_create_layout (renderer->cr);
#endif

  cairo_set_fill_rule (renderer->cr, CAIRO_FILL_RULE_EVEN_ODD);

#if 0 /* try to work around bug #341481 - no luck */
  {
    cairo_font_options_t *fo = cairo_font_options_create ();
    cairo_get_font_options (renderer->cr, fo);
    /* try to switch off kerning */
    cairo_font_options_set_hint_style (fo, CAIRO_HINT_STYLE_NONE);
    cairo_font_options_set_hint_metrics (fo, CAIRO_HINT_METRICS_OFF);

    cairo_set_font_options (renderer->cr, fo);
    cairo_font_options_destroy (fo);
#ifdef HAVE_PANGOCAIRO_H
    pango_cairo_update_context (renderer->cr, pango_layout_get_context (renderer->layout));
    pango_layout_context_changed (renderer->layout);
#endif
  }
#endif
  
  DIAG_STATE(renderer->cr)
}

static void
end_render(DiaRenderer *self)
{
  DiaCairoRenderer *renderer = DIA_CAIRO_RENDERER (self);

  DIAG_NOTE(g_message( "end_render"));
 
  if (!renderer->skip_show_page)
    cairo_show_page (renderer->cr);
  /* pop current state, so we can start from new with every page */
  cairo_restore (renderer->cr);
  DIAG_STATE(renderer->cr)
}

/*!
 * \brief Advertize renderers capabilities
 *
 * Especially with cairo this should be 'all' so this function
 * is complaining if it will return FALSE
 * \memberof _DiaCairoRenderer
 */
static gboolean 
is_capable_to (DiaRenderer *renderer, RenderCapability cap)
{
  static RenderCapability warned = RENDER_HOLES;

  if (RENDER_HOLES == cap)
    return TRUE;
  else if (RENDER_ALPHA == cap)
    return TRUE;
  else if (RENDER_AFFINE == cap)
    return TRUE;
  if (cap != warned)
    g_warning ("New capability not supported by cairo??");
  warned = cap;
  return FALSE;
}

/*!
 * \brief Render the given object optionally transformed by matrix
 * @param self explicit this pointer
 * @param object the _DiaObject to draw
 * @param matrix the trnsformation matrix to use or NULL
 */
static void
draw_object (DiaRenderer *self, DiaObject *object, DiaMatrix *matrix)
{
  DiaCairoRenderer *renderer = DIA_CAIRO_RENDERER (self);
  cairo_matrix_t before;
  
  if (matrix) {
    cairo_get_matrix (renderer->cr, &before);
    g_assert (sizeof(cairo_matrix_t) == sizeof(DiaMatrix));
    cairo_transform (renderer->cr, (cairo_matrix_t *)matrix);
  }
  object->ops->draw(object, DIA_RENDERER (renderer));
  if (matrix)
    cairo_set_matrix (renderer->cr, &before);
}

/*!
 * \brief Ensure a minimum of one device unit
 * Dia as well as many other drawing applications/libraries is using a
 * line with 0f 0.0 tho mean hairline. Cairo doe not have this capability
 * so this functions should be used to get the thinnest line possible.
 * \protected \memberof _DiaCairoRenderer
 */
static void
ensure_minimum_one_device_unit(DiaCairoRenderer *renderer, real *value)
{
  double ax = 1., ay = 1.;

  cairo_device_to_user_distance (renderer->cr, &ax, &ay);

  ax = MAX(ax, ay);
  if (*value < ax)
      *value = ax;
}

static void
set_linewidth(DiaRenderer *self, real linewidth)
{  
  DiaCairoRenderer *renderer = DIA_CAIRO_RENDERER (self);

  DIAG_NOTE(g_message("set_linewidth %f", linewidth));

  /* make hairline? Everythnig below one device unit get the same width,
   * otherwise 0.0 may end up thicker than 0.0+epsilon
   */
  ensure_minimum_one_device_unit(renderer, &linewidth);

  cairo_set_line_width (renderer->cr, linewidth);
  DIAG_STATE(renderer->cr)
}

static void
set_linecaps(DiaRenderer *self, LineCaps mode)
{
  DiaCairoRenderer *renderer = DIA_CAIRO_RENDERER (self);

  DIAG_NOTE(g_message("set_linecaps %d", mode));

  switch(mode) {
  case LINECAPS_BUTT:
    cairo_set_line_cap (renderer->cr, CAIRO_LINE_CAP_BUTT);
    break;
  case LINECAPS_ROUND:
    cairo_set_line_cap (renderer->cr, CAIRO_LINE_CAP_ROUND);
    break;
  case LINECAPS_PROJECTING:
    cairo_set_line_cap (renderer->cr, CAIRO_LINE_CAP_SQUARE); /* ?? */
    break;
  default:
    g_warning("DiaCairoRenderer : Unsupported caps mode specified!\n");
  }
  DIAG_STATE(renderer->cr)
}

static void
set_linejoin(DiaRenderer *self, LineJoin mode)
{
  DiaCairoRenderer *renderer = DIA_CAIRO_RENDERER (self);

  DIAG_NOTE(g_message("set_join %d", mode));

  switch(mode) {
  case LINEJOIN_MITER:
    cairo_set_line_join (renderer->cr, CAIRO_LINE_JOIN_MITER);
    break;
  case LINEJOIN_ROUND:
    cairo_set_line_join (renderer->cr, CAIRO_LINE_JOIN_ROUND);
    break;
  case LINEJOIN_BEVEL:
    cairo_set_line_join (renderer->cr, CAIRO_LINE_JOIN_BEVEL);
    break;
  default:
    g_warning("DiaCairoRenderer : Unsupported join mode specified!\n");
  }
  DIAG_STATE(renderer->cr)
}

static void
set_linestyle(DiaRenderer *self, LineStyle mode)
{
  /* dot = 10% of len */
  DiaCairoRenderer *renderer = DIA_CAIRO_RENDERER (self);
  double dash[6];

  DIAG_NOTE(g_message("set_linestyle %d", mode));

  /* also stored for use in set_dashlength */
  renderer->line_style = mode;
  /* line type */
  switch (mode) {
  case LINESTYLE_SOLID:
    cairo_set_dash (renderer->cr, NULL, 0, 0);
    break;
  case LINESTYLE_DASHED:
    dash[0] = renderer->dash_length;
    dash[1] = renderer->dash_length;
    cairo_set_dash (renderer->cr, dash, 2, 0);
    break;
  case LINESTYLE_DASH_DOT:
    dash[0] = renderer->dash_length;
    dash[1] = renderer->dash_length * 0.45;
    dash[2] = renderer->dash_length * 0.1;
    dash[3] = renderer->dash_length * 0.45;
    cairo_set_dash (renderer->cr, dash, 4, 0);
    break;
  case LINESTYLE_DASH_DOT_DOT:
    dash[0] = renderer->dash_length;
    dash[1] = renderer->dash_length * (0.8/3);
    dash[2] = renderer->dash_length * 0.1;
    dash[3] = renderer->dash_length * (0.8/3);
    dash[4] = renderer->dash_length * 0.1;
    dash[5] = renderer->dash_length * (0.8/3);
    cairo_set_dash (renderer->cr, dash, 6, 0);
    break;
  case LINESTYLE_DOTTED:
    dash[0] = renderer->dash_length * 0.1;
    dash[1] = renderer->dash_length * 0.1;
    cairo_set_dash (renderer->cr, dash, 2, 0);
    break;
  default:
    g_warning("DiaCairoRenderer : Unsupported line style specified!\n");
  }
  DIAG_STATE(renderer->cr)
}

static void
set_dashlength(DiaRenderer *self, real length)
{  
  DiaCairoRenderer *renderer = DIA_CAIRO_RENDERER (self);

  DIAG_NOTE(g_message("set_dashlength %f", length));

  /* this call does not make sense, the value is certainly bigger
   * than one device unit. But the side-effect seems to end the endless loop */
  ensure_minimum_one_device_unit(renderer, &length);
  renderer->dash_length = length;
  /* updating the line style (potentially once more) make the real
   * style and it's length indepndent of the calling sequence */
  set_linestyle(self, renderer->line_style);
}

/*!
 * \brief Set the fill style
 * The fill style is one of the areas, where the cairo library offers a lot
 * more the Dia currently uses. Cairo can render repeating patterns as well
 * as linear and radial gradients. As of this writing Dia just uses solid
 * color fill.
 * \memberof _DiaCairoRenderer
 */
static void
set_fillstyle(DiaRenderer *self, FillStyle mode)
{
  DiaCairoRenderer *renderer = DIA_CAIRO_RENDERER (self);
  DIAG_NOTE(g_message("set_fillstyle %d", mode));

  switch(mode) {
  case FILLSTYLE_SOLID:
    /* FIXME: how to set _no_ pattern ?
      * cairo_set_pattern (renderer->cr, NULL);
      */
    break;
  default:
    g_warning("DiaCairoRenderer : Unsupported fill mode specified!\n");
  }
  DIAG_STATE(DIA_CAIRO_RENDERER (self)->cr)
}

static void
set_font(DiaRenderer *self, DiaFont *font, real height)
{
  DiaCairoRenderer *renderer = DIA_CAIRO_RENDERER (self);
  /* pango/cairo wants the font size, not the (line-) height */
  real size = dia_font_get_size (font) * (height / dia_font_get_height (font));

  PangoFontDescription *pfd = pango_font_description_copy (dia_font_get_description (font));
  DIAG_NOTE(g_message("set_font %f %s", height, dia_font_get_family(font)));

#ifdef HAVE_PANGOCAIRO_H
  /* select font and size */
  pango_font_description_set_absolute_size (pfd, (int)(size * PANGO_SCALE));
  pango_layout_set_font_description (renderer->layout, pfd);
  pango_font_description_free (pfd);
#else
  if (renderer->cr) {
    DiaFontStyle style = dia_font_get_style (font);
    const char *family_name = dia_font_get_family(font);

    cairo_select_font_face (
        renderer->cr,
        family_name,
        DIA_FONT_STYLE_GET_SLANT(style) == DIA_FONT_NORMAL ? CAIRO_FONT_SLANT_NORMAL : CAIRO_FONT_SLANT_ITALIC,
        DIA_FONT_STYLE_GET_WEIGHT(style) < DIA_FONT_MEDIUM ? CAIRO_FONT_WEIGHT_NORMAL : CAIRO_FONT_WEIGHT_BOLD); 
    cairo_set_font_size (renderer->cr, size);

    DIAG_STATE(renderer->cr)
  }
#endif

  /* for the interactive case we must maintain the font field in the base class */
  if (self->is_interactive) {
    dia_font_ref(font);
    if (self->font)
      dia_font_unref(self->font);
    self->font = font;
    self->font_height = height;
  }
}

static void
draw_line(DiaRenderer *self, 
          Point *start, Point *end, 
          Color *color)
{
  DiaCairoRenderer *renderer = DIA_CAIRO_RENDERER (self);

  DIAG_NOTE(g_message("draw_line %f,%f -> %f, %f", 
            start->x, start->y, end->x, end->y));

  cairo_set_source_rgba (renderer->cr, color->red, color->green, color->blue, color->alpha);
  cairo_move_to (renderer->cr, start->x, start->y);
  cairo_line_to (renderer->cr, end->x, end->y);
  if (!renderer->stroke_pending)
    cairo_stroke (renderer->cr);
  DIAG_STATE(renderer->cr)
}

static void
draw_polyline(DiaRenderer *self, 
              Point *points, int num_points, 
              Color *color)
{
  DiaCairoRenderer *renderer = DIA_CAIRO_RENDERER (self);
  int i;

  DIAG_NOTE(g_message("draw_polyline n:%d %f,%f ...", 
            num_points, points->x, points->y));

  g_return_if_fail(1 < num_points);

  cairo_set_source_rgba (renderer->cr, color->red, color->green, color->blue, color->alpha);

  cairo_new_path (renderer->cr);
  /* point data */
  cairo_move_to (renderer->cr, points[0].x, points[0].y);
  for (i = 1; i < num_points; i++)
    {
      cairo_line_to (renderer->cr, points[i].x, points[i].y);
    }
  cairo_stroke (renderer->cr);
  DIAG_STATE(renderer->cr)
}

static void
_polygon(DiaRenderer *self, 
         Point *points, int num_points, 
         Color *color,
         gboolean fill)
{
  DiaCairoRenderer *renderer = DIA_CAIRO_RENDERER (self);
  int i;

  DIAG_NOTE(g_message("%s_polygon n:%d %f,%f ...",
            fill ? "fill" : "draw",
            num_points, points->x, points->y));

  g_return_if_fail(1 < num_points);

  cairo_set_source_rgba (renderer->cr, color->red, color->green, color->blue, color->alpha);

  cairo_new_path (renderer->cr);
  /* point data */
  cairo_move_to (renderer->cr, points[0].x, points[0].y);
  for (i = 1; i < num_points; i++)
    {
      cairo_line_to (renderer->cr, points[i].x, points[i].y);
    }
  cairo_line_to (renderer->cr, points[0].x, points[0].y);
  cairo_close_path (renderer->cr);
  if (fill)
    cairo_fill (renderer->cr);
  else
    cairo_stroke (renderer->cr);
  DIAG_STATE(renderer->cr)
}

static void
draw_polygon(DiaRenderer *self, 
             Point *points, int num_points, 
             Color *color)
{
  _polygon (self, points, num_points, color, FALSE);
}

static void
fill_polygon(DiaRenderer *self, 
             Point *points, int num_points, 
             Color *color)
{
  _polygon (self, points, num_points, color, TRUE);
}

static void
_rect(DiaRenderer *self, 
      Point *ul_corner, Point *lr_corner,
      Color *color,
      gboolean fill)
{
  DiaCairoRenderer *renderer = DIA_CAIRO_RENDERER (self);

  DIAG_NOTE(g_message("%s_rect %f,%f -> %f,%f", 
            fill ? "fill" : "draw",
            ul_corner->x, ul_corner->y, lr_corner->x, lr_corner->y));

  cairo_set_source_rgba (renderer->cr, color->red, color->green, color->blue, color->alpha);
  
  cairo_rectangle (renderer->cr, 
                   ul_corner->x, ul_corner->y, 
                   lr_corner->x - ul_corner->x, lr_corner->y - ul_corner->y);

  if (fill)
    cairo_fill (renderer->cr);
  else
    cairo_stroke (renderer->cr);
  DIAG_STATE(renderer->cr)
}

static void
draw_rect(DiaRenderer *self, 
          Point *ul_corner, Point *lr_corner,
          Color *color)
{
  _rect (self, ul_corner, lr_corner, color, FALSE);
}

static void
fill_rect(DiaRenderer *self, 
          Point *ul_corner, Point *lr_corner,
          Color *color)
{
  _rect (self, ul_corner, lr_corner, color, TRUE);
}

static void
draw_arc(DiaRenderer *self, 
	 Point *center,
	 real width, real height,
	 real angle1, real angle2,
	 Color *color)
{
  DiaCairoRenderer *renderer = DIA_CAIRO_RENDERER (self);
  Point start;
  double a1, a2;
  real onedu = 0.0;

  DIAG_NOTE(g_message("draw_arc %fx%f <%f,<%f", 
            width, height, angle1, angle2));

  cairo_set_source_rgba (renderer->cr, color->red, color->green, color->blue, color->alpha);

  if (!renderer->stroke_pending)
    cairo_new_path (renderer->cr);
  /* Dia and Cairo don't agree on arc definitions, so it needs
   * to be converted, i.e. mirrored at the x axis
   */
  start.x = center->x + (width / 2.0)  * cos((M_PI / 180.0) * angle1);
  start.y = center->y - (height / 2.0) * sin((M_PI / 180.0) * angle1);
  cairo_move_to (renderer->cr, start.x, start.y);
  a1 = - (angle1 / 180.0) * G_PI;
  a2 = - (angle2 / 180.0) * G_PI;
  /* FIXME: to handle width != height some cairo_scale/cairo_translate would be needed */
  ensure_minimum_one_device_unit (renderer, &onedu);
  /* FIXME2: with too small arcs cairo goes into an endless loop */
  if (height/2.0 > onedu && width/2.0 > onedu)
    cairo_arc_negative (renderer->cr, center->x, center->y, 
                        width > height ? height / 2.0 : width / 2.0, /* FIXME 2nd radius */
                        a1, a2);
  if (!renderer->stroke_pending)
    cairo_stroke (renderer->cr);
  DIAG_STATE(renderer->cr)
}

static void
fill_arc(DiaRenderer *self, 
         Point *center,
         real width, real height,
         real angle1, real angle2,
         Color *color)
{
  DiaCairoRenderer *renderer = DIA_CAIRO_RENDERER (self);
  Point start;
  double a1, a2;

  DIAG_NOTE(g_message("draw_arc %fx%f <%f,<%f", 
            width, height, angle1, angle2));

  cairo_set_source_rgba (renderer->cr, color->red, color->green, color->blue, color->alpha);
  
  cairo_new_path (renderer->cr);
  start.x = center->x + (width / 2.0)  * cos((M_PI / 180.0) * angle1);
  start.y = center->y - (height / 2.0) * sin((M_PI / 180.0) * angle1);
  cairo_move_to (renderer->cr, center->x, center->y);
  cairo_line_to (renderer->cr, start.x, start.y);
  a1 = - (angle1 / 180.0) * G_PI;
  a2 = - (angle2 / 180.0) * G_PI;
  /* FIXME: to handle width != height some cairo_scale/cairo_translate would be needed */
  cairo_arc_negative (renderer->cr, center->x, center->y, 
                      width > height ? height / 2.0 : width / 2.0, /* FIXME 2nd radius */
                      a1, a2);
  cairo_line_to (renderer->cr, center->x, center->y);
  cairo_close_path (renderer->cr);
  cairo_fill (renderer->cr);
  DIAG_STATE(renderer->cr)
}

static void
_ellipse(DiaRenderer *self, 
         Point *center,
         real width, real height,
         Color *color,
         gboolean fill)
{
  DiaCairoRenderer *renderer = DIA_CAIRO_RENDERER (self);

  DIAG_NOTE(g_message("%s_ellipse %fx%f center @ %f,%f", 
            fill ? "fill" : "draw", width, height, center->x, center->y));

  /* avoid screwing cairo context - I'd say restore should fix it again, but it doesn't
   * (dia.exe:3152): DiaCairo-WARNING **: diacairo-renderer.c:254, invalid matrix (not invertible)
   */
  if (!(width > 0. && height > 0.))
    return;

  cairo_set_source_rgba (renderer->cr, color->red, color->green, color->blue, color->alpha);

  cairo_save (renderer->cr);
  /* don't create a line from the current point to the beginning 
   * of the ellipse */
  cairo_new_sub_path (renderer->cr);
  /* copied straight from cairo's documentation, and fixed the bug there */
  cairo_translate (renderer->cr, center->x, center->y);
  cairo_scale (renderer->cr, width / 2., height / 2.);
  cairo_arc (renderer->cr, 0., 0., 1., 0., 2 * G_PI);
  cairo_restore (renderer->cr);

  if (fill)
    cairo_fill (renderer->cr);
  else
    cairo_stroke (renderer->cr);
  DIAG_STATE(renderer->cr)
}

static void
draw_ellipse(DiaRenderer *self, 
             Point *center,
             real width, real height,
             Color *color)
{
  _ellipse (self, center, width, height, color, FALSE);
}

static void
fill_ellipse(DiaRenderer *self, 
             Point *center,
             real width, real height,
             Color *color)
{
  _ellipse (self, center, width, height, color, TRUE);
}

static void
_bezier(DiaRenderer *self, 
        BezPoint *points,
        int numpoints,
        Color *color,
        gboolean fill)
{
  DiaCairoRenderer *renderer = DIA_CAIRO_RENDERER (self);
  int i;

  DIAG_NOTE(g_message("%s_bezier n:%d %fx%f ...", 
            fill ? "fill" : "draw", numpoints, points->p1.x, points->p1.y));

  cairo_set_source_rgba (renderer->cr, color->red, color->green, color->blue, color->alpha);

  cairo_new_path (renderer->cr);
  for (i = 0; i < numpoints; i++)
  {
    switch (points[i].type)
    {
    case BEZ_MOVE_TO:
      cairo_move_to (renderer->cr, points[i].p1.x, points[i].p1.y);
      break;
    case BEZ_LINE_TO:
      cairo_line_to (renderer->cr, points[i].p1.x, points[i].p1.y);
      break;
    case BEZ_CURVE_TO:
      cairo_curve_to (renderer->cr, 
                      points[i].p1.x, points[i].p1.y,
                      points[i].p2.x, points[i].p2.y,
                      points[i].p3.x, points[i].p3.y);
      break;
    default :
      g_assert_not_reached ();
    }
  }

  /* The stroke would benefit by an explicit cairo_close_path() but we can not do it.
   * At this point there is not enough information left, i.e. if it comes from a Beziergon
   * and should be closed or if it was a Bezierline which happens to end in the start point.
   */
  if (fill)
    cairo_fill (renderer->cr);
  else
    cairo_stroke (renderer->cr);
  DIAG_STATE(renderer->cr)
}

static void
draw_bezier(DiaRenderer *self, 
            BezPoint *points,
            int numpoints,
            Color *color)
{
  _bezier (self, points, numpoints, color, FALSE);
}

static void
fill_bezier(DiaRenderer *self, 
            BezPoint *points,
            int numpoints,
            Color *color)
{
  _bezier (self, points, numpoints, color, TRUE);
}

static void
draw_string(DiaRenderer *self,
            const char *text,
            Point *pos, Alignment alignment,
            Color *color)
{
  DiaCairoRenderer *renderer = DIA_CAIRO_RENDERER (self);
  int len = strlen(text);

  DIAG_NOTE(g_message("draw_string(%d) %f,%f %s", 
            len, pos->x, pos->y, text));

  if (len < 1) return; /* shouldn't this be handled by Dia's core ? */

  cairo_set_source_rgba (renderer->cr, color->red, color->green, color->blue, color->alpha);
#ifdef HAVE_PANGOCAIRO_H
  cairo_save (renderer->cr);
  /* alignment calculation done by pangocairo? */
  pango_layout_set_alignment (renderer->layout, alignment == ALIGN_CENTER ? PANGO_ALIGN_CENTER :
                                                alignment == ALIGN_RIGHT ? PANGO_ALIGN_RIGHT : PANGO_ALIGN_LEFT);
  pango_layout_set_text (renderer->layout, text, len);
  {
    PangoLayoutIter *iter = pango_layout_get_iter(renderer->layout);
    int bline = pango_layout_iter_get_baseline(iter);
    /* although we give the alignment above we need to adjust the start point */
    PangoRectangle extents;
    int shift;
    pango_layout_iter_get_line_extents (iter, NULL, &extents);
    shift = alignment == ALIGN_CENTER ? PANGO_RBEARING(extents)/2 :
            alignment == ALIGN_RIGHT ? PANGO_RBEARING(extents) : 0;
    cairo_move_to (renderer->cr, pos->x - (double)shift / PANGO_SCALE, pos->y - (double)bline / PANGO_SCALE);
    pango_layout_iter_free (iter);
  }
  /* does this hide bug #341481? */
  pango_cairo_update_context (renderer->cr, pango_layout_get_context (renderer->layout));
  pango_layout_context_changed (renderer->layout);

  pango_cairo_show_layout (renderer->cr, renderer->layout);
  cairo_restore (renderer->cr);
#else
  /* using the 'toy API' */
  {
    cairo_text_extents_t extents;
    double x = 0, y = 0;
    cairo_set_source_rgba (renderer->cr, color->red, color->green, color->blue, color->alpha);
    cairo_text_extents (renderer->cr,
                        text,
                        &extents);

    y = pos->y; /* ?? */

    switch (alignment) {
    case ALIGN_LEFT:
      x = pos->x;
      break;
    case ALIGN_CENTER:
      x = pos->x - extents.width / 2 + +extents.x_bearing;
      break;
    case ALIGN_RIGHT:
      x = pos->x - extents.width + extents.x_bearing;
      break;
    }
    cairo_move_to (renderer->cr, x, y);
    cairo_show_text (renderer->cr, text);
  }
#endif

  DIAG_STATE(renderer->cr)
}

static void
draw_image(DiaRenderer *self,
           Point *point,
           real width, real height,
           DiaImage *image)
{
  DiaCairoRenderer *renderer = DIA_CAIRO_RENDERER (self);
  cairo_surface_t *surface;
  guint8 *data;
  int w = dia_image_width(image);
  int h = dia_image_height(image);
  int rs = dia_image_rowstride(image);

  DIAG_NOTE(g_message("draw_image %fx%f [%d(%d),%d] @%f,%f", 
            width, height, w, rs, h, point->x, point->y));

  if (dia_image_rgba_data (image))
    {
      const guint8 *p1 = dia_image_rgba_data (image);
      /* we need to make a copy to rearrange channels ... */
      guint8 *p2 = data = g_try_malloc (h * rs);
      int i;

      if (!data) 
        {
          message_warning (_("Not enough memory for image drawing."));
	  return;
        }

      for (i = 0; i < (h * rs) / 4; i++)
        {
#  if G_BYTE_ORDER == G_LITTLE_ENDIAN
          p2[0] = p1[2]; /* b */
          p2[1] = p1[1]; /* g */
          p2[2] = p1[0]; /* r */
          p2[3] = p1[3]; /* a */
#  else
          p2[3] = p1[2]; /* b */
          p2[2] = p1[1]; /* g */
          p2[1] = p1[0]; /* r */
          p2[0] = p1[3]; /* a */
#  endif
          p1+=4;
          p2+=4;
        }

      surface = cairo_image_surface_create_for_data (data, CAIRO_FORMAT_ARGB32, w, h, rs);
    }
  else
    {
      guint8 *p = NULL, *p2;
      guint8 *p1 = data = dia_image_rgb_data (image);
      /* cairo wants RGB24 32 bit aligned, so copy ... */
      int x, y;

      if (data)
	p = p2 = g_try_malloc(h*w*4);
	
      if (!p)
        {
          message_warning (_("Not enough memory for image drawing."));
	  return;
        }

      for (y = 0; y < h; y++)
        {
          for (x = 0; x < w; x++)
            {
#if G_BYTE_ORDER == G_LITTLE_ENDIAN
              /* apparently BGR is required */
              p2[x*4  ] = p1[x*3+2];
              p2[x*4+1] = p1[x*3+1];
              p2[x*4+2] = p1[x*3  ];
              p2[x*4+3] = 0x80; /* should not matter */
#else
              p2[x*4+3] = p1[x*3+2];
              p2[x*4+2] = p1[x*3+1];
              p2[x*4+1] = p1[x*3  ];
              p2[x*4+0] = 0x80; /* should not matter */
#endif
            }
          p2 += (w*4);
          p1 += rs;
        }
      surface = cairo_image_surface_create_for_data (p, CAIRO_FORMAT_RGB24, w, h, w*4);
      g_free (data);
      data = p;
    }
  cairo_save (renderer->cr);
  cairo_translate (renderer->cr, point->x, point->y);
  cairo_scale (renderer->cr, width/w, height/h);
  cairo_move_to (renderer->cr, 0.0, 0.0);
  /* maybe just the second set_filter is required */
#if 0
  cairo_surface_set_filter (renderer->surface, CAIRO_FILTER_BEST);
  cairo_surface_set_filter (surface, CAIRO_FILTER_BEST);
#endif
  cairo_set_source_surface (renderer->cr, surface, 0.0, 0.0);
  cairo_paint (renderer->cr);
  cairo_restore (renderer->cr);
  cairo_surface_destroy (surface);

  g_free (data);

  DIAG_STATE(renderer->cr);
}

static void 
_rounded_rect (DiaRenderer *self,
               Point *topleft, Point *bottomright,
               Color *color, real radius,
               gboolean fill)
{
  DiaCairoRenderer *renderer = DIA_CAIRO_RENDERER (self);
  double rv[2];

  radius = MIN(radius, (bottomright->x - topleft->x)/2);
  radius = MIN(radius, (bottomright->y - topleft->y)/2);
  
  /* ignore radius if it is smaller than the device unit, avoids anti-aliasing artifacts */
  rv[0] = radius;
  rv[1] = 0.0;
  cairo_user_to_device_distance (renderer->cr, &rv[0], &rv[1]);
  if (rv[0] < 1.0 && rv[1] < 1.0) {
    _rect (self, topleft, bottomright, color, fill);
    return;  
  }

  DIAG_NOTE(g_message("%s_rounded_rect %f,%f -> %f,%f, %f", 
            fill ? "fill" : "draw",
            topleft->x, topleft->y, bottomright->x, bottomright->y, radius));

  cairo_set_source_rgba (renderer->cr, color->red, color->green, color->blue, color->alpha);

  cairo_new_path (renderer->cr);
  cairo_move_to (renderer->cr, /* north-west */
                 topleft->x + radius, topleft->y);

  cairo_line_to  (renderer->cr, /* north-east */
                  bottomright->x - radius, topleft->y);
  cairo_arc (renderer->cr,
             bottomright->x - radius, topleft->y + radius, radius, -G_PI_2, 0);
  cairo_line_to  (renderer->cr, /* south-east */
                  bottomright->x, bottomright->y - radius);
  cairo_arc (renderer->cr,
             bottomright->x - radius, bottomright->y - radius, radius, 0, G_PI_2);
  cairo_line_to  (renderer->cr, /* south-west */
                  topleft->x + radius, bottomright->y);
  cairo_arc (renderer->cr,
             topleft->x + radius, bottomright->y - radius, radius, G_PI_2, G_PI);
  cairo_line_to  (renderer->cr, /* north-west */
                  topleft->x, topleft->y + radius); 
  cairo_arc (renderer->cr,
             topleft->x + radius, topleft->y + radius, radius, G_PI, -G_PI_2);
  if (fill)
    cairo_fill (renderer->cr);
  else
    cairo_stroke (renderer->cr);
  DIAG_STATE(renderer->cr)
}

static void 
draw_rounded_rect (DiaRenderer *renderer,
                   Point *topleft, Point *bottomright,
                   Color *color, real radius)
{
  _rounded_rect (renderer, topleft, bottomright, color, radius, FALSE);
}

static void 
fill_rounded_rect (DiaRenderer *renderer,
                   Point *topleft, Point *bottomright,
                   Color *color, real radius)
{
  _rounded_rect (renderer, topleft, bottomright, color, radius, TRUE);
}

static gpointer parent_class = NULL;

static void
draw_rounded_polyline (DiaRenderer *self,
                       Point *points, int num_points,
                       Color *color, real radius)
{
  DiaCairoRenderer *renderer = DIA_CAIRO_RENDERER (self);

  cairo_new_path (renderer->cr);
  cairo_move_to (renderer->cr, points[0].x, points[0].y);
  /* use base class implementation */
  renderer->stroke_pending = TRUE;
  DIA_RENDERER_CLASS(parent_class)->draw_rounded_polyline (self, 
                                                           points, num_points,
							   color, radius);
  renderer->stroke_pending = FALSE;
  cairo_stroke (renderer->cr);
  DIAG_STATE(renderer->cr)
}

/* gobject boiler plate */
static void cairo_renderer_init (DiaCairoRenderer *r, void *p);
static void cairo_renderer_class_init (DiaCairoRendererClass *klass);

GType
dia_cairo_renderer_get_type (void)
{
  static GType object_type = 0;

  if (!object_type)
    {
      static const GTypeInfo object_info =
      {
        sizeof (DiaCairoRendererClass),
        (GBaseInitFunc) NULL,
        (GBaseFinalizeFunc) NULL,
        (GClassInitFunc) cairo_renderer_class_init,
        NULL,           /* class_finalize */
        NULL,           /* class_data */
        sizeof (DiaCairoRenderer),
        0,              /* n_preallocs */
	(GInstanceInitFunc)cairo_renderer_init /* init */
      };

      object_type = g_type_register_static (DIA_TYPE_RENDERER,
                                            "DiaCairoRenderer",
                                            &object_info, 0);
    }
  
  return object_type;
}

static void
cairo_renderer_init (DiaCairoRenderer *renderer, void *p)
{
  renderer->line_style = LINESTYLE_SOLID;
  /*
   * Initialize fields where 0 init isn't good enough. Bug #151716
   * appears to show that we are sometimes called to render a line
   * without setting the linestyle first. Probably a bug elsewhere
   * but it's this plug-in which hangs ;)
   */
  renderer->dash_length = 1.0;
  renderer->scale = 1.0;
}

static void
cairo_renderer_finalize (GObject *object)
{
  DiaCairoRenderer *renderer = DIA_CAIRO_RENDERER (object);

  cairo_destroy (renderer->cr);
  if (renderer->surface)
    cairo_surface_destroy (renderer->surface);
#ifdef HAVE_PANGOCAIRO_H
  if (renderer->layout)
    g_object_unref (renderer->layout);  
#endif

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
cairo_renderer_class_init (DiaCairoRendererClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  DiaRendererClass *renderer_class = DIA_RENDERER_CLASS (klass);

  parent_class = g_type_class_peek_parent (klass);

  object_class->finalize = cairo_renderer_finalize;

  /* renderer members */
  renderer_class->begin_render = begin_render;
  renderer_class->end_render   = end_render;
  renderer_class->draw_object = draw_object;

  renderer_class->set_linewidth  = set_linewidth;
  renderer_class->set_linecaps   = set_linecaps;
  renderer_class->set_linejoin   = set_linejoin;
  renderer_class->set_linestyle  = set_linestyle;
  renderer_class->set_dashlength = set_dashlength;
  renderer_class->set_fillstyle  = set_fillstyle;

  renderer_class->set_font  = set_font;

  renderer_class->draw_line    = draw_line;
  renderer_class->fill_polygon = fill_polygon;
  renderer_class->draw_rect    = draw_rect;
  renderer_class->fill_rect    = fill_rect;
  renderer_class->draw_arc     = draw_arc;
  renderer_class->fill_arc     = fill_arc;
  renderer_class->draw_ellipse = draw_ellipse;
  renderer_class->fill_ellipse = fill_ellipse;

  renderer_class->draw_string  = draw_string;
  renderer_class->draw_image   = draw_image;

  /* medium level functions */
  renderer_class->draw_rect = draw_rect;
  renderer_class->draw_polyline  = draw_polyline;
  renderer_class->draw_polygon   = draw_polygon;

  renderer_class->draw_bezier   = draw_bezier;
  renderer_class->fill_bezier   = fill_bezier;

  /* highest level functions */
  renderer_class->draw_rounded_rect = draw_rounded_rect;
  renderer_class->fill_rounded_rect = fill_rounded_rect;
  renderer_class->draw_rounded_polyline = draw_rounded_polyline;
  /* other */
  renderer_class->is_capable_to = is_capable_to;
}
