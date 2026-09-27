/* gimp-fusion plug-in: layers in and out
 *
 * Copyright 2026 David
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef FUSION_LAYERS_H
#define FUSION_LAYERS_H

#include <libgimp/gimp.h>

#include "fz.h"

typedef enum
{
  FUSION_LAYERS_AUTO,       /* the selected layers if two or more, else the visible ones */
  FUSION_LAYERS_SELECTED,
  FUSION_LAYERS_VISIBLE,
  FUSION_LAYERS_ALL
} FusionLayers;

typedef enum
{
  FUSION_SPACE_IMAGE,       /* as the image stores its pixels */
  FUSION_SPACE_PERCEPTUAL,  /* non-linear, the image's TRC (sRGB for sRGB images) */
  FUSION_SPACE_LINEAR
} FusionSpace;

typedef struct
{
  FzSource      src;
  GimpImage    *image;
  GimpLayer   **layers;     /* bottom to top */
  int           n;
  const Babl   *format;     /* float, with alpha, in the chosen encoding */
  gboolean      linear;
  gboolean      any_alpha;  /* a layer has alpha, a mask, or does not cover the canvas */
  int           loads;
} LayerSource;

/* the layers to fuse, bottom of the stack first; NULL with *error */
GimpLayer **fusion_collect_layers (GimpImage *image, FusionLayers which, int *n,
                                   GError **error);

/* a source over the layers, in the image's canvas; space as chosen */
void fusion_layer_source_init (LayerSource *ls, GimpImage *image, GimpLayer **layers,
                               int n, FusionSpace space);

/* the rectangle all layers cover, in canvas coordinates */
void fusion_layers_cover (LayerSource *ls, int *x, int *y, int *w, int *h);

/* a new layer from the planes (and alpha, or NULL for opaque) in the
 * source's encoding, inserted above the topmost layer of the stack */
GimpLayer *fusion_add_layer (LayerSource *ls, const char *name, const FzImage *img,
                             const float *alpha, GimpLayer *above);

/* a new layer of gray values (0..1, -1 as transparent) as numbers:
 * written so that the stored values are proportional to them */
GimpLayer *fusion_add_value_layer (LayerSource *ls, const char *name, const float *v,
                                   GimpLayer *above);

#endif
