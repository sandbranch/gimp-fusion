/* gimp-fusion core: Gaussian and Laplacian pyramids (Burt and Adelson
 * 1983), with the sizes, kernel and edge handling of Tom Mertens'
 * reference implementation of exposure fusion (downsample.m, upsample.m,
 * gaussian_pyramid.m, laplacian_pyramid.m, reconstruct_laplacian_pyramid.m
 * in github.com/Mericam/exposure-fusion), rewritten in C. That code's
 * licence asks for its notice to be kept:
 *
 *   Copyright (c) 2015, Tom Mertens
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the
 *     distribution.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * The Matlab expand pads by repeating the edge, puts 4 x the samples on
 * the even positions of a zero image, filters and crops. Worked out per
 * axis, that is: out[2a] = (s[a-1] + 6 s[a] + s[a+1]) / 8 and
 * out[2a+1] = (s[a] + s[a+1]) / 2, with indices clamped to the image,
 * which is what fz_upsample_add computes (tests/ref checks it against a
 * literal numpy port of the Matlab code).
 *
 * Copyright 2026 David (this file as a whole)
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "fz-private.h"

#include <stdlib.h>
#include <string.h>

static const float K[5] = { 0.0625f, 0.25f, 0.375f, 0.25f, 0.0625f };

int
fz_pyramid_levels (int w, int h)
{
  int m = FZ_MIN (w, h), n = 0;

  /* floor (log (m) / log (2)) without rounding trouble */
  while (m >= 2)
    {
      m /= 2;
      n++;
    }
  return FZ_CLAMP (n, 1, FZ_MAX_LEVELS);
}

/* half sample symmetric edges: -1 -> 0, -2 -> 1, n -> n - 1 */
static inline int
reflect (int i, int n)
{
  while (i < 0 || i >= n)
    {
      if (i < 0)
        i = -i - 1;
      if (i >= n)
        i = 2 * n - i - 1;
    }
  return i;
}

void
fz_downsample (const float *src, int w, int h, float *dst)
{
  int       w2 = (w + 1) / 2, h2 = (h + 1) / 2;
  float    *tmp = fz_alloc_floats ((size_t) w2 * h, 0);
  ptrdiff_t y;

  if (! tmp)
    {
      /* without memory, a slower path that needs none: filter each output
       * sample directly */
#pragma omp parallel for schedule(static)
      for (y = 0; y < h2; y++)
        {
          int x;

          for (x = 0; x < w2; x++)
            {
              float acc = 0.0f;
              int   i, j;

              for (j = -2; j <= 2; j++)
                {
                  const float *row = src + (size_t) reflect (2 * (int) y + j, h) * w;
                  float        racc = 0.0f;

                  for (i = -2; i <= 2; i++)
                    racc += K[i + 2] * row[reflect (2 * x + i, w)];
                  acc += K[j + 2] * racc;
                }
              dst[(size_t) y * w2 + x] = acc;
            }
        }
      return;
    }

#pragma omp parallel for schedule(static)
  for (y = 0; y < h; y++)
    {
      const float *s = src + (size_t) y * w;
      float       *t = tmp + (size_t) y * w2;
      int          x;

      for (x = 0; x < w2; x++)
        {
          int c = 2 * x;

          if (c >= 2 && c + 2 < w)
            t[x] = K[0] * (s[c - 2] + s[c + 2]) + K[1] * (s[c - 1] + s[c + 1])
                   + K[2] * s[c];
          else
            t[x] = K[0] * (s[reflect (c - 2, w)] + s[reflect (c + 2, w)])
                   + K[1] * (s[reflect (c - 1, w)] + s[reflect (c + 1, w)])
                   + K[2] * s[c];
        }
    }
#pragma omp parallel for schedule(static)
  for (y = 0; y < h2; y++)
    {
      int          c = 2 * (int) y;
      const float *r0 = tmp + (size_t) reflect (c - 2, h) * w2;
      const float *r1 = tmp + (size_t) reflect (c - 1, h) * w2;
      const float *r2 = tmp + (size_t) c * w2;
      const float *r3 = tmp + (size_t) reflect (c + 1, h) * w2;
      const float *r4 = tmp + (size_t) reflect (c + 2, h) * w2;
      float       *d = dst + (size_t) y * w2;
      int          x;

      for (x = 0; x < w2; x++)
        d[x] = K[0] * (r0[x] + r4[x]) + K[1] * (r1[x] + r3[x]) + K[2] * r2[x];
    }
  free (tmp);
}

int
fz_upsample_add (const float *src, int w, int h, float *dst, int W, int H,
                 float sign)
{
  float    *tmp;
  ptrdiff_t y;

  if (W > 2 * w || W < 2 * w - 1 || H > 2 * h || H < 2 * h - 1)
    return 0;
  tmp = fz_alloc_floats ((size_t) w * H, 0);
  if (! tmp)
    return 0;

  /* columns: w x h to w x H */
#pragma omp parallel for schedule(static)
  for (y = 0; y < H; y++)
    {
      int    a = (int) y / 2;
      float *t = tmp + (size_t) y * w;
      int    x;

      if ((y & 1) == 0)
        {
          const float *p = src + (size_t) FZ_MAX (a - 1, 0) * w;
          const float *c = src + (size_t) a * w;
          const float *n = src + (size_t) FZ_MIN (a + 1, h - 1) * w;

          for (x = 0; x < w; x++)
            t[x] = 0.125f * (p[x] + n[x]) + 0.75f * c[x];
        }
      else
        {
          const float *c = src + (size_t) a * w;
          const float *n = src + (size_t) FZ_MIN (a + 1, h - 1) * w;

          for (x = 0; x < w; x++)
            t[x] = 0.5f * (c[x] + n[x]);
        }
    }
  /* rows: w to W, added to dst */
#pragma omp parallel for schedule(static)
  for (y = 0; y < H; y++)
    {
      const float *t = tmp + (size_t) y * w;
      float       *d = dst + (size_t) y * W;
      int          x;

      for (x = 0; x < W; x++)
        {
          int   a = x / 2;
          float v;

          if ((x & 1) == 0)
            v = 0.125f * (t[FZ_MAX (a - 1, 0)] + t[FZ_MIN (a + 1, w - 1)])
                + 0.75f * t[a];
          else
            v = 0.5f * (t[a] + t[FZ_MIN (a + 1, w - 1)]);
          d[x] += sign * v;
        }
    }
  free (tmp);
  return 1;
}

FzPyramid *
fz_pyramid_new (int w, int h, int c, int levels)
{
  FzPyramid *pyr;
  int        l;

  if (w <= 0 || h <= 0 || c <= 0)
    return NULL;
  pyr = calloc (1, sizeof *pyr);
  if (! pyr)
    return NULL;
  if (levels <= 0)
    levels = fz_pyramid_levels (w, h);
  pyr->levels = FZ_CLAMP (levels, 1, FZ_MAX_LEVELS);
  pyr->c = c;
  for (l = 0; l < pyr->levels; l++)
    {
      pyr->w[l] = w;
      pyr->h[l] = h;
      pyr->lev[l] = fz_alloc_floats ((size_t) w * h * c, 1);
      if (! pyr->lev[l])
        {
          fz_pyramid_free (pyr);
          return NULL;
        }
      w = (w + 1) / 2;
      h = (h + 1) / 2;
    }
  return pyr;
}

void
fz_pyramid_free (FzPyramid *pyr)
{
  int l;

  if (! pyr)
    return;
  for (l = 0; l < FZ_MAX_LEVELS; l++)
    free (pyr->lev[l]);
  free (pyr);
}

int
fz_pyramid_collapse (FzPyramid *pyr)
{
  int l, ch;

  for (l = pyr->levels - 2; l >= 0; l--)
    {
      size_t n1 = (size_t) pyr->w[l + 1] * pyr->h[l + 1];
      size_t n0 = (size_t) pyr->w[l] * pyr->h[l];

      for (ch = 0; ch < pyr->c; ch++)
        if (! fz_upsample_add (pyr->lev[l + 1] + ch * n1, pyr->w[l + 1], pyr->h[l + 1],
                               pyr->lev[l] + ch * n0, pyr->w[l], pyr->h[l], 1.0f))
          return 0;
      /* the coarser levels are not needed any more */
      free (pyr->lev[l + 1]);
      pyr->lev[l + 1] = NULL;
    }
  return 1;
}

int
fz_pyramid_accumulate (FzPyramid *acc, float *img, float *weight)
{
  /* g and wt are the current level; they belong to the caller at level 0
   * and to us (own) below */
  float *g = img, *wt = weight;
  int    l, ch, ok = 1, own = 0;

  for (l = 0; l < acc->levels; l++)
    {
      int       w = acc->w[l], h = acc->h[l];
      size_t    n = (size_t) w * h;
      float    *g1 = NULL, *w1 = NULL;
      ptrdiff_t i;

      if (l < acc->levels - 1)
        {
          int    w2 = acc->w[l + 1], h2 = acc->h[l + 1];
          size_t n2 = (size_t) w2 * h2;

          g1 = fz_alloc_floats (n2 * acc->c, 0);
          w1 = weight ? fz_alloc_floats (n2, 0) : NULL;
          if (! g1 || (weight && ! w1))
            {
              free (g1);
              free (w1);
              ok = 0;
              break;
            }
          for (ch = 0; ch < acc->c; ch++)
            {
              fz_downsample (g + ch * n, w, h, g1 + ch * n2);
              /* the band: this level minus the expanded next one */
              if (! fz_upsample_add (g1 + ch * n2, w2, h2, g + ch * n, w, h, -1.0f))
                ok = 0;
            }
          if (weight)
            fz_downsample (wt, w, h, w1);
        }
      for (ch = 0; ch < acc->c && ok; ch++)
        {
          float       *a = acc->lev[l] + ch * n;
          const float *b = g + ch * n;

          if (weight)
            {
#pragma omp parallel for schedule(static)
              for (i = 0; i < (ptrdiff_t) n; i++)
                a[i] += wt[i] * b[i];
            }
          else
            {
#pragma omp parallel for schedule(static)
              for (i = 0; i < (ptrdiff_t) n; i++)
                a[i] += b[i];
            }
        }
      if (own)
        {
          free (g);
          free (wt);
        }
      g = g1;
      wt = w1;
      own = 1;
      if (! ok)
        break;
    }
  if (own)
    {
      free (g);
      free (wt);
    }
  return ok;
}

FzPyramid *
fz_laplacian_pyramid (const FzImage *img, int levels)
{
  FzPyramid *pyr = fz_pyramid_new (img->w, img->h, img->c, levels);
  float     *copy;

  if (! pyr)
    return NULL;
  copy = fz_alloc_floats ((size_t) img->w * img->h * img->c, 0);
  if (! copy)
    {
      fz_pyramid_free (pyr);
      return NULL;
    }
  memcpy (copy, img->p, (size_t) img->w * img->h * img->c * sizeof (float));
  if (! fz_pyramid_accumulate (pyr, copy, NULL))
    {
      fz_pyramid_free (pyr);
      pyr = NULL;
    }
  free (copy);
  return pyr;
}
