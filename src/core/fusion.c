/* gimp-fusion core: exposure fusion (Mertens, Kautz and Van Reeth 2007)
 * and focus stacking, streaming over the stack: the images are loaded
 * one at a time, twice, so that memory does not grow with their number.
 *
 * Exposure fusion, as in the paper and Mertens' reference code: weights
 * W = C^wc S^ws E^we + 1e-12 (contrast: |Laplacian| of the gray image;
 * saturation: standard deviation of R, G and B; well-exposedness: the
 * product over the channels of exp (-(v - 0.5)^2 / (2 sigma^2))),
 * normalized to sum 1 at each pixel; the result's Laplacian pyramid is
 * the sum of the images' Laplacian pyramids times the Gaussian pyramids of
 * their weights. The first pass sums the weights, the second blends.
 *
 * Focus stacking: a sharpness measure per image (the local energy of the
 * Laplacian, or the local variance), then one of
 *   - pyramid: the sharpest image at each pixel gets weight 1, the others
 *     0, and those masks are blended as above (the "hard mask" of enfuse,
 *     the method of its focus stacking);
 *   - coefficients: at each level of the Laplacian pyramid the
 *     coefficient of the image with the most local energy there (Burt and
 *     Kolczynski 1993); the coarsest level is averaged;
 *   - soft: weights sharpness^power, blended as above;
 *   - pixels: the sharpest image's pixel, unblended (the values of the
 *     inputs exactly, for measurements).
 *
 * Copyright 2026 David
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "fz-private.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define EPS_WEIGHT 1e-12f

/* ------------------------------------------------------------- weights */

void
fz_exposure_params_default (FzExposureParams *p)
{
  p->contrast = 1.0;
  p->saturation = 1.0;
  p->exposure = 1.0;
  p->sigma = 0.2;
  p->center = 0.5;
  p->levels = 0;
  p->clip = 1;
}

static inline float
power (float v, float e)
{
  if (e == 1.0f)
    return v;
  if (e == 2.0f)
    return v * v;
  return powf (v, e);
}

int
fz_exposure_weights (const FzImage *img, const float *valid,
                     const FzExposureParams *p, float *weight)
{
  size_t    n = (size_t) img->w * img->h;
  float    *con = NULL;
  float     wc = (float) p->contrast, ws = (float) p->saturation;
  float     we = (float) p->exposure;
  float     inv2s2 = (float) (0.5 / (p->sigma * p->sigma));
  float     center = (float) p->center;
  int       c = img->c;
  ptrdiff_t i;

  if (wc > 0.0f)
    {
      float *gray = fz_alloc_floats (n, 0);

      con = fz_alloc_floats (n, 0);
      if (! gray || ! con)
        {
          free (gray);
          free (con);
          return 0;
        }
      fz_luma601 (img, gray);
      fz_laplacian (gray, con, img->w, img->h);
      free (gray);
    }

#pragma omp parallel for schedule(static)
  for (i = 0; i < (ptrdiff_t) n; i++)
    {
      float w = 1.0f;

      if (con)
        {
          /* contrast below 1e-6 of the value is none: it is below the
           * resolution of 16-bit data, and it is what float rounding
           * leaves on ramps where Mertens' code (in double) gets exactly
           * 0; left in, those crumbs would outweigh its 1e-12 and decide
           * areas that no image shows any contrast in */
          float cv = fabsf (con[i]);

          if (cv < 1e-6f * FZ_MAX (1.0f, fabsf (img->p[i])))
            cv = 0.0f;
          w *= power (cv, wc);
        }
      if (ws > 0.0f && c >= 3)
        {
          float r = img->p[i], g = img->p[n + i], b = img->p[2 * n + i];
          float mu = (r + g + b) / 3.0f;
          float s = sqrtf (((r - mu) * (r - mu) + (g - mu) * (g - mu)
                            + (b - mu) * (b - mu)) / 3.0f);

          w *= power (s, ws);
        }
      if (we > 0.0f)
        {
          float e = 1.0f;
          int   ch;

          for (ch = 0; ch < FZ_MIN (c, 3); ch++)
            {
              float d = img->p[ch * n + i] - center;

              e *= expf (-d * d * inv2s2);
            }
          w *= power (e, we);
        }
      /* Mertens' code adds 1e-12 so that no pixel is left without weight */
      w += EPS_WEIGHT;
      if (valid)
        w *= FZ_CLAMP (valid[i], 0.0f, 1.0f);
      /* NaN in, nothing out */
      weight[i] = w == w ? w : 0.0f;
    }
  free (con);
  return 1;
}

/* ------------------------------------------------------------- buffers */

typedef struct
{
  FzImage *img;
  float   *valid;
  float   *weight;
} Scratch;

static int
scratch_init (Scratch *s, const FzSource *src)
{
  size_t n = (size_t) src->w * src->h;

  s->img = fz_image_new (src->w, src->h, src->c);
  s->valid = fz_alloc_floats (n, 0);
  s->weight = fz_alloc_floats (n, 0);
  return s->img && s->valid && s->weight;
}

static void
scratch_free (Scratch *s)
{
  fz_image_free (s->img);
  free (s->valid);
  free (s->weight);
}

static int
load (const FzSource *src, int k, Scratch *s, char **error)
{
  size_t    n = (size_t) src->w * src->h;
  ptrdiff_t i;

  if (! src->load (src->data, k, s->img, s->valid, error))
    {
      fz_set_error (error, "could not read image %d of the stack", k + 1);
      return 0;
    }
  /* NaN and infinity would spread over the whole pyramid: they count as
   * missing (and hold 0) */
#pragma omp parallel for schedule(static)
  for (i = 0; i < (ptrdiff_t) n; i++)
    {
      int ch, bad = ! (s->valid[i] == s->valid[i]);

      for (ch = 0; ch < src->c; ch++)
        if (! isfinite (s->img->p[ch * n + i]))
          {
            s->img->p[ch * n + i] = 0.0f;
            bad = 1;
          }
      if (bad)
        s->valid[i] = 0.0f;
    }
  return 1;
}

static int
check_source (const FzSource *src, char **error)
{
  if (! src || src->n < 1 || src->w < 1 || src->h < 1 ||
      (src->c != 1 && src->c != 3) || ! src->load)
    {
      fz_set_error (error, "no images to fuse");
      return 0;
    }
  return 1;
}

static void
finish (const FzPyramid *acc, FzImage *out, int clip)
{
  size_t    n = (size_t) out->w * out->h * out->c;
  ptrdiff_t i;

  memcpy (out->p, acc->lev[0], n * sizeof (float));
  if (clip)
    {
#pragma omp parallel for schedule(static)
      for (i = 0; i < (ptrdiff_t) n; i++)
        out->p[i] = FZ_CLAMP (out->p[i], 0.0f, 1.0f);
    }
}

static void
update_alpha (float *alpha, const float *valid, size_t n)
{
  ptrdiff_t i;

  if (! alpha)
    return;
#pragma omp parallel for schedule(static)
  for (i = 0; i < (ptrdiff_t) n; i++)
    alpha[i] = FZ_MAX (alpha[i], FZ_CLAMP (valid[i], 0.0f, 1.0f));
}

/* ---------------------------------------------------- exposure fusion */

int
fz_fuse_exposures (const FzSource *src, const FzExposureParams *p,
                   FzImage *out, float *alpha, FzProgress *prog,
                   char **error)
{
  Scratch    s = { 0 };
  FzPyramid *acc = NULL, *wsum = NULL;
  float     *sum = NULL;
  size_t     n;
  int        k, ok = 0;
  double     t0 = fz_now ();
  ptrdiff_t  i;

  if (! check_source (src, error))
    return 0;
  if (! out || out->w != src->w || out->h != src->h || out->c != src->c)
    {
      fz_set_error (error, "the result has the wrong size");
      return 0;
    }
  n = (size_t) src->w * src->h;
  if (alpha)
    memset (alpha, 0, n * sizeof (float));

  sum = fz_alloc_floats (n, 1);
  if (! sum || ! scratch_init (&s, src))
    goto oom;

  /* pass 1: the sum of the weights */
  for (k = 0; k < src->n; k++)
    {
      if (! load (src, k, &s, error))
        goto done;
      if (! fz_exposure_weights (s.img, s.valid, p, s.weight))
        goto oom;
#pragma omp parallel for schedule(static)
      for (i = 0; i < (ptrdiff_t) n; i++)
        sum[i] += s.weight[i];
      update_alpha (alpha, s.valid, n);
      fz_report (prog, 0.3 * (k + 1) / src->n);
    }

  acc = fz_pyramid_new (src->w, src->h, src->c, p->levels);
  wsum = acc ? fz_pyramid_new (src->w, src->h, 1, acc->levels) : NULL;
  if (! wsum)
    goto oom;

  /* pass 2: blend */
  for (k = 0; k < src->n; k++)
    {
      if (! load (src, k, &s, error))
        goto done;
      if (! fz_exposure_weights (s.img, s.valid, p, s.weight))
        goto oom;
#pragma omp parallel for schedule(static)
      for (i = 0; i < (ptrdiff_t) n; i++)
        s.weight[i] = sum[i] > 0.0f ? s.weight[i] / sum[i] : 0.0f;
      if (! fz_pyramid_accumulate (acc, wsum, s.img->p, s.weight))
        goto oom;
      fz_report (prog, 0.3 + 0.65 * (k + 1) / src->n);
    }
  scratch_free (&s);
  memset (&s, 0, sizeof s);
  free (sum);
  sum = NULL;

  fz_pyramid_normalize (acc, wsum);
  fz_pyramid_free (wsum);
  wsum = NULL;
  if (! fz_pyramid_collapse (acc))
    goto oom;
  finish (acc, out, p->clip);
  fz_report (prog, 1.0);
  ok = 1;
  goto done;

oom:
  fz_set_error (error, "not enough memory for %d images of %d x %d",
                src->n, src->w, src->h);
done:
  scratch_free (&s);
  free (sum);
  fz_pyramid_free (acc);
  fz_pyramid_free (wsum);
  if (prog)
    prog->seconds_fuse = fz_now () - t0;
  return ok;
}

/* ------------------------------------------------------ focus measures */

void
fz_focus_params_default (FzFocusParams *p)
{
  p->method = FZ_FOCUS_PYRAMID;
  p->measure = FZ_MEASURE_LAPLACIAN;
  p->noise = 1.0;
  p->window = 2.0;
  p->power = 4.0;
  p->depth_smooth = 4.0;
  p->levels = 0;
  p->clip = 1;
}

int
fz_focus_measure (const FzImage *img, const float *valid,
                  const FzFocusParams *p, float *measure)
{
  int       w = img->w, h = img->h;
  size_t    n = (size_t) w * h;
  float    *y = fz_alloc_floats (n, 0);
  float    *t = fz_alloc_floats (n, 0);
  int       ok = 0;
  ptrdiff_t i;

  if (! y || ! t)
    goto done;
  fz_luma709 (img, y);
  if (p->noise > 0.0 && ! fz_gauss_blur (y, y, w, h, p->noise))
    goto done;

  if (p->measure == FZ_MEASURE_VARIANCE)
    {
#pragma omp parallel for schedule(static)
      for (i = 0; i < (ptrdiff_t) n; i++)
        t[i] = y[i] * y[i];
      if (! fz_gauss_blur (y, y, w, h, FZ_MAX (p->window, 0.5)) ||
          ! fz_gauss_blur (t, t, w, h, FZ_MAX (p->window, 0.5)))
        goto done;
#pragma omp parallel for schedule(static)
      for (i = 0; i < (ptrdiff_t) n; i++)
        measure[i] = FZ_MAX (t[i] - y[i] * y[i], 0.0f);
    }
  else
    {
      fz_laplacian (y, t, w, h);
#pragma omp parallel for schedule(static)
      for (i = 0; i < (ptrdiff_t) n; i++)
        t[i] = t[i] * t[i];
      if (! fz_gauss_blur (t, measure, w, h, p->window))
        goto done;
    }
  if (valid)
    {
#pragma omp parallel for schedule(static)
      for (i = 0; i < (ptrdiff_t) n; i++)
        if (! (valid[i] >= 0.5f) || ! (measure[i] == measure[i]))
          measure[i] = -1.0f;
    }
  ok = 1;
done:
  free (y);
  free (t);
  return ok;
}

/* ------------------------------------------------------- focus stacking */

typedef struct
{
  float   *best;      /* the best measure so far, -1: none */
  int32_t *idx;       /* its image, -1: none */
  float   *left;      /* the measure of image idx - 1 there (NAN: none) */
  float   *right;     /* and of idx + 1 */
  float   *prev;      /* the measure of the previous image */
  float   *sum;       /* soft: sum of (m / best)^power */
} Pick;

static void
pick_free (Pick *pk)
{
  free (pk->best);
  free (pk->idx);
  free (pk->left);
  free (pk->right);
  free (pk->prev);
  free (pk->sum);
}

static int
pick_init (Pick *pk, size_t n, int depth, int soft)
{
  size_t i;

  memset (pk, 0, sizeof *pk);
  pk->best = fz_alloc_floats (n, 0);
  pk->idx = malloc (n * sizeof (int32_t));
  if (depth)
    {
      pk->left = fz_alloc_floats (n, 0);
      pk->right = fz_alloc_floats (n, 0);
      pk->prev = fz_alloc_floats (n, 0);
    }
  if (soft)
    pk->sum = fz_alloc_floats (n, 1);
  if (! pk->best || ! pk->idx || (depth && (! pk->left || ! pk->right || ! pk->prev)) ||
      (soft && ! pk->sum))
    {
      pick_free (pk);
      return 0;
    }
  for (i = 0; i < n; i++)
    {
      pk->best[i] = -1.0f;
      pk->idx[i] = -1;
      if (depth)
        pk->left[i] = pk->right[i] = NAN;
    }
  return 1;
}

static void
pick_update (Pick *pk, const float *m, int k, size_t n, float pw)
{
  ptrdiff_t i;

#pragma omp parallel for schedule(static)
  for (i = 0; i < (ptrdiff_t) n; i++)
    {
      float v = m[i];

      if (v > pk->best[i])
        {
          if (pk->sum)
            {
              /* the running sum of (m / best)^power, rescaled to the new
               * best */
              if (pk->best[i] >= 0.0f)
                pk->sum[i] = pk->sum[i] * power (pk->best[i] / v, pw) + 1.0f;
              else
                pk->sum[i] = 1.0f;
            }
          if (pk->left)
            {
              pk->left[i] = k > 0 ? pk->prev[i] : NAN;
              pk->right[i] = NAN;
            }
          pk->best[i] = v;
          pk->idx[i] = k;
        }
      else
        {
          if (pk->sum && v >= 0.0f)
            pk->sum[i] += pk->best[i] > 0.0f ? power (v / pk->best[i], pw) : 1.0f;
          if (pk->left && pk->idx[i] == k - 1 && k > 0)
            pk->right[i] = v;
        }
    }
  if (pk->prev)
    memcpy (pk->prev, m, n * sizeof (float));
}

/* the depth map from the picks: index plus a parabola's offset, then
 * smoothed with the best measure as confidence */
static int
make_depth (const Pick *pk, int w, int h, double smooth, float *depth)
{
  size_t    n = (size_t) w * h;
  ptrdiff_t i;

#pragma omp parallel for schedule(static)
  for (i = 0; i < (ptrdiff_t) n; i++)
    {
      float d = (float) pk->idx[i];

      if (pk->idx[i] >= 0 && pk->left && pk->left[i] == pk->left[i] &&
          pk->right[i] == pk->right[i])
        {
          float l = FZ_MAX (pk->left[i], 0.0f), c = pk->best[i];
          float r = FZ_MAX (pk->right[i], 0.0f);
          float den = l - 2.0f * c + r;

          if (den < 0.0f)
            d += FZ_CLAMP (0.5f * (l - r) / den, -0.5f, 0.5f);
        }
      depth[i] = d;
    }
  if (smooth > 0.0)
    {
      float *num = fz_alloc_floats (n, 0), *den = fz_alloc_floats (n, 0);

      if (! num || ! den)
        {
          free (num);
          free (den);
          return 0;
        }
#pragma omp parallel for schedule(static)
      for (i = 0; i < (ptrdiff_t) n; i++)
        {
          float c = pk->idx[i] >= 0 ? FZ_MAX (pk->best[i], 0.0f) : 0.0f;

          /* the confidence is the sharpness itself (its square root left
           * the specks of shadows and flat gaps, which no layer shows
           * sharply, in the map); a floor keeps flat areas from being left
           * without any */
          c = c + 1e-12f * (pk->idx[i] >= 0);
          num[i] = c * depth[i];
          den[i] = c;
        }
      if (! fz_gauss_blur (num, num, w, h, smooth) || ! fz_gauss_blur (den, den, w, h, smooth))
        {
          free (num);
          free (den);
          return 0;
        }
#pragma omp parallel for schedule(static)
      for (i = 0; i < (ptrdiff_t) n; i++)
        depth[i] = pk->idx[i] < 0 ? -1.0f : den[i] > 0.0f ? num[i] / den[i] : depth[i];
      free (num);
      free (den);
    }
  else
    {
#pragma omp parallel for schedule(static)
      for (i = 0; i < (ptrdiff_t) n; i++)
        if (pk->idx[i] < 0)
          depth[i] = -1.0f;
    }
  return 1;
}

/* the coefficient method: one pass, selecting per level by local energy */
typedef struct
{
  FzPyramid *acc;      /* selected coefficients (and summed residual) */
  FzPyramid *sal;      /* best salience per level, one channel */
  float     *rw;       /* weight sum of the residual */
} Coeff;

static int
coeff_add (Coeff *cf, float *img, float *valid)
{
  FzPyramid *acc = cf->acc;
  float     *g = img, *v = valid;
  int        l, ch, c = acc->c, ok = 1, own = 0;
  ptrdiff_t  i;

  for (l = 0; l < acc->levels; l++)
    {
      int    w = acc->w[l], h = acc->h[l];
      size_t n = (size_t) w * h;
      float *g1 = NULL, *v1 = NULL;

      if (l < acc->levels - 1)
        {
          int    w2 = acc->w[l + 1], h2 = acc->h[l + 1];
          size_t n2 = (size_t) w2 * h2;
          float *s;

          g1 = fz_alloc_floats (n2 * c, 0);
          v1 = fz_alloc_floats (n2, 0);
          s = fz_alloc_floats (n, 1);
          if (! g1 || ! v1 || ! s)
            {
              free (g1);
              free (v1);
              free (s);
              ok = 0;
              break;
            }
          for (ch = 0; ch < c; ch++)
            {
              fz_downsample (g + ch * n, w, h, g1 + ch * n2);
              if (! fz_upsample_add (g1 + ch * n2, w2, h2, g + ch * n, w, h, -1.0f))
                ok = 0;
            }
          fz_downsample (v, w, h, v1);
          /* salience: the local energy of the band, over the channels */
          for (ch = 0; ch < c; ch++)
            {
              const float *b = g + ch * n;

#pragma omp parallel for schedule(static)
              for (i = 0; i < (ptrdiff_t) n; i++)
                s[i] += b[i] * b[i];
            }
          if (ok && ! fz_gauss_blur (s, s, w, h, 1.0))
            ok = 0;
          if (ok)
            {
              float *best = cf->sal->lev[l];

#pragma omp parallel for schedule(static)
              for (i = 0; i < (ptrdiff_t) n; i++)
                if (v[i] >= 0.5f && s[i] > best[i])
                  {
                    int cc;

                    best[i] = s[i];
                    for (cc = 0; cc < c; cc++)
                      acc->lev[l][cc * n + i] = g[cc * n + i];
                  }
            }
          free (s);
        }
      else
        {
          /* the residual: averaged over the images that are there */
#pragma omp parallel for schedule(static)
          for (i = 0; i < (ptrdiff_t) n; i++)
            {
              float vv = FZ_CLAMP (v[i], 0.0f, 1.0f);
              int   cc;

              cf->rw[i] += vv;
              for (cc = 0; cc < c; cc++)
                acc->lev[l][cc * n + i] += vv * g[cc * n + i];
            }
        }
      if (own)
        {
          free (g);
          free (v);
        }
      g = g1;
      v = v1;
      own = 1;
      if (! ok)
        break;
    }
  if (own)
    {
      free (g);
      free (v);
    }
  return ok;
}

int
fz_focus_stack (const FzSource *src, const FzFocusParams *p,
                FzImage *out, float *alpha, float *depth,
                FzProgress *prog, char **error)
{
  Scratch    s = { 0 };
  Pick       pk = { 0 };
  Coeff      cf = { 0 };
  FzPyramid *acc = NULL, *wsum = NULL;
  float     *m = NULL;
  size_t     n;
  int        k, ok = 0, have_pick = 0;
  int        two_pass = p->method != FZ_FOCUS_COEFF;
  float      pw = (float) FZ_MAX (p->power, 0.0);
  double     t0 = fz_now ();
  ptrdiff_t  i;

  if (! check_source (src, error))
    return 0;
  if (! out || out->w != src->w || out->h != src->h || out->c != src->c)
    {
      fz_set_error (error, "the result has the wrong size");
      return 0;
    }
  n = (size_t) src->w * src->h;
  if (alpha)
    memset (alpha, 0, n * sizeof (float));
  memset (out->p, 0, n * out->c * sizeof (float));

  m = fz_alloc_floats (n, 0);
  if (! m || ! scratch_init (&s, src))
    goto oom;
  if (! pick_init (&pk, n, depth != NULL, p->method == FZ_FOCUS_SOFT))
    goto oom;
  have_pick = 1;

  if (p->method == FZ_FOCUS_COEFF)
    {
      cf.acc = fz_pyramid_new (src->w, src->h, src->c, p->levels);
      if (cf.acc)
        cf.sal = fz_pyramid_new (src->w, src->h, 1, cf.acc->levels);
      if (cf.sal)
        cf.rw = fz_alloc_floats ((size_t) cf.acc->w[cf.acc->levels - 1] *
                                 cf.acc->h[cf.acc->levels - 1], 1);
      if (! cf.rw)
        goto oom;
      for (k = 0; k < cf.sal->levels; k++)
        for (i = 0; i < (ptrdiff_t) ((size_t) cf.sal->w[k] * cf.sal->h[k]); i++)
          cf.sal->lev[k][i] = -1.0f;
    }

  /* pass 1: the measures (the coefficient method blends here too) */
  for (k = 0; k < src->n; k++)
    {
      if (! load (src, k, &s, error))
        goto done;
      if (! fz_focus_measure (s.img, s.valid, p, m))
        goto oom;
      pick_update (&pk, m, k, n, pw);
      update_alpha (alpha, s.valid, n);
      if (p->method == FZ_FOCUS_COEFF && ! coeff_add (&cf, s.img->p, s.valid))
        goto oom;
      fz_report (prog, (two_pass ? 0.4 : 0.95) * (k + 1) / src->n);
    }
  free (m);
  m = NULL;

  if (p->method == FZ_FOCUS_COEFF)
    {
      int    L = cf.acc->levels - 1;
      size_t nr = (size_t) cf.acc->w[L] * cf.acc->h[L];
      int    ch;

      for (ch = 0; ch < src->c; ch++)
        for (i = 0; i < (ptrdiff_t) nr; i++)
          cf.acc->lev[L][ch * nr + i] = cf.rw[i] > 0.0f ?
            cf.acc->lev[L][ch * nr + i] / cf.rw[i] : 0.0f;
      if (! fz_pyramid_collapse (cf.acc))
        goto oom;
      finish (cf.acc, out, p->clip);
    }
  else if (p->method == FZ_FOCUS_PIXELS)
    {
      for (k = 0; k < src->n; k++)
        {
          int ch;

          if (! load (src, k, &s, error))
            goto done;
          for (ch = 0; ch < src->c; ch++)
            {
              float       *o = fz_image_plane (out, ch);
              const float *v = fz_image_plane (s.img, ch);

#pragma omp parallel for schedule(static)
              for (i = 0; i < (ptrdiff_t) n; i++)
                if (pk.idx[i] == k)
                  o[i] = v[i];
            }
          fz_report (prog, 0.4 + 0.55 * (k + 1) / src->n);
        }
      if (p->clip)
        for (i = 0; i < (ptrdiff_t) (n * out->c); i++)
          out->p[i] = FZ_CLAMP (out->p[i], 0.0f, 1.0f);
    }
  else
    {
      acc = fz_pyramid_new (src->w, src->h, src->c, p->levels);
      wsum = acc ? fz_pyramid_new (src->w, src->h, 1, acc->levels) : NULL;
      if (! wsum)
        goto oom;
      for (k = 0; k < src->n; k++)
        {
          if (! load (src, k, &s, error))
            goto done;
          if (p->method == FZ_FOCUS_SOFT)
            {
              if (! fz_focus_measure (s.img, s.valid, p, s.weight))
                goto oom;
#pragma omp parallel for schedule(static)
              for (i = 0; i < (ptrdiff_t) n; i++)
                {
                  float v = s.weight[i], b = pk.best[i];
                  float w;

                  if (v < 0.0f || pk.idx[i] < 0)
                    w = 0.0f;
                  else if (b > 0.0f)
                    w = power (v / b, pw);
                  else
                    w = 1.0f;
                  s.weight[i] = pk.sum[i] > 0.0f ? w / pk.sum[i] : 0.0f;
                }
            }
          else
            {
#pragma omp parallel for schedule(static)
              for (i = 0; i < (ptrdiff_t) n; i++)
                s.weight[i] = pk.idx[i] == k ? 1.0f : 0.0f;
            }
          if (! fz_pyramid_accumulate (acc, wsum, s.img->p, s.weight))
            goto oom;
          fz_report (prog, 0.4 + 0.55 * (k + 1) / src->n);
        }
      scratch_free (&s);
      memset (&s, 0, sizeof s);
      fz_pyramid_normalize (acc, wsum);
      if (! fz_pyramid_collapse (acc))
        goto oom;
      finish (acc, out, p->clip);
    }

  if (depth && ! make_depth (&pk, src->w, src->h, p->depth_smooth, depth))
    goto oom;
  fz_report (prog, 1.0);
  ok = 1;
  goto done;

oom:
  fz_set_error (error, "not enough memory for %d images of %d x %d",
                src->n, src->w, src->h);
done:
  scratch_free (&s);
  if (have_pick)
    pick_free (&pk);
  free (m);
  fz_pyramid_free (acc);
  fz_pyramid_free (wsum);
  fz_pyramid_free (cf.acc);
  fz_pyramid_free (cf.sal);
  free (cf.rw);
  if (prog)
    prog->seconds_fuse = fz_now () - t0;
  return ok;
}
