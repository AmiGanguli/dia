/* Dia -- an diagram creation/manipulation program
 * Copyright (C) 1998 Alexander Larsson
 *
 * node type for UML diagrams
 * Copyright (C) 2000 Stefan Seefeld <stefan@berlin-consortium.org>
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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <assert.h>
#include <math.h>
#include <string.h>

#include "intl.h"
#include "object.h"
#include "element.h"
#include "diarenderer.h"
#include "attributes.h"
#include "text.h"
#include "properties.h"

#include "uml.h"

#include "pixmaps/branch.xpm"

typedef struct _Branch Branch;

struct _Branch
{
  Element element;
  ConnectionPoint connections[8];
};

static const double BRANCH_BORDERWIDTH = 0.1;
static const double BRANCH_WIDTH = 2.0;
static const double BRANCH_HEIGHT = 2.0;

static real branch_distance_from(Branch *branch, Point *point);
static void branch_select(Branch *branch, Point *clicked_point, DiaRenderer *interactive_renderer);
static void branch_move_handle(Branch *branch, Handle *handle,
			       Point *to, HandleMoveReason reason, ModifierKeys modifiers);
static void branch_move(Branch *branch, Point *to);
static void branch_draw(Branch *branch, DiaRenderer *renderer);
static Object *branch_create(Point *startpoint,
			     void *user_data,
			     Handle **handle1,
			     Handle **handle2);
static void branch_destroy(Branch *branch);
static Object *branch_load(ObjectNode obj_node, int version,
			   const char *filename);

static PropDescription *branch_describe_props(Branch *branch);
static void branch_get_props(Branch *branch, GPtrArray *props);
static void branch_set_props(Branch *branch, GPtrArray *props);

static void branch_update_data(Branch *branch);

static ObjectTypeOps branch_type_ops =
{
  (CreateFunc) branch_create,
  (LoadFunc)   branch_load,/*using_properties*/     /* load */
  (SaveFunc)   object_save_using_properties,      /* save */
  (GetDefaultsFunc)   NULL, 
  (ApplyDefaultsFunc) NULL
};

ObjectType branch_type =
{
  "UML - Branch",   /* name */
  0,                      /* version */
  (char **) branch_xpm,  /* pixmap */
  
  &branch_type_ops       /* ops */
};

static ObjectOps branch_ops =
{
  (DestroyFunc)         branch_destroy,
  (DrawFunc)            branch_draw,
  (DistanceFunc)        branch_distance_from,
  (SelectFunc)          branch_select,
  (CopyFunc)            object_copy_using_properties,
  (MoveFunc)            branch_move,
  (MoveHandleFunc)      branch_move_handle,
  (GetPropertiesFunc)   object_return_null,
  (ApplyPropertiesFunc) object_return_void,
  (ObjectMenuFunc)      NULL,
  (DescribePropsFunc)   branch_describe_props,
  (GetPropsFunc)        branch_get_props,
  (SetPropsFunc)        branch_set_props
};

static PropDescription branch_props[] = {
  ELEMENT_COMMON_PROPERTIES,
  
  PROP_DESC_END
};

static PropDescription *
branch_describe_props(Branch *branch)
{
  if (branch_props[0].quark == 0) {
    prop_desc_list_calculate_quarks(branch_props);
  }
  return branch_props;
}

static PropOffset branch_offsets[] = {
  ELEMENT_COMMON_PROPERTIES_OFFSETS,
  { NULL, 0, 0 },
};

static void
branch_get_props(Branch * branch, GPtrArray *props)
{
  object_get_props_from_offsets(&branch->element.object, 
                                branch_offsets, props);
}

static void
branch_set_props(Branch *branch, GPtrArray *props)
{
  object_set_props_from_offsets(&branch->element.object, 
                                branch_offsets, props);
  branch_update_data(branch);
}

static real
branch_distance_from(Branch *branch, Point *point)
{
  Object *obj = &branch->element.object;
  return distance_rectangle_point(&obj->bounding_box, point);
}

static void
branch_select(Branch *branch, Point *clicked_point, DiaRenderer *interactive_renderer)
{
  element_update_handles(&branch->element);
}

static void
branch_move_handle(Branch *branch, Handle *handle,
			 Point *to, HandleMoveReason reason, ModifierKeys modifiers)
{
  assert(branch!=NULL);
  assert(handle!=NULL);
  assert(to!=NULL);

  assert(handle->id < 8);
  
  element_move_handle(&branch->element, handle->id, to, reason);
  branch_update_data(branch);
}

static void
branch_move(Branch *branch, Point *to)
{
  branch->element.corner = *to;
  branch_update_data(branch);
}

static void branch_draw(Branch *branch, DiaRenderer *renderer)
{
  DiaRendererClass *renderer_ops = DIA_RENDERER_GET_CLASS (renderer);
  Element *elem;
  real w, h;
  Point points[4];
  
  assert(branch != NULL);
  assert(renderer != NULL);

  elem = &branch->element;
  w = elem->width/2;
  h = elem->height/2;
  points[0].x = elem->corner.x,       points[0].y = elem->corner.y + h;
  points[1].x = elem->corner.x + w,   points[1].y = elem->corner.y;
  points[2].x = elem->corner.x + 2*w, points[2].y = elem->corner.y + h;
  points[3].x = elem->corner.x + w,   points[3].y = elem->corner.y + 2*h;
  
  renderer_ops->set_fillstyle(renderer, FILLSTYLE_SOLID);
  renderer_ops->set_linewidth(renderer, BRANCH_BORDERWIDTH);
  renderer_ops->set_linestyle(renderer, LINESTYLE_SOLID);

  renderer_ops->fill_polygon(renderer, points, 4, &color_white);
  renderer_ops->draw_polygon(renderer, points, 4, &color_black);
}

static void branch_update_data(Branch *branch)
{
  Element *elem = &branch->element;
  Object *obj = &elem->object;
 
  elem->width = BRANCH_WIDTH;
  elem->height = BRANCH_HEIGHT;
  
  /* Update connections: */
  branch->connections[0].pos.x = elem->corner.x;
  branch->connections[0].pos.y = elem->corner.y + elem->height / 2.;
  branch->connections[0].directions = DIR_WEST;
  branch->connections[1].pos.x = elem->corner.x + elem->width / 2.;
  branch->connections[1].pos.y = elem->corner.y;
  branch->connections[1].directions = DIR_NORTH;
  branch->connections[2].pos.x = elem->corner.x + elem->width;
  branch->connections[2].pos.y = elem->corner.y + elem->height / 2.;
  branch->connections[2].directions = DIR_EAST;
  branch->connections[3].pos.x = elem->corner.x + elem->width / 2.;;
  branch->connections[3].pos.y = elem->corner.y + elem->height;
  branch->connections[3].directions = DIR_SOUTH;
  
  element_update_boundingbox(elem);
  obj->position = elem->corner;

  element_update_handles(elem);
}

static Object *branch_create(Point *startpoint, void *user_data, Handle **handle1, Handle **handle2)
{
  Branch *branch;
  Element *elem;
  Object *obj;
  int i;
  
  branch = g_malloc0(sizeof(Branch));
  elem = &branch->element;
  obj = &elem->object;
  
  obj->type = &branch_type;

  obj->ops = &branch_ops;

  elem->corner = *startpoint;
  element_init(elem, 8, 8);

  for (i=0;i<8;i++)
    {
      obj->connections[i] = &branch->connections[i];
      branch->connections[i].object = obj;
      branch->connections[i].connected = NULL;
    }
  elem->extra_spacing.border_trans = BRANCH_BORDERWIDTH / 2.0;
  branch_update_data(branch);

  *handle1 = NULL;
  *handle2 = obj->handles[0];
  return &branch->element.object;
}

static void branch_destroy(Branch *branch)
{
  element_destroy(&branch->element);
}

static Object *branch_load(ObjectNode obj_node, int version, const char *filename)
{
  return object_load_using_properties(&branch_type,
                                      obj_node,version,filename);
}




