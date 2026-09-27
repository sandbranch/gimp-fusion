/* shared helpers of the unit tests
 *
 * Copyright 2026 David
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "common.h"

#include <stdarg.h>
#include <string.h>

int test_failures, test_passes;

void
check (int ok, const char *fmt, ...)
{
  va_list ap;

  printf ("%s ", ok ? "PASS" : "FAIL");
  va_start (ap, fmt);
  vprintf (fmt, ap);
  va_end (ap);
  printf ("\n");
  fflush (stdout);
  if (ok)
    test_passes++;
  else
    test_failures++;
}

int
test_finish (void)
{
  printf ("%d passed, %d failed\n", test_passes, test_failures);
  return test_failures ? 1 : 0;
}

double
urand (unsigned *state)
{
  *state = *state * 1664525u + 1013904223u;
  return (*state >> 8) / 16777216.0;
}

double
pattern (double x, double y, unsigned seed)
{
  /* broadband, as photographs are: many blobs of many sizes, and a few
   * waves */
  unsigned s = seed * 7919u + 17u;
  double   v = 0.5;
  int      i;

  for (i = 0; i < 4; i++)
    {
      double fx = (urand (&s) - 0.5) * 0.2, fy = (urand (&s) - 0.5) * 0.2;
      double ph = urand (&s) * 6.283, a = 0.03 + 0.03 * urand (&s);

      v += a * sin (fx * x + fy * y + ph);
    }
  for (i = 0; i < 400; i++)
    {
      double cx = urand (&s) * 480.0 - 40.0, cy = urand (&s) * 400.0 - 40.0;
      double r = 1.5 + 9.0 * urand (&s) * urand (&s), a = (urand (&s) - 0.5) * 0.35;
      double dx = x - cx, dy = y - cy, q = (dx * dx + dy * dy) / (2.0 * r * r);

      if (q < 30.0)
        v += a * exp (-q);
    }
  return v;
}

FzImage *
pattern_image (int w, int h, int c, unsigned seed, const FzTransform *t)
{
  FzImage *img = fz_image_new (w, h, c);
  double   cx = 0.5 * (w - 1), cy = 0.5 * (h - 1);
  int      x, y, ch;

  if (! img)
    return NULL;
  for (y = 0; y < h; y++)
    for (x = 0; x < w; x++)
      {
        double xs = x, ys = y;

        if (t)
          fz_transform_apply (t, cx, cy, x, y, &xs, &ys);
        for (ch = 0; ch < c; ch++)
          fz_image_plane (img, ch)[(size_t) y * w + x] =
            (float) (pattern (xs, ys, seed) * (1.0 - 0.15 * ch) + 0.05 * ch);
      }
  return img;
}

double
max_abs_diff (const float *a, const float *b, size_t n)
{
  double m = 0.0;
  size_t i;

  for (i = 0; i < n; i++)
    {
      double d = fabs ((double) a[i] - b[i]);

      if (! (d <= m))
        m = d;   /* (NaN too) */
    }
  return m;
}

double
psnr (const float *a, const float *b, size_t n)
{
  double se = 0.0;
  size_t i;

  for (i = 0; i < n; i++)
    se += ((double) a[i] - b[i]) * ((double) a[i] - b[i]);
  se /= (double) n;
  return se > 0.0 ? 10.0 * log10 (1.0 / se) : 999.0;
}

int
array_load (void *data, int k, FzImage *img, float *valid, char **error)
{
  ArraySource *as = data;
  size_t       n = (size_t) img->w * img->h, i;

  (void) error;
  as->loads++;
  memcpy (img->p, as->img[k]->p, n * img->c * sizeof (float));
  for (i = 0; i < n; i++)
    valid[i] = as->valid && as->valid[k] ? as->valid[k][i] : 1.0f;
  return 1;
}

void
array_source (FzSource *src, ArraySource *as, FzImage **img, float **valid, int n)
{
  as->img = img;
  as->valid = valid;
  as->loads = 0;
  src->n = n;
  src->w = img[0]->w;
  src->h = img[0]->h;
  src->c = img[0]->c;
  src->load = array_load;
  src->data = as;
}
