/* gimp-fusion core: images, filters, small helpers
 *
 * Copyright 2026 David
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "fz-private.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

FzImage *
fz_image_new (int w, int h, int c)
{
  FzImage *img;

  if (w <= 0 || h <= 0 || c <= 0)
    return NULL;
  img = calloc (1, sizeof *img);
  if (! img)
    return NULL;
  img->w = w;
  img->h = h;
  img->c = c;
  img->p = fz_alloc_floats ((size_t) w * h * c, 1);
  if (! img->p)
    {
      free (img);
      return NULL;
    }
  return img;
}

void
fz_image_free (FzImage *img)
{
  if (! img)
    return;
  free (img->p);
  free (img);
}

float *
fz_image_plane (const FzImage *img, int ch)
{
  return img->p + (size_t) ch * img->w * img->h;
}

float *
fz_alloc_floats (size_t n, int zero)
{
  if (n == 0)
    n = 1;
  if (n > SIZE_MAX / sizeof (float))
    return NULL;
  return zero ? calloc (n, sizeof (float)) : malloc (n * sizeof (float));
}

static void
luma (const FzImage *img, float *out, float kr, float kg, float kb)
{
  size_t n = (size_t) img->w * img->h;

  if (img->c < 3)
    {
      memcpy (out, img->p, n * sizeof (float));
      return;
    }
  {
    const float *r = fz_image_plane (img, 0);
    const float *g = fz_image_plane (img, 1);
    const float *b = fz_image_plane (img, 2);
    ptrdiff_t    i;

#pragma omp parallel for schedule(static)
    for (i = 0; i < (ptrdiff_t) n; i++)
      out[i] = kr * r[i] + kg * g[i] + kb * b[i];
  }
}

void
fz_luma601 (const FzImage *img, float *out)
{
  /* Matlab's rgb2gray, which Mertens' exposure_fusion.m uses */
  luma (img, out, 0.298936021293775f, 0.587043074451121f, 0.114020904255103f);
}

void
fz_luma709 (const FzImage *img, float *out)
{
  luma (img, out, 0.2126f, 0.7152f, 0.0722f);
}

/* ------------------------------------------------------------ filtering */

int
fz_gauss_blur (const float *src, float *dst, int w, int h, double sigma)
{
  float    *k, *tmp;
  int       r, i;
  double    sum = 0.0;
  ptrdiff_t y;

  if (sigma <= 0.0)
    {
      if (src != dst)
        memcpy (dst, src, (size_t) w * h * sizeof (float));
      return 1;
    }
  r = (int) ceil (3.0 * sigma);
  if (r < 1)
    r = 1;
  k = malloc ((2 * r + 1) * sizeof (float));
  tmp = fz_alloc_floats ((size_t) w * h, 0);
  if (! k || ! tmp)
    {
      free (k);
      free (tmp);
      return 0;
    }
  for (i = -r; i <= r; i++)
    sum += exp (-0.5 * i * i / (sigma * sigma));
  for (i = -r; i <= r; i++)
    k[i + r] = (float) (exp (-0.5 * i * i / (sigma * sigma)) / sum);

  /* rows: the middle without edge checks */
#pragma omp parallel for schedule(static)
  for (y = 0; y < h; y++)
    {
      const float *s = src + (size_t) y * w;
      float       *t = tmp + (size_t) y * w;
      int          x;

      for (x = 0; x < w; x++)
        {
          float acc = 0.0f;
          int   j;

          if (x >= r && x + r < w)
            for (j = -r; j <= r; j++)
              acc += k[j + r] * s[x + j];
          else
            for (j = -r; j <= r; j++)
              acc += k[j + r] * s[FZ_CLAMP (x + j, 0, w - 1)];
          t[x] = acc;
        }
    }
  /* columns, a row at a time */
#pragma omp parallel for schedule(static)
  for (y = 0; y < h; y++)
    {
      float *d = dst + (size_t) y * w;
      int    x, j;

      for (x = 0; x < w; x++)
        d[x] = 0.0f;
      for (j = -r; j <= r; j++)
        {
          const float *t = tmp + (size_t) FZ_CLAMP ((int) y + j, 0, h - 1) * w;
          float        kj = k[j + r];

          for (x = 0; x < w; x++)
            d[x] += kj * t[x];
        }
    }
  free (tmp);
  free (k);
  return 1;
}

void
fz_laplacian (const float *src, float *dst, int w, int h)
{
  ptrdiff_t y;

#pragma omp parallel for schedule(static)
  for (y = 0; y < h; y++)
    {
      const float *s  = src + (size_t) y * w;
      const float *up = src + (size_t) (y > 0 ? y - 1 : 0) * w;
      const float *dn = src + (size_t) (y < h - 1 ? y + 1 : h - 1) * w;
      float       *d  = dst + (size_t) y * w;
      int          x;

      for (x = 0; x < w; x++)
        {
          float l = s[x > 0 ? x - 1 : 0];
          float r = s[x < w - 1 ? x + 1 : w - 1];

          d[x] = up[x] + dn[x] + l + r - 4.0f * s[x];
        }
    }
}

/* ----------------------------------------------------------------- misc */

void
fz_set_threads (int n)
{
#ifdef _OPENMP
  if (n > 0)
    omp_set_num_threads (n);
#else
  (void) n;
#endif
}

char *
fz_strdup_printf (const char *fmt, ...)
{
  va_list ap;
  int     n;
  char   *s;

  va_start (ap, fmt);
  n = vsnprintf (NULL, 0, fmt, ap);
  va_end (ap);
  if (n < 0)
    return NULL;
  s = malloc ((size_t) n + 1);
  if (! s)
    return NULL;
  va_start (ap, fmt);
  vsnprintf (s, (size_t) n + 1, fmt, ap);
  va_end (ap);
  return s;
}

void
fz_set_error (char **error, const char *fmt, ...)
{
  va_list ap;
  int     n;
  char   *s;

  if (! error || *error)
    return;
  va_start (ap, fmt);
  n = vsnprintf (NULL, 0, fmt, ap);
  va_end (ap);
  if (n < 0)
    return;
  s = malloc ((size_t) n + 1);
  if (! s)
    return;
  va_start (ap, fmt);
  vsnprintf (s, (size_t) n + 1, fmt, ap);
  va_end (ap);
  *error = s;
}

double
fz_now (void)
{
#ifdef _OPENMP
  return omp_get_wtime ();
#else
  struct timespec ts;

  clock_gettime (CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + 1e-9 * ts.tv_nsec;
#endif
}

void
fz_report (FzProgress *prog, double fraction)
{
  if (prog && prog->progress)
    prog->progress (prog->data, FZ_CLAMP (fraction, 0.0, 1.0));
}
