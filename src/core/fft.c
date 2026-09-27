/* gimp-fusion core: a small radix-2 FFT, enough for phase correlation on
 * images of up to a few thousand pixels a side
 *
 * Copyright 2026 David
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "fz-private.h"

#include <math.h>
#include <stdlib.h>

void
fz_fft (double *re, double *im, int n, int inverse)
{
  int i, j, len;

  /* bit reversal */
  for (i = 1, j = 0; i < n; i++)
    {
      int bit = n >> 1;

      for (; j & bit; bit >>= 1)
        j ^= bit;
      j ^= bit;
      if (i < j)
        {
          double t = re[i];

          re[i] = re[j];
          re[j] = t;
          t = im[i];
          im[i] = im[j];
          im[j] = t;
        }
    }
  for (len = 2; len <= n; len <<= 1)
    {
      double ang = 2.0 * M_PI / len * (inverse ? 1.0 : -1.0);
      double wr = cos (ang), wi = sin (ang);

      for (i = 0; i < n; i += len)
        {
          double cr = 1.0, ci = 0.0;
          int    k;

          for (k = 0; k < len / 2; k++)
            {
              int    a = i + k, b = i + k + len / 2;
              double xr = re[b] * cr - im[b] * ci;
              double xi = re[b] * ci + im[b] * cr;
              double t;

              re[b] = re[a] - xr;
              im[b] = im[a] - xi;
              re[a] += xr;
              im[a] += xi;
              t = cr * wr - ci * wi;
              ci = cr * wi + ci * wr;
              cr = t;
            }
        }
    }
}

int
fz_fft2 (double *re, double *im, int w, int h, int inverse)
{
  double   *cr, *ci;
  ptrdiff_t y;
  int       x;

  if ((w & (w - 1)) || (h & (h - 1)) || w < 1 || h < 1)
    return 0;
#pragma omp parallel for schedule(static)
  for (y = 0; y < h; y++)
    fz_fft (re + (size_t) y * w, im + (size_t) y * w, w, inverse);

  cr = malloc (sizeof (double) * (size_t) h * w);
  ci = malloc (sizeof (double) * (size_t) h * w);
  if (! cr || ! ci)
    {
      free (cr);
      free (ci);
      return 0;
    }
  /* columns, transposed into rows */
#pragma omp parallel for schedule(static)
  for (x = 0; x < w; x++)
    {
      double *r = cr + (size_t) x * h, *i = ci + (size_t) x * h;
      int     yy;

      for (yy = 0; yy < h; yy++)
        {
          r[yy] = re[(size_t) yy * w + x];
          i[yy] = im[(size_t) yy * w + x];
        }
      fz_fft (r, i, h, inverse);
      for (yy = 0; yy < h; yy++)
        {
          re[(size_t) yy * w + x] = r[yy];
          im[(size_t) yy * w + x] = i[yy];
        }
    }
  free (cr);
  free (ci);
  return 1;
}
