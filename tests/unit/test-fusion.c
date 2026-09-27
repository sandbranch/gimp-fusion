/* unit tests: exposure fusion and focus stacking
 *
 * Copyright 2026 David
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "common.h"

#include <string.h>

static FzImage *
copy_image (const FzImage *a)
{
  FzImage *b = fz_image_new (a->w, a->h, a->c);

  memcpy (b->p, a->p, (size_t) a->w * a->h * a->c * sizeof (float));
  return b;
}

static void
test_weights (void)
{
  FzExposureParams p;
  FzImage         *img = fz_image_new (5, 5, 3);
  float            w[25];
  size_t           i;
  double           e;

  fz_exposure_params_default (&p);
  for (i = 0; i < 25; i++)
    {
      img->p[i] = 0.5f;
      img->p[25 + i] = 0.3f;
      img->p[50 + i] = 0.7f;
    }
  img->p[12] = 0.6f;   /* the middle pixel's red */
  fz_exposure_weights (img, NULL, &p, w);
  {
    /* by hand: gray = 0.2989 R + 0.5870 G + 0.1140 B; the middle's
     * Laplacian is -4 x 0.1 x 0.2989; its std and Gaussians */
    double c = 4 * 0.1 * 0.298936021293775;
    double mu = (0.6 + 0.3 + 0.7) / 3.0;
    double s = sqrt (((0.6 - mu) * (0.6 - mu) + (0.3 - mu) * (0.3 - mu) +
                      (0.7 - mu) * (0.7 - mu)) / 3.0);
    double ex = exp (-0.01 / 0.08) * exp (-0.04 / 0.08) * exp (-0.04 / 0.08);

    e = c * s * ex + 1e-12;
    check (fabs (w[12] - e) < 1e-6 * e, "the weight of a pixel = C S E + 1e-12 as in "
           "the paper (%.8g, by hand %.8g)", w[12], e);
  }
  check (fabsf (w[0] - 1e-12f) < 1e-18f, "a pixel without contrast weighs 1e-12 "
         "(Mertens' epsilon)");
  p.contrast = 0.0;
  p.saturation = 0.0;
  p.exposure = 2.0;
  fz_exposure_weights (img, NULL, &p, w);
  e = pow (exp (-0.25 / 0.08 * 0.0) * exp (-0.04 / 0.08) * exp (-0.04 / 0.08), 2.0) + 1e-12;
  check (fabs (w[0] - e) < 1e-6 * e, "only well-exposedness, exponent 2: E^2");
  {
    float valid[25];

    for (i = 0; i < 25; i++)
      valid[i] = i < 10 ? 0.0f : 0.5f;
    fz_exposure_weights (img, valid, &p, w);
    check (w[3] == 0.0f && fabs (w[20] - 0.5 * e) < 1e-6 * e,
           "the weight is multiplied by the validity");
  }
  fz_image_free (img);
}

static void
test_identity_stacks (void)
{
  FzExposureParams ep;
  FzFocusParams    fp;
  FzImage         *a = pattern_image (97, 61, 3, 1, NULL);
  FzImage         *stack[4] = { a, a, a, a };
  FzImage         *out = fz_image_new (97, 61, 3);
  ArraySource      as;
  FzSource         src;
  char            *err = NULL;
  float            alpha[97 * 61];
  size_t           n = 97 * 61 * 3;
  int              m;

  fz_exposure_params_default (&ep);
  ep.clip = 0;
  {
    int    ok;
    double d;

    array_source (&src, &as, stack, NULL, 4);
    ok = fz_fuse_exposures (&src, &ep, out, alpha, NULL, &err);
    d = max_abs_diff (out->p, a->p, n);
    check (ok && d < 3e-6, "exposure fusion of 4 copies of an image gives the image (%.2g)", d);
    check (as.loads == 8, "each image is read twice (%d reads for 4 images)", as.loads);
    check (alpha[0] == 1.0f && alpha[97 * 61 - 1] == 1.0f, "the result is opaque");
    array_source (&src, &as, stack, NULL, 1);
    ok = fz_fuse_exposures (&src, &ep, out, NULL, NULL, &err);
    d = max_abs_diff (out->p, a->p, n);
    check (ok && d < 3e-6, "a stack of one image gives the image (%.2g)", d);

    fz_focus_params_default (&fp);
    fp.clip = 0;
    for (m = FZ_FOCUS_PYRAMID; m <= FZ_FOCUS_PIXELS; m++)
      {
        static const char *names[] = { "pyramid", "coefficients", "soft", "pixels" };

        fp.method = m;
        array_source (&src, &as, stack, NULL, 4);
        ok = fz_focus_stack (&src, &fp, out, NULL, NULL, NULL, &err);
        d = max_abs_diff (out->p, a->p, n);
        check (ok && d < 3e-6, "focus stack (%s) of 4 copies gives the image (%.2g, %d reads)",
               names[m], d, as.loads);
      }
  }
  fz_image_free (a);
  fz_image_free (out);
  free (err);
}

/* a focus stack of two: image 0 sharp on the left, image 1 on the right */
static void
test_focus_halves (int c)
{
  int              w = 160, h = 96, m;
  FzImage         *sharp = fz_image_new (w, h, c);
  FzImage         *blur, *i0, *i1, *out = fz_image_new (w, h, c);
  FzImage         *stack[2];
  FzFocusParams    fp;
  ArraySource      as;
  FzSource         src;
  char            *err = NULL;
  float           *depth = malloc (sizeof (float) * w * h);
  unsigned         s = 5;
  size_t           n = (size_t) w * h, i;
  int              ch;

  /* a noise texture: all frequencies, so blur shows */
  for (i = 0; i < n * c; i++)
    sharp->p[i] = (float) (0.2 + 0.6 * urand (&s));
  blur = copy_image (sharp);
  for (ch = 0; ch < c; ch++)
    fz_gauss_blur (fz_image_plane (sharp, ch), fz_image_plane (blur, ch), w, h, 2.5);
  i0 = copy_image (blur);
  i1 = copy_image (blur);
  for (ch = 0; ch < c; ch++)
    for (i = 0; i < n; i++)
      {
        if ((int) (i % w) < w / 2)
          fz_image_plane (i0, ch)[i] = fz_image_plane (sharp, ch)[i];
        else
          fz_image_plane (i1, ch)[i] = fz_image_plane (sharp, ch)[i];
      }
  stack[0] = i0;
  stack[1] = i1;
  fz_focus_params_default (&fp);
  for (m = FZ_FOCUS_PYRAMID; m <= FZ_FOCUS_PIXELS; m++)
    {
      static const char *names[] = { "pyramid", "coefficients", "soft", "pixels" };
      double             p_in, p_out, dl = 0.0, dr = 0.0;
      int                cnt = 0, x, y, exact = 1;

      fp.method = m;
      array_source (&src, &as, stack, NULL, 2);
      if (! fz_focus_stack (&src, &fp, out, NULL, depth, NULL, &err))
        {
          check (0, "focus stack %s: %s", names[m], err);
          continue;
        }
      p_in = psnr (blur->p, sharp->p, n * c);
      /* away from the seam in the middle */
      {
        double se = 0.0;

        for (ch = 0; ch < c; ch++)
          for (y = 0; y < h; y++)
            for (x = 0; x < w; x++)
              if (abs (x - w / 2) > 8)
                {
                  size_t k = (size_t) ch * n + (size_t) y * w + x;
                  double d = out->p[k] - sharp->p[k];

                  se += d * d;
                  cnt++;
                  if (m == FZ_FOCUS_PIXELS && out->p[k] != sharp->p[k])
                    exact = 0;
                }
        p_out = 10.0 * log10 (cnt / (se > 0 ? se : 1e-30));
      }
      check (p_out > p_in + 15.0, "focus stack (%s, %d channel%s): %.1f dB against the "
             "sharp image, the blurred one has %.1f dB", names[m], c, c > 1 ? "s" : "",
             p_out, p_in);
      if (m == FZ_FOCUS_PIXELS)
        check (exact, "focus stack (pixels): the sharp pixels exactly, bit for bit, "
               "away from the seam");
      cnt = 0;
      for (y = 4; y < h - 4; y++)
        for (x = 4; x < w - 4; x++)
          if (abs (x - w / 2) > 8)
            {
              if (x < w / 2)
                dl += depth[y * w + x];
              else
                dr += depth[y * w + x];
              cnt++;
            }
      dl /= cnt / 2;
      dr /= cnt / 2;
      if (m == FZ_FOCUS_PYRAMID)
        check (dl < 0.1 && dr > 0.9, "depth map: %.3f on the left (image 0), %.3f on "
               "the right (image 1)", dl, dr);
    }
  fz_image_free (sharp);
  fz_image_free (blur);
  fz_image_free (i0);
  fz_image_free (i1);
  fz_image_free (out);
  free (depth);
  free (err);
}

static void
test_validity (void)
{
  int              w = 128, h = 64, x, y, bad = 0;
  FzImage         *a = pattern_image (w, h, 3, 2, NULL);
  FzImage         *b = pattern_image (w, h, 3, 3, NULL);
  FzImage         *stack[2] = { a, b };
  float           *va = malloc (sizeof (float) * w * h), *vb = malloc (sizeof (float) * w * h);
  float           *valid[2] = { va, vb };
  float           *alpha = malloc (sizeof (float) * w * h);
  FzImage         *out = fz_image_new (w, h, 3);
  FzExposureParams ep;
  FzFocusParams    fp;
  ArraySource      as;
  FzSource         src;
  char            *err = NULL;
  size_t           n = (size_t) w * h;

  for (y = 0; y < h; y++)
    for (x = 0; x < w; x++)
      {
        va[y * w + x] = 1.0f;
        vb[y * w + x] = x < w / 2 ? 0.0f : 1.0f;
      }
  fz_exposure_params_default (&ep);
  ep.clip = 0;
  /* a shallow pyramid: at the coarse levels of a deep one the images
   * always mix, pixels or not (that is Laplacian blending) */
  ep.levels = 3;
  array_source (&src, &as, stack, valid, 2);
  fz_fuse_exposures (&src, &ep, out, alpha, NULL, &err);
  ep.levels = 0;
  /* where only a is valid, far from b's edge: a (the weight pyramid of b
   * is 0 there) */
  for (y = 0; y < h; y++)
    for (x = 0; x < w / 4; x++)
      {
        int ch;

        for (ch = 0; ch < 3; ch++)
          if (fabsf (out->p[ch * n + y * w + x] - a->p[ch * n + y * w + x]) > 1e-4f)
            bad++;
      }
  check (bad == 0, "exposure fusion (3 levels): where only one image has pixels, "
         "that image (%d pixels off by more than 1e-4)", bad);
  /* nothing valid: 0 and transparent */
  for (y = 0; y < h * w; y++)
    va[y] = vb[y] = 0.0f;
  fz_fuse_exposures (&src, &ep, out, alpha, NULL, &err);
  check (max_abs_diff (out->p, out->p + 1, 1) == 0.0 && out->p[0] == 0.0f &&
         alpha[0] == 0.0f && alpha[n - 1] == 0.0f,
         "exposure fusion without any valid pixel: 0, transparent");
  fz_focus_params_default (&fp);
  fz_focus_stack (&src, &fp, out, alpha, NULL, NULL, &err);
  check (out->p[0] == 0.0f && alpha[0] == 0.0f,
         "focus stack without any valid pixel: 0, transparent");
  /* a NaN pixel does not spread */
  for (y = 0; y < h * w; y++)
    va[y] = vb[y] = 1.0f;
  a->p[5 * w + 5] = NAN;
  b->p[n + 40 * w + 70] = INFINITY;
  fz_fuse_exposures (&src, &ep, out, alpha, NULL, &err);
  {
    size_t i;
    int    finite = 1;

    for (i = 0; i < n * 3; i++)
      finite &= isfinite (out->p[i]) != 0;
    check (finite, "exposure fusion: a NaN and an infinite pixel do not spread");
    fz_focus_stack (&src, &fp, out, alpha, NULL, NULL, &err);
    finite = 1;
    for (i = 0; i < n * 3; i++)
      finite &= isfinite (out->p[i]) != 0;
    check (finite, "focus stack: a NaN and an infinite pixel do not spread");
  }
  fz_image_free (a);
  fz_image_free (b);
  fz_image_free (out);
  free (va);
  free (vb);
  free (alpha);
  free (err);
}

static void
test_tiny (void)
{
  static const int sizes[][2] = { { 1, 1 }, { 2, 1 }, { 1, 3 }, { 2, 2 }, { 3, 3 } };
  int              s, ok = 1, m;

  for (s = 0; s < 5; s++)
    {
      int              w = sizes[s][0], h = sizes[s][1];
      FzImage         *a = pattern_image (w, h, 3, 1, NULL), *b = pattern_image (w, h, 3, 2, NULL);
      FzImage         *stack[2] = { a, b }, *out = fz_image_new (w, h, 3);
      FzExposureParams ep;
      FzFocusParams    fp;
      ArraySource      as;
      FzSource         src;
      char            *err = NULL;
      float            depth[9];

      fz_exposure_params_default (&ep);
      fz_focus_params_default (&fp);
      array_source (&src, &as, stack, NULL, 2);
      ok &= fz_fuse_exposures (&src, &ep, out, NULL, NULL, &err);
      for (m = 0; m < 4; m++)
        {
          fp.method = m;
          ok &= fz_focus_stack (&src, &fp, out, NULL, depth, NULL, &err);
        }
      fz_image_free (a);
      fz_image_free (b);
      fz_image_free (out);
      free (err);
    }
  check (ok, "images of 1 x 1 to 3 x 3 pixels fuse and stack");
}

static void
test_errors (void)
{
  FzSource         src = { 0 };
  FzExposureParams ep;
  FzImage         *out = fz_image_new (4, 4, 3);
  char            *err = NULL;

  fz_exposure_params_default (&ep);
  check (! fz_fuse_exposures (&src, &ep, out, NULL, NULL, &err) && err,
         "an empty stack is an error: %s", err ? err : "(no message)");
  free (err);
  fz_image_free (out);
}

int
main (void)
{
  test_weights ();
  test_identity_stacks ();
  test_focus_halves (1);
  test_focus_halves (3);
  test_validity ();
  test_tiny ();
  test_errors ();
  return test_finish ();
}
