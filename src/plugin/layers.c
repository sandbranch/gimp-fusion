/* gimp-fusion plug-in: layers in and out
 *
 * A layer is read into the canvas as planar floats: its colors, and as
 * "valid" its alpha times its layer mask, 0 outside the layer. Outside the
 * layer the colors repeat its edge, so that the sharpness and contrast
 * measures see no edge that is not there.
 *
 * Copyright 2026 David
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <string.h>

#include "layers.h"

#define STRIPE 64

/* ------------------------------------------------------------- collecting */

/* every layer of the tree, top first, groups before their children */
static void
flatten (GimpLayer **layers, GPtrArray *out)
{
  int i;

  for (i = 0; layers && layers[i]; i++)
    {
      g_ptr_array_add (out, layers[i]);
      if (gimp_item_is_group (GIMP_ITEM (layers[i])))
        {
          GimpItem **children = gimp_item_get_children (GIMP_ITEM (layers[i]));

          flatten ((GimpLayer **) children, out);
          g_free (children);
        }
    }
}

GimpLayer **
fusion_collect_layers (GimpImage *image, FusionLayers which, int *n, GError **error)
{
  GimpLayer **top = gimp_image_get_layers (image);
  GimpLayer **sel = gimp_image_get_selected_layers (image);
  GPtrArray  *tree = g_ptr_array_new ();
  GPtrArray  *pick = g_ptr_array_new ();
  int         nsel = 0, i, j;

  for (i = 0; sel && sel[i]; i++)
    nsel++;
  if (which == FUSION_LAYERS_AUTO)
    which = nsel >= 2 ? FUSION_LAYERS_SELECTED : FUSION_LAYERS_VISIBLE;

  flatten (top, tree);
  if (which == FUSION_LAYERS_SELECTED)
    {
      /* in the order of the stack, whatever the order of selection */
      for (i = 0; i < (int) tree->len; i++)
        for (j = 0; j < nsel; j++)
          if (g_ptr_array_index (tree, i) == sel[j])
            g_ptr_array_add (pick, sel[j]);
    }
  else
    {
      /* the layers at the top level (groups as they look) */
      for (i = 0; top && top[i]; i++)
        if (which == FUSION_LAYERS_ALL || gimp_item_get_visible (GIMP_ITEM (top[i])))
          g_ptr_array_add (pick, top[i]);
    }
  g_ptr_array_free (tree, TRUE);
  g_free (top);
  g_free (sel);

  if (pick->len < 2)
    {
      g_set_error (error, GIMP_PLUG_IN_ERROR, 0,
                   which == FUSION_LAYERS_SELECTED ?
                   "Select two or more layers (or choose Layers: visible or all)." :
                   which == FUSION_LAYERS_VISIBLE ?
                   "The image needs two or more visible layers." :
                   "The image needs two or more layers.");
      g_ptr_array_free (pick, TRUE);
      return NULL;
    }
  /* bottom first */
  *n = pick->len;
  {
    GimpLayer **res = g_new0 (GimpLayer *, pick->len + 1);

    for (i = 0; i < (int) pick->len; i++)
      res[i] = g_ptr_array_index (pick, pick->len - 1 - i);
    g_ptr_array_free (pick, TRUE);
    return res;
  }
}

/* ---------------------------------------------------------------- reading */

static int
load_layer (void *data, int k, FzImage *img, float *valid, char **error)
{
  LayerSource  *ls = data;
  GimpDrawable *d = GIMP_DRAWABLE (ls->layers[k]);
  GimpLayerMask *mask = gimp_layer_get_mask (ls->layers[k]);
  GeglBuffer   *buf = gimp_drawable_get_buffer (d);
  GeglBuffer   *mbuf = mask ? gimp_drawable_get_buffer (GIMP_DRAWABLE (mask)) : NULL;
  int           W = img->w, H = img->h, c = img->c, nc = c + 1;
  int           ox, oy, lw, lh, x0, x1, y0, y1, y, x, ch;
  size_t        n = (size_t) W * H;
  float        *tmp, *mtmp = NULL;

  ls->loads++;
  gimp_drawable_get_offsets (d, &ox, &oy);
  lw = gimp_drawable_get_width (d);
  lh = gimp_drawable_get_height (d);
  x0 = MAX (ox, 0);
  y0 = MAX (oy, 0);
  x1 = MIN (ox + lw, W);
  y1 = MIN (oy + lh, H);

  memset (valid, 0, n * sizeof (float));
  if (x0 >= x1 || y0 >= y1)
    {
      /* nowhere on the canvas */
      memset (img->p, 0, n * c * sizeof (float));
      g_object_unref (buf);
      if (mbuf)
        g_object_unref (mbuf);
      return 1;
    }
  tmp = g_try_malloc (sizeof (float) * (size_t) (x1 - x0) * STRIPE * nc);
  if (mbuf)
    mtmp = g_try_malloc (sizeof (float) * (size_t) (x1 - x0) * STRIPE);
  if (! tmp || (mbuf && ! mtmp))
    {
      g_free (tmp);
      g_free (mtmp);
      g_object_unref (buf);
      if (mbuf)
        g_object_unref (mbuf);
      *error = fz_strdup_printf ("not enough memory to read %s",
                                 gimp_item_get_name (GIMP_ITEM (d)));
      return 0;
    }
  for (y = y0; y < y1; y += STRIPE)
    {
      int           rows = MIN (STRIPE, y1 - y);
      GeglRectangle r = { x0 - ox, y - oy, x1 - x0, rows };
      int           j;

      gegl_buffer_get (buf, &r, 1.0, ls->format, tmp, GEGL_AUTO_ROWSTRIDE, GEGL_ABYSS_NONE);
      if (mbuf)
        gegl_buffer_get (mbuf, &r, 1.0, babl_format ("Y float"), mtmp, GEGL_AUTO_ROWSTRIDE,
                         GEGL_ABYSS_NONE);
      for (j = 0; j < rows; j++)
        {
          const float *s = tmp + (size_t) j * (x1 - x0) * nc;
          size_t       row = (size_t) (y + j) * W;

          for (x = x0; x < x1; x++, s += nc)
            {
              float a = s[c];

              for (ch = 0; ch < c; ch++)
                img->p[ch * n + row + x] = s[ch];
              if (mtmp)
                a *= mtmp[(size_t) j * (x1 - x0) + (x - x0)];
              valid[row + x] = CLAMP (a, 0.0f, 1.0f);
            }
        }
    }
  g_free (tmp);
  g_free (mtmp);
  g_object_unref (buf);
  if (mbuf)
    g_object_unref (mbuf);

  /* outside the layer: its edge repeated */
  for (ch = 0; ch < c; ch++)
    {
      float *p = img->p + ch * n;

      for (y = y0; y < y1; y++)
        {
          float *row = p + (size_t) y * W;

          for (x = 0; x < x0; x++)
            row[x] = row[x0];
          for (x = x1; x < W; x++)
            row[x] = row[x1 - 1];
        }
      for (y = 0; y < y0; y++)
        memcpy (p + (size_t) y * W, p + (size_t) y0 * W, W * sizeof (float));
      for (y = y1; y < H; y++)
        memcpy (p + (size_t) y * W, p + (size_t) (y1 - 1) * W, W * sizeof (float));
    }
  return 1;
}

void
fusion_layer_source_init (LayerSource *ls, GimpImage *image, GimpLayer **layers,
                          int n, FusionSpace space)
{
  gboolean    gray = gimp_image_get_base_type (image) == GIMP_GRAY;
  const Babl *space_babl = babl_format_get_space (gimp_drawable_get_format (GIMP_DRAWABLE (layers[0])));
  int         W = gimp_image_get_width (image), H = gimp_image_get_height (image);
  int         k;

  memset (ls, 0, sizeof *ls);
  ls->image = image;
  ls->layers = layers;
  ls->n = n;
  if (space == FUSION_SPACE_IMAGE)
    {
      switch (gimp_image_get_precision (image))
        {
        case GIMP_PRECISION_U8_LINEAR:
        case GIMP_PRECISION_U16_LINEAR:
        case GIMP_PRECISION_U32_LINEAR:
        case GIMP_PRECISION_HALF_LINEAR:
        case GIMP_PRECISION_FLOAT_LINEAR:
        case GIMP_PRECISION_DOUBLE_LINEAR:
          ls->linear = TRUE;
          break;
        default:
          ls->linear = FALSE;
        }
    }
  else
    ls->linear = space == FUSION_SPACE_LINEAR;
  ls->format = babl_format_with_space (gray ? (ls->linear ? "YA float" : "Y'A float")
                                            : (ls->linear ? "RGBA float" : "R'G'B'A float"),
                                       space_babl);
  for (k = 0; k < n; k++)
    {
      GimpDrawable *d = GIMP_DRAWABLE (layers[k]);
      int           ox, oy;

      gimp_drawable_get_offsets (d, &ox, &oy);
      if (gimp_drawable_has_alpha (d) || gimp_layer_get_mask (layers[k]) ||
          ox > 0 || oy > 0 || ox + gimp_drawable_get_width (d) < W ||
          oy + gimp_drawable_get_height (d) < H)
        ls->any_alpha = TRUE;
    }
  ls->src.n = n;
  ls->src.w = W;
  ls->src.h = H;
  ls->src.c = gray ? 1 : 3;
  ls->src.load = load_layer;
  ls->src.data = ls;
}

void
fusion_layers_cover (LayerSource *ls, int *x, int *y, int *w, int *h)
{
  int x0 = 0, y0 = 0, x1 = ls->src.w, y1 = ls->src.h, k;

  for (k = 0; k < ls->n; k++)
    {
      GimpDrawable *d = GIMP_DRAWABLE (ls->layers[k]);
      int           ox, oy;

      gimp_drawable_get_offsets (d, &ox, &oy);
      x0 = MAX (x0, ox);
      y0 = MAX (y0, oy);
      x1 = MIN (x1, ox + gimp_drawable_get_width (d));
      y1 = MIN (y1, oy + gimp_drawable_get_height (d));
    }
  *x = x0;
  *y = y0;
  *w = MAX (0, x1 - x0);
  *h = MAX (0, y1 - y0);
}

/* ---------------------------------------------------------------- writing */

static GimpLayer *
new_layer (LayerSource *ls, const char *name, gboolean alpha, GimpLayer *above)
{
  gboolean   gray = ls->src.c == 1;
  GimpLayer *layer;
  GimpLayer *parent = GIMP_LAYER (gimp_item_get_parent (GIMP_ITEM (above)));

  layer = gimp_layer_new (ls->image, name, ls->src.w, ls->src.h,
                          gray ? (alpha ? GIMP_GRAYA_IMAGE : GIMP_GRAY_IMAGE)
                               : (alpha ? GIMP_RGBA_IMAGE : GIMP_RGB_IMAGE),
                          100.0, GIMP_LAYER_MODE_NORMAL);
  if (! layer)
    return NULL;
  gimp_image_insert_layer (ls->image, layer, parent,
                           gimp_image_get_item_position (ls->image, GIMP_ITEM (above)));
  return layer;
}

GimpLayer *
fusion_add_layer (LayerSource *ls, const char *name, const FzImage *img,
                  const float *alpha, GimpLayer *above)
{
  int         W = ls->src.w, H = ls->src.h, c = ls->src.c, nc = c + 1, y, x, ch;
  size_t      n = (size_t) W * H;
  GimpLayer  *layer = new_layer (ls, name, alpha != NULL, above);
  GeglBuffer *buf;
  float      *tmp;

  if (! layer)
    return NULL;
  buf = gimp_drawable_get_buffer (GIMP_DRAWABLE (layer));
  tmp = g_malloc (sizeof (float) * (size_t) W * STRIPE * nc);
  for (y = 0; y < H; y += STRIPE)
    {
      int           rows = MIN (STRIPE, H - y);
      GeglRectangle r = { 0, y, W, rows };
      int           j;

      for (j = 0; j < rows; j++)
        {
          float *d = tmp + (size_t) j * W * nc;
          size_t row = (size_t) (y + j) * W;

          for (x = 0; x < W; x++, d += nc)
            {
              for (ch = 0; ch < c; ch++)
                d[ch] = img->p[ch * n + row + x];
              d[c] = alpha ? alpha[row + x] : 1.0f;
            }
        }
      gegl_buffer_set (buf, &r, 0, ls->format, tmp, GEGL_AUTO_ROWSTRIDE);
    }
  g_free (tmp);
  g_object_unref (buf);
  gimp_drawable_update (GIMP_DRAWABLE (layer), 0, 0, W, H);
  return layer;
}

GimpLayer *
fusion_add_value_layer (LayerSource *ls, const char *name, const float *v, GimpLayer *above)
{
  int         W = ls->src.w, H = ls->src.h, y, x;
  gboolean    any_missing = FALSE;
  size_t      i, n = (size_t) W * H;
  GimpLayer  *layer;
  GeglBuffer *buf;
  float      *tmp;
  /* in the image's own encoding, so that the numbers stored are the
   * values (a linear image stores them as they are; a non-linear one
   * would otherwise bend them by its curve) */
  const Babl *fmt = babl_format_with_space (ls->linear ? "YA float" : "Y'A float",
                                            babl_format_get_space (ls->format));

  for (i = 0; i < n; i++)
    any_missing |= v[i] < 0.0f;
  layer = new_layer (ls, name, any_missing, above);
  if (! layer)
    return NULL;
  buf = gimp_drawable_get_buffer (GIMP_DRAWABLE (layer));
  tmp = g_malloc (sizeof (float) * (size_t) W * STRIPE * 2);
  for (y = 0; y < H; y += STRIPE)
    {
      int           rows = MIN (STRIPE, H - y);
      GeglRectangle r = { 0, y, W, rows };
      int           j;

      for (j = 0; j < rows; j++)
        for (x = 0; x < W; x++)
          {
            float val = v[(size_t) (y + j) * W + x];

            tmp[((size_t) j * W + x) * 2] = val < 0.0f ? 0.0f : val;
            tmp[((size_t) j * W + x) * 2 + 1] = val < 0.0f ? 0.0f : 1.0f;
          }
      gegl_buffer_set (buf, &r, 0, fmt, tmp, GEGL_AUTO_ROWSTRIDE);
    }
  g_free (tmp);
  g_object_unref (buf);
  gimp_drawable_update (GIMP_DRAWABLE (layer), 0, 0, W, H);
  return layer;
}
