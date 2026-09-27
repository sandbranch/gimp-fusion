/* unit tests: pyramids, filters, FFT
 *
 * Copyright 2026 David
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "common.h"

#include <string.h>

static const float K[5] = { 0.0625f, 0.25f, 0.375f, 0.25f, 0.0625f };

static int
refl (int i, int n)
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

/* the reduce step computed the slow way, straight from the definition */
static void
slow_down (const float *s, int w, int h, float *d)
{
  int x, y, i, j;

  for (y = 0; y < (h + 1) / 2; y++)
    for (x = 0; x < (w + 1) / 2; x++)
      {
        double acc = 0.0;

        for (j = -2; j <= 2; j++)
          for (i = -2; i <= 2; i++)
            acc += K[i + 2] * K[j + 2] * s[refl (2 * y + j, h) * w + refl (2 * x + i, w)];
        d[y * ((w + 1) / 2) + x] = (float) acc;
      }
}

/* Mertens' upsample.m, literally: replicate padding, 4 x samples on the
 * even positions of a zero image, filter with zeros outside, crop */
static void
slow_up (const float *s, int w, int h, float *d, int W, int H)
{
  int     pw = w + 2, ph = h + 2, uw = 2 * pw, uh = 2 * ph, x, y, i, j;
  double *u = calloc ((size_t) uw * uh, sizeof (double));
  int     oddw = 2 * w - W, oddh = 2 * h - H;

  for (y = 0; y < ph; y++)
    for (x = 0; x < pw; x++)
      {
        int sx = x - 1 < 0 ? 0 : x - 1 >= w ? w - 1 : x - 1;
        int sy = y - 1 < 0 ? 0 : y - 1 >= h ? h - 1 : y - 1;

        u[(2 * y) * uw + 2 * x] = 4.0 * s[sy * w + sx];
      }
  /* rows 2 .. uh - 3 - odd, the same for columns */
  for (y = 2; y <= uh - 3 - oddh; y++)
    for (x = 2; x <= uw - 3 - oddw; x++)
      {
        double acc = 0.0;

        for (j = -2; j <= 2; j++)
          for (i = -2; i <= 2; i++)
            {
              int yy = y + j, xx = x + i;

              if (yy >= 0 && yy < uh && xx >= 0 && xx < uw)
                acc += K[i + 2] * K[j + 2] * u[yy * uw + xx];
            }
        d[(y - 2) * W + (x - 2)] = (float) acc;
      }
  free (u);
}

static void
test_levels (void)
{
  check (fz_pyramid_levels (1, 1) == 1, "levels 1 x 1: 1");
  check (fz_pyramid_levels (2, 2) == 1, "levels 2 x 2: 1 (floor (log2 2))");
  check (fz_pyramid_levels (4, 9) == 2, "levels 4 x 9: 2");
  check (fz_pyramid_levels (1000, 700) == 9, "levels 1000 x 700: 9");
  check (fz_pyramid_levels (6000, 4000) == 11, "levels 6000 x 4000: 11");
  check (fz_pyramid_levels (1024, 4096) == 10, "levels 1024 x 4096: 10 (exactly log2)");
}

static void
test_reduce_expand (void)
{
  static const int sizes[][2] = { { 1, 1 }, { 1, 7 }, { 7, 1 }, { 2, 3 }, { 5, 5 },
                                  { 16, 9 }, { 33, 17 }, { 64, 64 } };
  int s, bad_down = 0, bad_up = 0;

  for (s = 0; s < (int) (sizeof sizes / sizeof sizes[0]); s++)
    {
      int      w = sizes[s][0], h = sizes[s][1], w2 = (w + 1) / 2, h2 = (h + 1) / 2;
      FzImage *img = pattern_image (w, h, 1, 3 + s, NULL);
      float   *a = calloc ((size_t) w2 * h2, sizeof (float));
      float   *b = calloc ((size_t) w2 * h2, sizeof (float));
      float   *ua = calloc ((size_t) w * h, sizeof (float));
      float   *ub = calloc ((size_t) w * h, sizeof (float));

      fz_downsample (img->p, w, h, a);
      slow_down (img->p, w, h, b);
      if (max_abs_diff (a, b, (size_t) w2 * h2) > 1e-6)
        bad_down++;
      /* expand the coarse one back up, both ways */
      fz_upsample_add (a, w2, h2, ua, w, h, 1.0f);
      slow_up (a, w2, h2, ub, w, h);
      if (max_abs_diff (ua, ub, (size_t) w * h) > 1e-6)
        {
          bad_up++;
          printf ("  expand %d x %d differs by %g\n", w, h, max_abs_diff (ua, ub, (size_t) w * h));
        }
      fz_image_free (img);
      free (a);
      free (b);
      free (ua);
      free (ub);
    }
  check (bad_down == 0, "reduce = Burt-Adelson 5-tap filter with symmetric edges, "
         "taken at even positions (8 sizes, 1 x 1 to 64 x 64)");
  check (bad_up == 0, "expand = a literal port of Mertens' upsample.m (8 sizes)");
  {
    float src[4] = { 1, 2, 3, 4 }, dst[16] = { 0 };

    check (! fz_upsample_add (src, 2, 2, dst, 5, 4, 1.0f),
           "expand refuses a size that is not twice (or twice minus one) the coarse one");
  }
  {
    /* a constant stays that constant, exactly */
    float c[35], d[9], u[35] = { 0 };
    int   i, ok = 1;

    for (i = 0; i < 35; i++)
      c[i] = 0.3f;
    fz_downsample (c, 7, 5, d);
    fz_upsample_add (d, 4, 3, u, 7, 5, 1.0f);
    for (i = 0; i < 9; i++)
      ok &= fabsf (d[i] - 0.3f) < 1e-7f;
    for (i = 0; i < 35; i++)
      ok &= fabsf (u[i] - 0.3f) < 1e-7f;
    check (ok, "reduce and expand keep a constant image constant");
  }
}

static void
test_roundtrip (void)
{
  static const int sizes[][3] = { { 1, 1, 1 }, { 1, 9, 3 }, { 9, 1, 1 }, { 2, 2, 3 },
                                  { 3, 5, 1 }, { 127, 65, 3 }, { 256, 256, 1 },
                                  { 513, 257, 3 } };
  int    s;
  double worst = 0.0;

  for (s = 0; s < (int) (sizeof sizes / sizeof sizes[0]); s++)
    {
      FzImage   *img = pattern_image (sizes[s][0], sizes[s][1], sizes[s][2], s, NULL);
      FzPyramid *pyr = fz_laplacian_pyramid (img, 0);
      double     d;

      if (! pyr || ! fz_pyramid_collapse (pyr))
        {
          check (0, "Laplacian pyramid of %d x %d", sizes[s][0], sizes[s][1]);
          continue;
        }
      d = max_abs_diff (pyr->lev[0], img->p, (size_t) img->w * img->h * img->c);
      worst = d > worst ? d : worst;
      fz_pyramid_free (pyr);
      fz_image_free (img);
    }
  check (worst < 2e-6, "decompose and collapse give the image back "
         "(8 sizes, 1 x 1 to 513 x 257, 1 and 3 channels; largest error %.2g)", worst);
  {
    FzImage   *img = pattern_image (100, 60, 1, 9, NULL);
    FzPyramid *pyr = fz_laplacian_pyramid (img, 12);
    int        ok = pyr && pyr->levels == 12 && pyr->w[11] == 1 && pyr->h[11] == 1;

    if (ok && fz_pyramid_collapse (pyr))
      ok = max_abs_diff (pyr->lev[0], img->p, 6000) < 2e-6;
    check (ok, "a deeper pyramid than the paper's (12 levels, down to 1 x 1) "
           "gives the image back too");
    fz_pyramid_free (pyr);
    fz_image_free (img);
  }
}

static void
test_filters (void)
{
  float  src[50], dst[50];
  int    i, ok = 1;
  double sum = 0.0;

  for (i = 0; i < 50; i++)
    src[i] = 0.7f;
  fz_laplacian (src, dst, 10, 5);
  for (i = 0; i < 50; i++)
    ok &= dst[i] == 0.0f;
  check (ok, "the Laplacian of a constant is 0, at the edges too");
  memset (src, 0, sizeof src);
  src[2 * 10 + 5] = 1.0f;
  fz_laplacian (src, dst, 10, 5);
  check (dst[25] == -4.0f && dst[24] == 1.0f && dst[15] == 1.0f && dst[26] == 1.0f,
         "the Laplacian of a point is [0 1 0; 1 -4 1; 0 1 0]");
  {
    float big[400] = { 0 }, out[400];

    big[10 * 20 + 10] = 1.0f;
    fz_gauss_blur (big, out, 20, 20, 1.5);
    for (i = 0; i < 400; i++)
      sum += out[i];
    check (fabs (sum - 1.0) < 1e-5 && out[210] > out[209] && out[209] > out[208] &&
           fabsf (out[209] - out[211]) < 1e-7f && fabsf (out[190] - out[209]) < 1e-7f,
           "Gaussian blur of a point: sum 1, peak in the middle, symmetric");
  }
  fz_gauss_blur (src, dst, 10, 5, 0.0);
  check (max_abs_diff (src, dst, 50) == 0.0, "Gaussian blur with sigma 0 copies");
}

static void
test_fft (void)
{
  int    n, worst_n = 0;
  double worst = 0.0;

  for (n = 1; n <= 64; n *= 2)
    {
      double re[64], im[64], r0[64], i0[64];
      unsigned s = n;
      int    k, j;

      for (k = 0; k < n; k++)
        {
          re[k] = r0[k] = urand (&s) - 0.5;
          im[k] = i0[k] = urand (&s) - 0.5;
        }
      fz_fft (re, im, n, 0);
      for (k = 0; k < n; k++)
        {
          double sr = 0.0, si = 0.0;

          for (j = 0; j < n; j++)
            {
              double a = -2.0 * M_PI * k * j / n;

              sr += r0[j] * cos (a) - i0[j] * sin (a);
              si += r0[j] * sin (a) + i0[j] * cos (a);
            }
          if (fabs (sr - re[k]) + fabs (si - im[k]) > worst)
            {
              worst = fabs (sr - re[k]) + fabs (si - im[k]);
              worst_n = n;
            }
        }
      fz_fft (re, im, n, 1);
      for (k = 0; k < n; k++)
        if (fabs (re[k] / n - r0[k]) + fabs (im[k] / n - i0[k]) > worst)
          worst = fabs (re[k] / n - r0[k]) + fabs (im[k] / n - i0[k]);
    }
  check (worst < 1e-12, "FFT = the DFT and back, n = 1 to 64 (largest error %.2g at n %d)",
         worst, worst_n);
  {
    double re[32] = { 0 }, im[32] = { 0 };

    check (! fz_fft2 (re, im, 6, 4, 0), "2-D FFT refuses sizes that are not powers of 2");
    re[1 * 8 + 2] = 1.0;
    fz_fft2 (re, im, 8, 4, 0);
    /* a delta at (2, 1): X(u, v) = exp (-2 pi i (2 u / 8 + v / 4)) */
    check (fabs (re[1 * 8 + 1] - cos (-2.0 * M_PI * (2.0 / 8 + 1.0 / 4))) < 1e-12 &&
           fabs (im[1 * 8 + 1] - sin (-2.0 * M_PI * (2.0 / 8 + 1.0 / 4))) < 1e-12,
           "2-D FFT of a point is the expected phase ramp");
  }
}

int
main (void)
{
  test_levels ();
  test_reduce_expand ();
  test_roundtrip ();
  test_filters ();
  test_fft ();
  return test_finish ();
}
