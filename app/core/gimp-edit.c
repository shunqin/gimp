/* GIMP - The GNU Image Manipulation Program
 * Copyright (C) 1995 Spencer Kimball and Peter Mattis
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"

#include <stdlib.h>

#include <cairo.h>
#include <gegl.h>
#include <gdk-pixbuf/gdk-pixbuf.h>

#include "libgimpbase/gimpbase.h"
#include "libgimpcolor/gimpcolor.h"

#include "core-types.h"

#include "gegl/gimp-gegl-utils.h"

#include "gimp.h"
#include "gimp-edit.h"
#include "gimp-utils.h"
#include "gimpbuffer.h"
#include "gimpchannel.h"
#include "gimpcontext.h"
#include "gimpfilloptions.h"
#include "gimpdrawableundo.h"
#include "gimpimage.h"
#include "gimpimage-duplicate.h"
#include "gimpimage-new.h"
#include "gimpimage-undo.h"
#include "gimplayer.h"
#include "gimplayer-floating-selection.h"
#include "gimplayer-new.h"
#include "gimplist.h"
#include "gimppickable.h"
#include "gimpselection.h"
#include "gimptempbuf.h"

#include "gimp-intl.h"


/*  local function protypes  */

static GimpBuffer * gimp_edit_extract (GimpImage     *image,
                                       GimpPickable  *pickable,
                                       GimpContext   *context,
                                       gboolean       cut_pixels,
                                       GError       **error);


/*  public functions  */

GimpObject *
gimp_edit_cut (GimpImage     *image,
               GimpDrawable  *drawable,
               GimpContext   *context,
               GError       **error)
{
  g_return_val_if_fail (GIMP_IS_IMAGE (image), NULL);
  g_return_val_if_fail (GIMP_IS_DRAWABLE (drawable), NULL);
  g_return_val_if_fail (gimp_item_is_attached (GIMP_ITEM (drawable)), NULL);
  g_return_val_if_fail (GIMP_IS_CONTEXT (context), NULL);
  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  if (GIMP_IS_LAYER (drawable) &&
      gimp_channel_is_empty (gimp_image_get_mask (image)))
    {
      GimpImage *clip_image;

      clip_image = gimp_image_new_from_drawable (image->gimp, drawable);
      gimp_container_remove (image->gimp->images, GIMP_OBJECT (clip_image));
      gimp_set_clipboard_image (image->gimp, clip_image);
      g_object_unref (clip_image);

      gimp_image_undo_group_start (image, GIMP_UNDO_GROUP_EDIT_CUT,
                                   C_("undo-type", "Cut Layer"));

      gimp_image_remove_layer (image, GIMP_LAYER (drawable),
                               TRUE, NULL);

      gimp_image_undo_group_end (image);

      return GIMP_OBJECT (gimp_get_clipboard_image (image->gimp));
    }
  else
    {
      GimpBuffer *buffer;

      buffer = gimp_edit_extract (image, GIMP_PICKABLE (drawable),
                                  context, TRUE, error);

      if (buffer)
        {
          gimp_set_clipboard_buffer (image->gimp, buffer);
          g_object_unref (buffer);

          return GIMP_OBJECT (gimp_get_clipboard_buffer (image->gimp));
        }
    }

  return NULL;
}

GimpObject *
gimp_edit_copy (GimpImage     *image,
                GimpDrawable  *drawable,
                GimpContext   *context,
                GError       **error)
{
  g_return_val_if_fail (GIMP_IS_IMAGE (image), NULL);
  g_return_val_if_fail (GIMP_IS_DRAWABLE (drawable), NULL);
  g_return_val_if_fail (gimp_item_is_attached (GIMP_ITEM (drawable)), NULL);
  g_return_val_if_fail (GIMP_IS_CONTEXT (context), NULL);
  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  if (GIMP_IS_LAYER (drawable) &&
      gimp_channel_is_empty (gimp_image_get_mask (image)))
    {
      GimpImage *clip_image;

      clip_image = gimp_image_new_from_drawable (image->gimp, drawable);
      gimp_container_remove (image->gimp->images, GIMP_OBJECT (clip_image));
      gimp_set_clipboard_image (image->gimp, clip_image);
      g_object_unref (clip_image);

      return GIMP_OBJECT (gimp_get_clipboard_image (image->gimp));
    }
  else
    {
      GimpBuffer *buffer;

      buffer = gimp_edit_extract (image, GIMP_PICKABLE (drawable),
                                  context, FALSE, error);

      if (buffer)
        {
          gimp_set_clipboard_buffer (image->gimp, buffer);
          g_object_unref (buffer);

          return GIMP_OBJECT (gimp_get_clipboard_buffer (image->gimp));
        }
    }

  return NULL;
}

GimpBuffer *
gimp_edit_copy_visible (GimpImage    *image,
                        GimpContext  *context,
                        GError      **error)
{
  GimpBuffer *buffer;

  g_return_val_if_fail (GIMP_IS_IMAGE (image), NULL);
  g_return_val_if_fail (GIMP_IS_CONTEXT (context), NULL);
  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  buffer = gimp_edit_extract (image, GIMP_PICKABLE (image),
                              context, FALSE, error);

  if (buffer)
    {
      gimp_set_clipboard_buffer (image->gimp, buffer);
      g_object_unref (buffer);

      return gimp_get_clipboard_buffer (image->gimp);
    }

  return NULL;
}

void
gimp_edit_get_paste_offset (GimpImage    *image,
                            GimpDrawable *drawable,
                            GimpObject   *paste,
                            gint          viewport_x,
                            gint          viewport_y,
                            gint          viewport_width,
                            gint          viewport_height,
                            gint         *offset_x,
                            gint         *offset_y)
{
  gint     image_width;
  gint     image_height;
  gint     width;
  gint     height;
  gboolean clamp_to_image = TRUE;

  g_return_if_fail (GIMP_IS_IMAGE (image));
  g_return_if_fail (drawable == NULL || GIMP_IS_DRAWABLE (drawable));
  g_return_if_fail (drawable == NULL ||
                    gimp_item_is_attached (GIMP_ITEM (drawable)));
  g_return_if_fail (GIMP_IS_VIEWABLE (paste));
  g_return_if_fail (offset_x != NULL);
  g_return_if_fail (offset_y != NULL);

  image_width  = gimp_image_get_width  (image);
  image_height = gimp_image_get_height (image);

  gimp_viewable_get_size (GIMP_VIEWABLE (paste), &width, &height);

  if (viewport_width  == image_width &&
      viewport_height == image_height)
    {
      /* if the whole image is visible, act as if there was no viewport */

      viewport_x      = 0;
      viewport_y      = 0;
      viewport_width  = 0;
      viewport_height = 0;
    }

  if (drawable)
    {
      /*  if pasting to a drawable  */

      GimpContainer *children;
      gint           off_x, off_y;
      gint           target_x, target_y;
      gint           target_width, target_height;
      gint           paste_x, paste_y;
      gint           paste_width, paste_height;
      gboolean       have_mask;

      have_mask = ! gimp_channel_is_empty (gimp_image_get_mask (image));

      gimp_item_get_offset (GIMP_ITEM (drawable), &off_x, &off_y);

      children = gimp_viewable_get_children (GIMP_VIEWABLE (drawable));

      if (children && gimp_container_get_n_children (children) == 0)
        {
          /* treat empty layer groups as image-sized, use the selection
           * as target
           */
          gimp_item_bounds (GIMP_ITEM (gimp_image_get_mask (image)),
                            &target_x, &target_y,
                            &target_width, &target_height);
        }
      else
        {
          gimp_item_mask_intersect (GIMP_ITEM (drawable),
                                    &target_x, &target_y,
                                    &target_width, &target_height);
        }

      if (! have_mask         &&    /* if we have no mask */
          viewport_width  > 0 &&    /* and we have a viewport */
          viewport_height > 0 &&
          (width  < target_width || /* and the paste is smaller than the target */
           height < target_height) &&

          /* and the viewport intersects with the target */
          gimp_rectangle_intersect (viewport_x, viewport_y,
                                    viewport_width, viewport_height,
                                    off_x, off_y, /* target_x,y are 0 */
                                    target_width, target_height,
                                    &paste_x, &paste_y,
                                    &paste_width, &paste_height))
        {
          /*  center on the viewport  */

          *offset_x = paste_x + (paste_width - width)  / 2;
          *offset_y = paste_y + (paste_height- height) / 2;
        }
      else
        {
          /*  otherwise center on the target  */

          *offset_x = off_x + target_x + (target_width  - width)  / 2;
          *offset_y = off_y + target_y + (target_height - height) / 2;

          /*  and keep it that way  */
          clamp_to_image = FALSE;
        }
    }
  else if (viewport_width  > 0 &&  /* if we have a viewport */
           viewport_height > 0 &&
           (width  < image_width || /* and the paste is       */
            height < image_height)) /* smaller than the image */
    {
      /*  center on the viewport  */

      *offset_x = viewport_x + (viewport_width  - width)  / 2;
      *offset_y = viewport_y + (viewport_height - height) / 2;
    }
  else
    {
      /*  otherwise center on the image  */

      *offset_x = (image_width  - width)  / 2;
      *offset_y = (image_height - height) / 2;

      /*  and keep it that way  */
      clamp_to_image = FALSE;
    }

  if (clamp_to_image)
    {
      /*  Ensure that the pasted layer is always within the image, if it
       *  fits and aligned at top left if it doesn't. (See bug #142944).
       */
      *offset_x = MIN (*offset_x, image_width  - width);
      *offset_y = MIN (*offset_y, image_height - height);
      *offset_x = MAX (*offset_x, 0);
      *offset_y = MAX (*offset_y, 0);
    }
}

GimpLayer *
gimp_edit_paste (GimpImage     *image,
                 GimpDrawable  *drawable,
                 GimpObject    *paste,
                 GimpPasteType  paste_type,
                 gint           viewport_x,
                 gint           viewport_y,
                 gint           viewport_width,
                 gint           viewport_height)
{
  GimpLayer  *layer = NULL;
  const Babl *floating_format;
  gint        offset_x;
  gint        offset_y;

  g_return_val_if_fail (GIMP_IS_IMAGE (image), NULL);
  g_return_val_if_fail (drawable == NULL || GIMP_IS_DRAWABLE (drawable), NULL);
  g_return_val_if_fail (drawable == NULL ||
                        gimp_item_is_attached (GIMP_ITEM (drawable)), NULL);
  g_return_val_if_fail (GIMP_IS_IMAGE (paste) || GIMP_IS_BUFFER (paste), NULL);

  /*  change paste type to NEW_LAYER for cases where we can't attach a
   *  floating selection
   */
  if (! drawable                                            ||
      gimp_viewable_get_children (GIMP_VIEWABLE (drawable)) ||
      gimp_item_is_content_locked (GIMP_ITEM (drawable)))
    {
      paste_type = GIMP_PASTE_TYPE_NEW_LAYER;
    }

  /*  floating pastes always have the pasted-to drawable's format with
   *  alpha; if drawable == NULL, user is pasting into an empty image
   */
  if (drawable)
    floating_format = gimp_drawable_get_format_with_alpha (drawable);
  else
    floating_format = gimp_image_get_layer_format (image, TRUE);

  if (GIMP_IS_IMAGE (paste))
    {
      GType layer_type;

      layer = gimp_image_get_layer_iter (GIMP_IMAGE (paste))->data;

      switch (paste_type)
        {
        case GIMP_PASTE_TYPE_FLOATING:
        case GIMP_PASTE_TYPE_FLOATING_INTO:
          /*  when pasting as floating selection, force creation of a
           *  plain layer, so gimp_item_convert() will collapse a
           *  group layer
           */
          layer_type = GIMP_TYPE_LAYER;
          break;

        case GIMP_PASTE_TYPE_NEW_LAYER:
          layer_type = G_TYPE_FROM_INSTANCE (layer);
          break;

        default:
          g_return_val_if_reached (NULL);
        }

      layer = GIMP_LAYER (gimp_item_convert (GIMP_ITEM (layer),
                                             image, layer_type));

      switch (paste_type)
        {
        case GIMP_PASTE_TYPE_FLOATING:
        case GIMP_PASTE_TYPE_FLOATING_INTO:
          /*  when pasting as floating selection, get rid of the layer mask,
           *  and make sure the layer has the right format
           */
          if (gimp_layer_get_mask (layer))
            gimp_layer_apply_mask (layer, GIMP_MASK_DISCARD, FALSE);

          if (gimp_drawable_get_format (GIMP_DRAWABLE (layer)) !=
              floating_format)
            {
              gimp_drawable_convert_type (GIMP_DRAWABLE (layer), image,
                                          gimp_drawable_get_base_type (drawable),
                                          gimp_drawable_get_precision (drawable),
                                          TRUE,
                                          NULL,
                                          GEGL_DITHER_NONE, GEGL_DITHER_NONE,
                                          FALSE, NULL);
            }
          break;

        default:
          break;
        }
    }
  else if (GIMP_IS_BUFFER (paste))
    {
      layer = gimp_layer_new_from_buffer (GIMP_BUFFER (paste), image,
                                          floating_format,
                                          _("Pasted Layer"),
                                          GIMP_OPACITY_OPAQUE,
                                          GIMP_LAYER_MODE_NORMAL_LEGACY);
    }

  if (! layer)
    return NULL;

  gimp_edit_get_paste_offset (image, drawable, GIMP_OBJECT (layer),
                              viewport_x,
                              viewport_y,
                              viewport_width,
                              viewport_height,
                              &offset_x,
                              &offset_y);
  gimp_item_translate (GIMP_ITEM (layer), offset_x, offset_y, FALSE);

  gimp_image_undo_group_start (image, GIMP_UNDO_GROUP_EDIT_PASTE,
                               C_("undo-type", "Paste"));

  switch (paste_type)
    {
    case GIMP_PASTE_TYPE_FLOATING:
      /*  if there is a selection mask clear it - this might not
       *  always be desired, but in general, it seems like the correct
       *  behavior
       */
      if (! gimp_channel_is_empty (gimp_image_get_mask (image)))
        gimp_channel_clear (gimp_image_get_mask (image), NULL, TRUE);

      /* fall thru */

    case GIMP_PASTE_TYPE_FLOATING_INTO:
      floating_sel_attach (layer, drawable);
      break;

    case GIMP_PASTE_TYPE_NEW_LAYER:
      {
        GimpLayer *parent   = NULL;
        gint       position = 0;

        /* always add on top of the passed layer, where we would
         * attach a floating selection
         */
        if (GIMP_IS_LAYER (drawable))
          {
            parent   = gimp_layer_get_parent (GIMP_LAYER (drawable));
            position = gimp_item_get_index (GIMP_ITEM (drawable));
          }

        gimp_image_add_layer (image, layer, parent, position, TRUE);
      }
      break;
    }

  gimp_image_undo_group_end (image);

  return layer;
}

GimpImage *
gimp_edit_paste_as_new_image (Gimp       *gimp,
                              GimpObject *paste)
{
  GimpImage *image = NULL;

  g_return_val_if_fail (GIMP_IS_GIMP (gimp), NULL);
  g_return_val_if_fail (GIMP_IS_IMAGE (paste) || GIMP_IS_BUFFER (paste), NULL);

  if (GIMP_IS_IMAGE (paste))
    {
      image = gimp_image_duplicate (GIMP_IMAGE (paste));
    }
  else if (GIMP_IS_BUFFER (paste))
    {
      image = gimp_image_new_from_buffer (gimp, GIMP_BUFFER (paste));
    }

  return image;
}

const gchar *
gimp_edit_named_cut (GimpImage     *image,
                     const gchar   *name,
                     GimpDrawable  *drawable,
                     GimpContext   *context,
                     GError       **error)
{
  GimpBuffer *buffer;

  g_return_val_if_fail (GIMP_IS_IMAGE (image), NULL);
  g_return_val_if_fail (name != NULL, NULL);
  g_return_val_if_fail (GIMP_IS_DRAWABLE (drawable), NULL);
  g_return_val_if_fail (gimp_item_is_attached (GIMP_ITEM (drawable)), NULL);
  g_return_val_if_fail (GIMP_IS_CONTEXT (context), NULL);
  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  buffer = gimp_edit_extract (image, GIMP_PICKABLE (drawable),
                              context, TRUE, error);

  if (buffer)
    {
      gimp_object_set_name (GIMP_OBJECT (buffer), name);
      gimp_container_add (image->gimp->named_buffers, GIMP_OBJECT (buffer));
      g_object_unref (buffer);

      return gimp_object_get_name (buffer);
    }

  return NULL;
}

const gchar *
gimp_edit_named_copy (GimpImage     *image,
                      const gchar   *name,
                      GimpDrawable  *drawable,
                      GimpContext   *context,
                      GError       **error)
{
  GimpBuffer *buffer;

  g_return_val_if_fail (GIMP_IS_IMAGE (image), NULL);
  g_return_val_if_fail (name != NULL, NULL);
  g_return_val_if_fail (GIMP_IS_DRAWABLE (drawable), NULL);
  g_return_val_if_fail (gimp_item_is_attached (GIMP_ITEM (drawable)), NULL);
  g_return_val_if_fail (GIMP_IS_CONTEXT (context), NULL);
  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  buffer = gimp_edit_extract (image, GIMP_PICKABLE (drawable),
                              context, FALSE, error);

  if (buffer)
    {
      gimp_object_set_name (GIMP_OBJECT (buffer), name);
      gimp_container_add (image->gimp->named_buffers, GIMP_OBJECT (buffer));
      g_object_unref (buffer);

      return gimp_object_get_name (buffer);
    }

  return NULL;
}

const gchar *
gimp_edit_named_copy_visible (GimpImage    *image,
                              const gchar  *name,
                              GimpContext  *context,
                              GError      **error)
{
  GimpBuffer *buffer;

  g_return_val_if_fail (GIMP_IS_IMAGE (image), NULL);
  g_return_val_if_fail (name != NULL, NULL);
  g_return_val_if_fail (GIMP_IS_CONTEXT (context), NULL);
  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  buffer = gimp_edit_extract (image, GIMP_PICKABLE (image),
                              context, FALSE, error);

  if (buffer)
    {
      gimp_object_set_name (GIMP_OBJECT (buffer), name);
      gimp_container_add (image->gimp->named_buffers, GIMP_OBJECT (buffer));
      g_object_unref (buffer);

      return gimp_object_get_name (buffer);
    }

  return NULL;
}

void
gimp_edit_clear (GimpImage    *image,
                 GimpDrawable *drawable,
                 GimpContext  *context)
{
  GimpFillOptions *options;

  g_return_if_fail (GIMP_IS_IMAGE (image));
  g_return_if_fail (GIMP_IS_DRAWABLE (drawable));
  g_return_if_fail (gimp_item_is_attached (GIMP_ITEM (drawable)));
  g_return_if_fail (GIMP_IS_CONTEXT (context));

  options = gimp_fill_options_new (context->gimp, NULL, FALSE);

  if (gimp_drawable_has_alpha (drawable))
    gimp_fill_options_set_by_fill_type (options, context,
                                        GIMP_FILL_TRANSPARENT, NULL);
  else
    gimp_fill_options_set_by_fill_type (options, context,
                                        GIMP_FILL_BACKGROUND, NULL);

  gimp_edit_fill (image, drawable, options, C_("undo-type", "Clear"));

  g_object_unref (options);
}

void
gimp_edit_fill (GimpImage       *image,
                GimpDrawable    *drawable,
                GimpFillOptions *options,
                const gchar     *undo_desc)
{
  GeglBuffer  *buffer;
  gint         x, y, width, height;

  g_return_if_fail (GIMP_IS_IMAGE (image));
  g_return_if_fail (GIMP_IS_DRAWABLE (drawable));
  g_return_if_fail (gimp_item_is_attached (GIMP_ITEM (drawable)));
  g_return_if_fail (GIMP_IS_FILL_OPTIONS (options));

  if (! gimp_item_mask_intersect (GIMP_ITEM (drawable), &x, &y, &width, &height))
    return;  /*  nothing to do, but the fill succeeded  */

  buffer = gimp_fill_options_create_buffer (options, drawable,
                                            GEGL_RECTANGLE (0, 0,
                                                            width, height));

  if (! undo_desc)
    undo_desc = gimp_fill_options_get_undo_desc (options);

  gimp_drawable_apply_buffer (drawable, buffer,
                              GEGL_RECTANGLE (0, 0, width, height),
                              TRUE, undo_desc,
                              gimp_context_get_opacity (GIMP_CONTEXT (options)),
                              gimp_context_get_paint_mode (GIMP_CONTEXT (options)),
                              GIMP_LAYER_COLOR_SPACE_AUTO,
                              GIMP_LAYER_COLOR_SPACE_AUTO,
                              GIMP_LAYER_COMPOSITE_AUTO,
                              NULL, x, y);

  g_object_unref (buffer);

  gimp_drawable_update (drawable, x, y, width, height);
}

gboolean
gimp_edit_fade (GimpImage   *image,
                GimpContext *context)
{
  GimpDrawableUndo *undo;

  g_return_val_if_fail (GIMP_IS_IMAGE (image), FALSE);
  g_return_val_if_fail (GIMP_IS_CONTEXT (context), FALSE);

  undo = GIMP_DRAWABLE_UNDO (gimp_image_undo_get_fadeable (image));

  if (undo && undo->applied_buffer)
    {
      GimpDrawable *drawable;
      GeglBuffer   *buffer;

      drawable = GIMP_DRAWABLE (GIMP_ITEM_UNDO (undo)->item);

      g_object_ref (undo);
      buffer = g_object_ref (undo->applied_buffer);

      gimp_image_undo (image);

      gimp_drawable_apply_buffer (drawable, buffer,
                                  GEGL_RECTANGLE (0, 0,
                                                  gegl_buffer_get_width (undo->buffer),
                                                  gegl_buffer_get_height (undo->buffer)),
                                  TRUE,
                                  gimp_object_get_name (undo),
                                  gimp_context_get_opacity (context),
                                  gimp_context_get_paint_mode (context),
                                  GIMP_LAYER_COLOR_SPACE_AUTO,
                                  GIMP_LAYER_COLOR_SPACE_AUTO,
                                  GIMP_LAYER_COMPOSITE_AUTO,
                                  NULL, undo->x, undo->y);

      g_object_unref (buffer);
      g_object_unref (undo);

      return TRUE;
    }

  return FALSE;
}


/*  private functions  */

static GimpBuffer *
gimp_edit_extract (GimpImage     *image,
                   GimpPickable  *pickable,
                   GimpContext   *context,
                   gboolean       cut_pixels,
                   GError       **error)
{
  GeglBuffer *buffer;
  gint        offset_x;
  gint        offset_y;

  if (cut_pixels)
    gimp_image_undo_group_start (image, GIMP_UNDO_GROUP_EDIT_CUT,
                                 C_("undo-type", "Cut"));

  /*  Cut/copy the mask portion from the image  */
  buffer = gimp_selection_extract (GIMP_SELECTION (gimp_image_get_mask (image)),
                                   pickable, context,
                                   cut_pixels, FALSE, FALSE,
                                   &offset_x, &offset_y, error);

  if (cut_pixels)
    gimp_image_undo_group_end (image);

  if (buffer)
    {
      GimpBuffer *gimp_buffer;
      gdouble     res_x;
      gdouble     res_y;

      gimp_buffer = gimp_buffer_new (buffer, _("Global Buffer"),
                                     offset_x, offset_y, FALSE);
      g_object_unref (buffer);

      gimp_image_get_resolution (image, &res_x, &res_y);
      gimp_buffer_set_resolution (gimp_buffer, res_x, res_y);
      gimp_buffer_set_unit (gimp_buffer, gimp_image_get_unit (image));

      if (GIMP_IS_COLOR_MANAGED (pickable))
        {
          GimpColorProfile *profile =
            gimp_color_managed_get_color_profile (GIMP_COLOR_MANAGED (pickable));

          if (profile)
            gimp_buffer_set_color_profile (gimp_buffer, profile);
        }

      return gimp_buffer;
    }

  return NULL;
}
