/* unit tests: alignment
 *
 * Copyright 2026 David
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "common.h"

#include <string.h>

static FzTransform
similarity (double s, double deg, double tx, double ty)
{
  FzTransform t;

  t.a = s * cos (deg * M_PI / 180.0);
  t.b = s * sin (deg * M_PI / 180.0);
  t.tx = tx;
  t.ty = ty;
  return t;
}

/* the largest distance between where two transforms send the corners */
static double
corner_error (const FzTransform *t, const FzTransform *u, int w, int h)
{
  double cx = 0.5 * (w - 1), cy = 0.5 * (h - 1), m = 0.0;
  int    k;

  for (k = 0; k < 4; k++)
    {
      double x = (k & 1) ? w - 1 : 0, y = (k & 2) ? h - 1 : 0, a, b, c, d;

      fz_transform_apply (t, cx, cy, x, y, &a, &b);
      fz_transform_apply (u, cx, cy, x, y, &c, &d);
      m = fmax (m, hypot (a - c, b - d));
    }
  return m;
}

static void
test_transforms (void)
{
  FzTransform t = similarity (1.03, 2.0, 5.0, -3.0), u = similarity (0.98, -1.0, -2.0, 7.0);
  FzTransform tu, inv, id;
  double      x, y, x1, y1, x2, y2;

  fz_transform_compose (&t, &u, &tu);
  fz_transform_apply (&u, 50, 40, 10, 20, &x1, &y1);
  fz_transform_apply (&t, 50, 40, x1, y1, &x2, &y2);
  fz_transform_apply (&tu, 50, 40, 10, 20, &x, &y);
  check (fabs (x - x2) < 1e-9 && fabs (y - y2) < 1e-9, "compose (t, u) = t after u");
  fz_transform_invert (&t, &inv);
  fz_transform_compose (&inv, &t, &id);
  check (fabs (id.a - 1) < 1e-12 && fabs (id.b) < 1e-12 && fabs (id.tx) < 1e-12 &&
         fabs (id.ty) < 1e-12, "invert (t) after t = identity");
}

static void
test_phase (void)
{
  static const double shifts[][2] = { { 0, 0 }, { 7, -3 }, { -20, 11 }, { 2.5, 0.25 },
                                      { -0.4, -13.7 } };
  int    w = 256, h = 192, s;
  double worst = 0.0;

  for (s = 0; s < 5; s++)
    {
      FzTransform sh = { 1, 0, shifts[s][0], shifts[s][1] };
      FzImage    *a = pattern_image (w, h, 1, 11, NULL);
      FzImage    *b = pattern_image (w, h, 1, 11, &sh);
      double      dx, dy, peak;

      /* b (x) = pattern (x + d), a (x) = pattern (x): a (x) = b (x - d),
       * so the shift of a to b is -d */
      fz_phase_correlate (a->p, b->p, w, h, &dx, &dy, &peak);
      printf ("  shift (%g, %g): found (%.3f, %.3f), peak %.3f\n", shifts[s][0],
              shifts[s][1], -dx, -dy, peak);
      worst = fmax (worst, hypot (dx + shifts[s][0], dy + shifts[s][1]));
      fz_image_free (a);
      fz_image_free (b);
    }
  check (worst < 0.3, "phase correlation finds shifts of 0 to 23 pixels, "
         "to %.3f pixels", worst);
}

static void
test_pair (FzAlignModel model, FzTransform truth, const char *what, double limit,
           int photometric)
{
  int         w = 320, h = 240;
  FzImage    *a = pattern_image (w, h, 1, 21, NULL);
  FzImage    *b;
  FzTransform t, inv;
  double      conf, err;
  size_t      i;

  /* b is the pattern seen through the inverse: a position p in a shows
   * what b shows at truth (p) */
  fz_transform_invert (&truth, &inv);
  b = pattern_image (w, h, 1, 21, &inv);
  if (photometric)
    for (i = 0; i < (size_t) w * h; i++)
      {
        /* darker, less contrast, clipped at the top, like another
         * exposure */
        float v = 1.6f * b->p[i] - 0.25f;

        b->p[i] = v > 1.0f ? 1.0f : v < 0.0f ? 0.0f : sqrtf (v);
      }
  fz_align_pair (a->p, NULL, b->p, NULL, w, h, model, 0, &t, &conf);
  err = corner_error (&t, &truth, w, h);
  check (err < limit, "%s: found to %.4f pixels at the corners (confidence %.3f)",
         what, err, conf);
  fz_image_free (a);
  fz_image_free (b);
}

static void
test_stack (void)
{
  int          w = 256, h = 200, n = 5, k;
  FzTransform  truth[5], found[5], inv;
  FzImage     *img[5];
  ArraySource  as;
  FzSource     src;
  FzAlignParams p = { FZ_ALIGN_SIMILARITY, -1, 0 };
  double       conf[5], worst = 0.0;
  char        *err = NULL;

  for (k = 0; k < n; k++)
    {
      /* a slowly growing magnification and drift, as in a focus stack */
      truth[k] = similarity (1.0 + 0.006 * (k - 2), 0.1 * (k - 2), 1.7 * (k - 2), -0.9 * (k - 2));
      fz_transform_invert (&truth[k], &inv);
      /* image k shows the scene at truth (p): sample the pattern at the
       * inverse */
      img[k] = pattern_image (w, h, 3, 31, &inv);
    }
  /* ...relative to the middle one */
  array_source (&src, &as, img, NULL, n);
  fz_align (&src, &p, found, conf, NULL, &err);
  for (k = 0; k < n; k++)
    {
      FzTransform rel, ir;

      /* image k (q) = pattern (truth[k]^-1 (q)), so image 2 (p) = image k
       * (truth[k] (truth[2]^-1 (p))): that is what found[k] should be */
      fz_transform_invert (&truth[2], &ir);
      fz_transform_compose (&truth[k], &ir, &rel);
      worst = fmax (worst, corner_error (&found[k], &rel, w, h));
    }
  check (worst < 0.1, "a stack of 5, aligned to the middle one through its neighbours: "
         "similarity found to %.4f pixels at the corners", worst);
  check (as.loads == n, "alignment reads each image once (%d reads)", as.loads);
  for (k = 0; k < n; k++)
    fz_image_free (img[k]);
  free (err);
}

static void
test_warp (void)
{
  int         w = 64, h = 48, x, y, ok = 1;
  FzImage    *a = pattern_image (w, h, 3, 4, NULL), *b = fz_image_new (w, h, 3);
  float      *va = malloc (sizeof (float) * w * h), *vb = malloc (sizeof (float) * w * h);
  FzTransform t = { 1, 0, 3, -2 };
  size_t      n = (size_t) w * h;

  for (x = 0; x < w * h; x++)
    va[x] = 1.0f;
  fz_warp (a, va, &t, b, vb);
  for (y = 0; y < h; y++)
    for (x = 0; x < w; x++)
      {
        int sx = x + 3, sy = y - 2, ch;

        if (sx < w && sy >= 0)
          {
            for (ch = 0; ch < 3; ch++)
              ok &= b->p[ch * n + y * w + x] == a->p[ch * n + sy * w + sx];
            ok &= vb[y * w + x] == 1.0f;
          }
        else
          ok &= vb[y * w + x] == 0.0f;
      }
  check (ok, "warping by whole pixels moves the pixels exactly; outside is not valid");
  {
    int cx, cy, cw, ch;
    FzTransform ts[3] = { { 1, 0, 0, 0 }, { 1, 0, 3, -2 }, { 1, 0, -5, 4 } };

    fz_common_area (w, h, 3, ts, &cx, &cy, &cw, &ch);
    check (cx == 5 && cy == 2 && cw == w - 8 && ch == h - 6,
           "the common area of shifts (3, -2) and (-5, 4): %d, %d, %d x %d", cx, cy, cw, ch);
  }
  fz_image_free (a);
  fz_image_free (b);
  free (va);
  free (vb);
}

int
main (void)
{
  test_transforms ();
  test_phase ();
  test_pair (FZ_ALIGN_TRANSLATION, similarity (1, 0, 12.3, -7.8), "translation (12.3, -7.8)",
             0.05, 0);
  test_pair (FZ_ALIGN_TRANSLATION, similarity (1, 0, 0.37, 0.61), "translation (0.37, 0.61)",
             0.05, 0);
  test_pair (FZ_ALIGN_SCALE, similarity (1.025, 0, 3, 2), "scale 1.025 and a shift",
             0.1, 0);
  test_pair (FZ_ALIGN_SIMILARITY, similarity (0.98, 1.5, -4, 6),
             "rotation 1.5 degrees, scale 0.98, shift", 0.1, 0);
  test_pair (FZ_ALIGN_SIMILARITY, similarity (1.0, 0.5, 6, 3),
             "another exposure (gamma, gain, clipping), rotation 0.5 degrees", 0.2, 1);
  test_stack ();
  test_warp ();
  return test_finish ();
}
