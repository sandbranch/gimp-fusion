/* gimp-fusion core: aligning the images of a stack
 *
 * Each image is aligned to its neighbour towards the reference, and the
 * transforms are chained, so that neighbours (similar in exposure or in
 * focus) are compared, never the two ends of a long stack.
 *
 * A pair: Gaussian pyramids of both images' luma; phase correlation
 * (Kuglin and Hines 1975) on a level of at most 512 pixels a side gives
 * the shift, robust to changes of brightness and contrast because only the
 * phase counts; then Gauss-Newton (Lucas and Kanade 1981), coarse to fine,
 * refines the shift and, by the model, the scale and the rotation. It
 * compares locally normalized images ((I - mean) / local standard
 * deviation over a few pixels), with a gain and an offset, so that other
 * exposures and other focus still compare, and leaves out pixels that are
 * clipped or missing; residuals are weighed with Huber's function.
 *
 * Copyright 2026 David
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "fz-private.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------ transforms */

void
fz_transform_identity (FzTransform *t)
{
  t->a = 1.0;
  t->b = 0.0;
  t->tx = 0.0;
  t->ty = 0.0;
}

void
fz_transform_apply (const FzTransform *t, double cx, double cy,
                    double x, double y, double *xo, double *yo)
{
  double dx = x - cx, dy = y - cy;

  *xo = cx + t->a * dx - t->b * dy + t->tx;
  *yo = cy + t->b * dx + t->a * dy + t->ty;
}

void
fz_transform_compose (const FzTransform *t1, const FzTransform *t2,
                      FzTransform *out)
{
  /* t1 (t2 (p)) with p' = c + M2 (p - c) + t2: M1 M2 (p - c) + M1 t2 + t1 */
  FzTransform r;

  r.a = t1->a * t2->a - t1->b * t2->b;
  r.b = t1->b * t2->a + t1->a * t2->b;
  r.tx = t1->a * t2->tx - t1->b * t2->ty + t1->tx;
  r.ty = t1->b * t2->tx + t1->a * t2->ty + t1->ty;
  *out = r;
}

void
fz_transform_invert (const FzTransform *t, FzTransform *out)
{
  double      d = t->a * t->a + t->b * t->b;
  FzTransform r;

  if (d <= 0.0)
    {
      fz_transform_identity (out);
      return;
    }
  /* M^-1 = (a, b; -b, a) / d */
  r.a = t->a / d;
  r.b = -t->b / d;
  r.tx = -(r.a * t->tx - r.b * t->ty);
  r.ty = -(r.b * t->tx + r.a * t->ty);
  *out = r;
}

static int
is_identity (const FzTransform *t)
{
  return t->a == 1.0 && t->b == 0.0 && t->tx == 0.0 && t->ty == 0.0;
}

/* ------------------------------------------------------ phase correlation */

static int
next_pow2 (int v)
{
  int p = 1;

  while (p < v)
    p <<= 1;
  return p;
}

int
fz_phase_correlate (const float *a, const float *b, int w, int h,
                    double *dx, double *dy, double *peak)
{
  int     W = next_pow2 (w), H = next_pow2 (h);
  size_t  N = (size_t) W * H;
  double *ar = calloc (N, sizeof (double)), *ai = calloc (N, sizeof (double));
  double *br = calloc (N, sizeof (double)), *bi = calloc (N, sizeof (double));
  double *wx = malloc (sizeof (double) * w), *wy = malloc (sizeof (double) * h);
  double  ma = 0.0, mb = 0.0, best = -1e300;
  int     x, y, px = 0, py = 0, ok = 0;
  size_t  i;

  if (! ar || ! ai || ! br || ! bi || ! wx || ! wy)
    goto done;
  /* a Hann window over the image, the means taken out, zero padding */
  for (x = 0; x < w; x++)
    wx[x] = w > 1 ? 0.5 - 0.5 * cos (2.0 * M_PI * (x + 0.5) / w) : 1.0;
  for (y = 0; y < h; y++)
    wy[y] = h > 1 ? 0.5 - 0.5 * cos (2.0 * M_PI * (y + 0.5) / h) : 1.0;
  for (i = 0; i < (size_t) w * h; i++)
    {
      ma += a[i];
      mb += b[i];
    }
  ma /= (double) w * h;
  mb /= (double) w * h;
  for (y = 0; y < h; y++)
    for (x = 0; x < w; x++)
      {
        double win = wx[x] * wy[y];

        ar[(size_t) y * W + x] = (a[(size_t) y * w + x] - ma) * win;
        br[(size_t) y * W + x] = (b[(size_t) y * w + x] - mb) * win;
      }
  if (! fz_fft2 (ar, ai, W, H, 0) || ! fz_fft2 (br, bi, W, H, 0))
    goto done;
  /* the normalized cross power spectrum conj (A) B: its inverse peaks at
   * the shift of b against a */
  for (i = 0; i < N; i++)
    {
      double re = ar[i] * br[i] + ai[i] * bi[i];
      double im = ar[i] * bi[i] - ai[i] * br[i];
      double m = sqrt (re * re + im * im);

      if (m > 1e-20)
        {
          ar[i] = re / m;
          ai[i] = im / m;
        }
      else
        ar[i] = ai[i] = 0.0;
    }
  if (! fz_fft2 (ar, ai, W, H, 1))
    goto done;
  for (y = 0; y < H; y++)
    for (x = 0; x < W; x++)
      if (ar[(size_t) y * W + x] > best)
        {
          best = ar[(size_t) y * W + x];
          px = x;
          py = y;
        }
  {
    /* the peak's position to a fraction of a pixel: the peak of phase
     * correlation is a sinc, whose samples c0 at the peak and c1 at the
     * larger neighbour give the offset c1 / (c1 + c0) (Foroosh, Zerubia
     * and Berthod 2002); a parabola would pull it towards the pixel */
    double c = best;
    double l = ar[(size_t) py * W + (px + W - 1) % W];
    double r = ar[(size_t) py * W + (px + 1) % W];
    double u = ar[(size_t) ((py + H - 1) % H) * W + px];
    double d = ar[(size_t) ((py + 1) % H) * W + px];
    double fx = 0.0, fy = 0.0;

    if (c > 0.0)
      {
        if (r > l && r > 0.0)
          fx = r / (r + c);
        else if (l > 0.0)
          fx = -l / (l + c);
        if (d > u && d > 0.0)
          fy = d / (d + c);
        else if (u > 0.0)
          fy = -u / (u + c);
      }
    *dx = (px > W / 2 ? px - W : px) + fx;
    *dy = (py > H / 2 ? py - H : py) + fy;
    *peak = FZ_CLAMP (best / (double) N, 0.0, 1.0);
  }
  ok = 1;
done:
  free (ar);
  free (ai);
  free (br);
  free (bi);
  free (wx);
  free (wy);
  return ok;
}

/* ---------------------------------------------------------------- pyramid */

typedef struct
{
  int    levels;
  int    w[FZ_MAX_LEVELS], h[FZ_MAX_LEVELS];
  float *p[FZ_MAX_LEVELS];      /* level 0 is not ours */
  float *m[FZ_MAX_LEVELS];      /* masks, 1 or 0; level 0 ours */
} Pyr;

static void
pyr_free (Pyr *p)
{
  int l;

  for (l = 0; l < p->levels; l++)
    {
      if (l > 0)
        free (p->p[l]);
      free (p->m[l]);
    }
  memset (p, 0, sizeof *p);
}

static int
pyr_build (Pyr *p, const float *img, const float *mask, int w, int h)
{
  int    l;
  size_t n = (size_t) w * h, i;

  memset (p, 0, sizeof *p);
  p->p[0] = (float *) img;
  p->w[0] = w;
  p->h[0] = h;
  p->m[0] = fz_alloc_floats (n, 0);
  p->levels = 1;
  if (! p->m[0])
    return 0;
  for (i = 0; i < n; i++)
    p->m[0][i] = ! mask || mask[i] >= 0.5f ? 1.0f : 0.0f;
  for (l = 1; l < FZ_MAX_LEVELS; l++)
    {
      int w2 = (p->w[l - 1] + 1) / 2, h2 = (p->h[l - 1] + 1) / 2;

      if (FZ_MIN (w2, h2) < 16)
        break;
      p->w[l] = w2;
      p->h[l] = h2;
      p->p[l] = fz_alloc_floats ((size_t) w2 * h2, 0);
      p->m[l] = fz_alloc_floats ((size_t) w2 * h2, 0);
      p->levels = l + 1;
      if (! p->p[l] || ! p->m[l])
        return 0;
      fz_downsample (p->p[l - 1], p->w[l - 1], p->h[l - 1], p->p[l]);
      fz_downsample (p->m[l - 1], p->w[l - 1], p->h[l - 1], p->m[l]);
      /* a coarse pixel counts if most of its footprint did */
      for (i = 0; i < (size_t) w2 * h2; i++)
        p->m[l][i] = p->m[l][i] > 0.75f ? 1.0f : 0.0f;
    }
  return 1;
}

/* (I - G I) / sqrt (G (I - G I)^2 + eps^2), eps a tenth of the mean
 * local contrast */
static float *
local_normalize (const float *src, int w, int h, double sigma, int normalize)
{
  size_t    n = (size_t) w * h;
  float    *d = fz_alloc_floats (n, 0), *v = fz_alloc_floats (n, 0);
  double    mean = 0.0;
  float     eps2;
  ptrdiff_t i;

  /* a slight blur first: interpolating what is left of the finest
   * detail would pull the result towards whole pixels */
  if (! d || ! v || ! fz_gauss_blur (src, v, w, h, 1.0) ||
      ! fz_gauss_blur (v, d, w, h, sigma))
    {
      free (d);
      free (v);
      return NULL;
    }
  if (! normalize)
    {
      /* the plain image, with the slight blur */
      free (d);
      return v;
    }
  for (i = 0; i < (ptrdiff_t) n; i++)
    {
      d[i] = v[i] - d[i];
      v[i] = d[i] * d[i];
    }
  if (! fz_gauss_blur (v, v, w, h, sigma))
    {
      free (d);
      free (v);
      return NULL;
    }
  for (i = 0; i < (ptrdiff_t) n; i++)
    mean += v[i];
  mean /= (double) n;
  eps2 = (float) (0.01 * mean) + 1e-30f;
#pragma omp parallel for schedule(static)
  for (i = 0; i < (ptrdiff_t) n; i++)
    d[i] = d[i] / sqrtf (v[i] + eps2);
  free (v);
  return d;
}

/* the pixels to compare: valid themselves, in a neighbourhood that is
 * nearly all valid (the local normalization there saw the same kind of
 * pixels as its counterpart in the other image), and at least margin
 * pixels from the image's edge. Scattered masked pixels cost only
 * themselves; a masked area also its surroundings. */
static float *
neighbourhood_mask (const float *m, int w, int h, double sigma, int margin)
{
  size_t    n = (size_t) w * h;
  float    *o = fz_alloc_floats (n, 0);
  ptrdiff_t y;

  if (! o || ! fz_gauss_blur (m, o, w, h, sigma))
    {
      free (o);
      return NULL;
    }
#pragma omp parallel for schedule(static)
  for (y = 0; y < h; y++)
    {
      int x;

      for (x = 0; x < w; x++)
        {
          size_t i = (size_t) y * w + x;
          int    in = x >= margin && x < w - margin && y >= margin && y < h - margin;

          o[i] = in && m[i] >= 0.5f && o[i] >= 0.95f ? 1.0f : 0.0f;
        }
    }
  return o;
}

static inline float
bilinear (const float *p, int w, int h, float x, float y, int *inside)
{
  int   x0, y0;
  float fx, fy;

  if (x < 0.0f || y < 0.0f || x > (float) (w - 1) || y > (float) (h - 1))
    {
      *inside = 0;
      return 0.0f;
    }
  x0 = (int) x;
  y0 = (int) y;
  if (x0 >= w - 1)
    x0 = w - 2 < 0 ? 0 : w - 2;
  if (y0 >= h - 1)
    y0 = h - 2 < 0 ? 0 : h - 2;
  fx = x - x0;
  fy = y - y0;
  *inside = 1;
  {
    const float *r0 = p + (size_t) y0 * w + x0;
    const float *r1 = h > 1 ? r0 + w : r0;
    float        a = r0[0], b = w > 1 ? r0[1] : r0[0];
    float        c = r1[0], d = w > 1 ? r1[1] : r1[0];

    return (a + (b - a) * fx) * (1.0f - fy) + (c + (d - c) * fx) * fy;
  }
}

/* Catmull-Rom, edges repeated; *inside as bilinear () */
static inline float
bicubic (const float *p, int w, int h, float x, float y, int *inside)
{
  int   x0, y0, i, j;
  float fx, fy, wx[4], wy[4], acc = 0.0f;

  if (x < 0.0f || y < 0.0f || x > (float) (w - 1) || y > (float) (h - 1))
    {
      *inside = 0;
      return 0.0f;
    }
  *inside = 1;
  x0 = (int) x;
  y0 = (int) y;
  fx = x - x0;
  fy = y - y0;
  for (i = 0; i < 4; i++)
    {
      float d = fabsf (fx - (i - 1)), e = fabsf (fy - (i - 1));

      wx[i] = d < 1.0f ? (1.5f * d - 2.5f) * d * d + 1.0f
                       : d < 2.0f ? ((-0.5f * d + 2.5f) * d - 4.0f) * d + 2.0f : 0.0f;
      wy[i] = e < 1.0f ? (1.5f * e - 2.5f) * e * e + 1.0f
                       : e < 2.0f ? ((-0.5f * e + 2.5f) * e - 4.0f) * e + 2.0f : 0.0f;
    }
  for (j = 0; j < 4; j++)
    {
      const float *row = p + (size_t) FZ_CLAMP (y0 - 1 + j, 0, h - 1) * w;
      float        r = 0.0f;

      for (i = 0; i < 4; i++)
        r += wx[i] * row[FZ_CLAMP (x0 - 1 + i, 0, w - 1)];
      acc += wy[j] * r;
    }
  return acc;
}

/* ------------------------------------------------------------ Gauss-Newton */

#define NP_MAX 6   /* tx, ty, a, b, gain, offset */

static int
solve (double A[NP_MAX][NP_MAX], double *bv, int n, double *x)
{
  double M[NP_MAX][NP_MAX + 1];
  int    i, j, k;

  for (i = 0; i < n; i++)
    {
      for (j = 0; j < n; j++)
        M[i][j] = A[i][j];
      M[i][n] = bv[i];
    }
  for (i = 0; i < n; i++)
    {
      int    piv = i;
      double m = fabs (M[i][i]);

      for (k = i + 1; k < n; k++)
        if (fabs (M[k][i]) > m)
          {
            m = fabs (M[k][i]);
            piv = k;
          }
      if (m < 1e-30)
        return 0;
      if (piv != i)
        for (j = 0; j <= n; j++)
          {
            double t = M[i][j];

            M[i][j] = M[piv][j];
            M[piv][j] = t;
          }
      for (k = i + 1; k < n; k++)
        {
          double f = M[k][i] / M[i][i];

          for (j = i; j <= n; j++)
            M[k][j] -= f * M[i][j];
        }
    }
  for (i = n - 1; i >= 0; i--)
    {
      double s = M[i][n];

      for (j = i + 1; j < n; j++)
        s -= M[i][j] * x[j];
      x[i] = s / M[i][i];
    }
  return 1;
}

typedef struct
{
  double H[NP_MAX][NP_MAX];
  double g[NP_MAX];
  double saa, sbb, sab, cnt;
} Acc;

/* one level: refines t (in full resolution units) */
static int
refine_level (const float *na, const float *ma, const float *nb, const float *mb,
              int w, int h, double scale, double cx, double cy,
              FzAlignModel model, int iters, FzTransform *t, double *ncc)
{
  size_t n = (size_t) w * h;
  float *gx = fz_alloc_floats (n, 0), *gy = fz_alloc_floats (n, 0);
  float *hx = fz_alloc_floats (n, 0), *hy = fz_alloc_floats (n, 0);
  Acc   *rows = calloc ((size_t) h, sizeof (Acc));
  int    np = model == FZ_ALIGN_SIMILARITY ? 4 : model == FZ_ALIGN_SCALE ? 3 : 2;
  int    stride = n > (size_t) 4000000 ? 2 : 1;
  double gain = 1.0, offset = 0.0;
  double lcx = cx / scale, lcy = cy / scale;
  double radius = 0.5 * sqrt ((double) w * w + (double) h * h);
  int    it;
  ptrdiff_t y;

  if (! gx || ! gy || ! hx || ! hy || ! rows)
    {
      free (gx);
      free (gy);
      free (hx);
      free (hy);
      free (rows);
      return 0;
    }
#pragma omp parallel for schedule(static)
  for (y = 0; y < h; y++)
    {
      int x;

      for (x = 0; x < w; x++)
        {
          size_t i = (size_t) y * w + x;

          gx[i] = 0.5f * (nb[(size_t) y * w + FZ_MIN (x + 1, w - 1)]
                          - nb[(size_t) y * w + FZ_MAX (x - 1, 0)]);
          gy[i] = 0.5f * (nb[(size_t) FZ_MIN (y + 1, h - 1) * w + x]
                          - nb[(size_t) FZ_MAX ((int) y - 1, 0) * w + x]);
          hx[i] = 0.5f * (na[(size_t) y * w + FZ_MIN (x + 1, w - 1)]
                          - na[(size_t) y * w + FZ_MAX (x - 1, 0)]);
          hy[i] = 0.5f * (na[(size_t) FZ_MIN (y + 1, h - 1) * w + x]
                          - na[(size_t) FZ_MAX ((int) y - 1, 0) * w + x]);
        }
    }

  for (it = 0; it < iters; it++)
    {
      Acc    tot;
      double A[NP_MAX][NP_MAX], bv[NP_MAX], dp[NP_MAX];
      int    q = np + 2, i, j;
      double la = t->a, lb = t->b, ltx = t->tx / scale, lty = t->ty / scale;
      double step;

      memset (rows, 0, (size_t) h * sizeof (Acc));
#pragma omp parallel for schedule(static)
      for (y = 0; y < h; y += stride)
        {
          Acc   *acc = rows + y;
          double ry = y - lcy;
          int    x;

          for (x = 0; x < w; x += stride)
            {
              size_t i0 = (size_t) y * w + x;
              double rx = x - lcx;
              float  xs, ys, vb, vgx, vgy, vm;
              int    in1, in2, in3, in4;
              double r, wgt, J[NP_MAX], ar;
              int    k, m;

              if (ma[i0] < 0.5f)
                continue;
              xs = (float) (lcx + la * rx - lb * ry + ltx);
              ys = (float) (lcy + lb * rx + la * ry + lty);
              vm = bilinear (mb, w, h, xs, ys, &in1);
              if (! in1 || vm < 0.99f)
                continue;
              vb = bicubic (nb, w, h, xs, ys, &in2);
              /* ESM (Benhimane and Malis 2004): the mean of both images'
               * gradients converges faster and without the bias of
               * interpolated gradients alone */
              vgx = bilinear (gx, w, h, xs, ys, &in3);
              vgy = bilinear (gy, w, h, xs, ys, &in4);
              vgx = 0.5f * (vgx + hx[i0]);
              vgy = 0.5f * (vgy + hy[i0]);
              ar = na[i0];
              r = gain * vb + offset - ar;
              /* Huber: quadratic up to 1 (in units of local contrast) */
              wgt = fabs (r) <= 1.0 ? 1.0 : 1.0 / fabs (r);
              J[0] = gain * vgx;
              J[1] = gain * vgy;
              if (np >= 3)
                J[2] = gain * (vgx * rx + vgy * ry);
              if (np >= 4)
                J[3] = gain * (-vgx * ry + vgy * rx);
              J[np] = vb;
              J[np + 1] = 1.0;
              for (k = 0; k < np + 2; k++)
                {
                  acc->g[k] += wgt * J[k] * r;
                  for (m = k; m < np + 2; m++)
                    acc->H[k][m] += wgt * J[k] * J[m];
                }
              acc->saa += ar * ar;
              acc->sbb += (double) vb * vb;
              acc->sab += ar * vb;
              acc->cnt += 1.0;
            }
        }
      /* summed in order: the same result with any number of threads */
      memset (&tot, 0, sizeof tot);
      for (y = 0; y < h; y++)
        {
          for (i = 0; i < q; i++)
            {
              tot.g[i] += rows[y].g[i];
              for (j = i; j < q; j++)
                tot.H[i][j] += rows[y].H[i][j];
            }
          tot.saa += rows[y].saa;
          tot.sbb += rows[y].sbb;
          tot.sab += rows[y].sab;
          tot.cnt += rows[y].cnt;
        }
      if (tot.cnt < 50.0)
        break;
      *ncc = tot.saa > 0.0 && tot.sbb > 0.0 ? tot.sab / sqrt (tot.saa * tot.sbb) : 0.0;
      for (i = 0; i < q; i++)
        {
          for (j = 0; j < q; j++)
            A[i][j] = j >= i ? tot.H[i][j] : tot.H[j][i];
          /* a little damping keeps flat images from running away */
          A[i][i] *= 1.0 + 1e-6;
          A[i][i] += 1e-9;
          bv[i] = -tot.g[i];
        }
      if (! solve (A, bv, q, dp))
        break;
      ltx += dp[0];
      lty += dp[1];
      if (np >= 3)
        la += dp[2];
      if (np >= 4)
        lb += dp[3];
      gain += dp[np];
      offset += dp[np + 1];
      if (! (gain > 0.05 && gain < 20.0))
        gain = 1.0;
      t->a = la;
      t->b = lb;
      t->tx = ltx * scale;
      t->ty = lty * scale;
      /* how far the corners moved, in pixels of this level */
      step = fabs (dp[0]) + fabs (dp[1]);
      if (np >= 3)
        step += fabs (dp[2]) * radius;
      if (np >= 4)
        step += fabs (dp[3]) * radius;
      if (getenv ("FZ_DEBUG") && getenv ("FZ_DEBUG")[0] == '2')
        fprintf (stderr, "  it %d: dp %.5f %.5f %.6f %.6f gain %.4f off %.4f n %.0f\n", it,
                 dp[0], dp[1], np >= 3 ? dp[2] : 0.0, np >= 4 ? dp[3] : 0.0, gain, offset,
                 tot.cnt);
      if (step < 1e-3)
        break;
    }
  free (gx);
  free (gy);
  free (hx);
  free (hy);
  free (rows);
  return 1;
}

int
fz_align_pair (const float *a, const float *mask_a,
               const float *b, const float *mask_b, int w, int h,
               FzAlignModel model, int max_shift, int normalize,
               FzTransform *t, double *confidence)
{
  Pyr    pa, pb;
  int    ok = 0, l, pl;
  double cx = 0.5 * (w - 1), cy = 0.5 * (h - 1);
  double ncc = 0.0, peak = 0.0;
  double limit;

  fz_transform_identity (t);
  if (confidence)
    *confidence = 0.0;
  if (model == FZ_ALIGN_NONE)
    {
      if (confidence)
        *confidence = 1.0;
      return 1;
    }
  if (! pyr_build (&pa, a, mask_a, w, h) || ! pyr_build (&pb, b, mask_b, w, h))
    {
      pyr_free (&pa);
      pyr_free (&pb);
      return 0;
    }
  if (max_shift <= 0)
    max_shift = 25;
  limit = FZ_MIN (w, h) * max_shift / 100.0;

  /* the shift: phase correlation on a level of at most 512 pixels a side,
   * masked pixels set to the mean */
  for (pl = 0; pl < pa.levels - 1 && FZ_MAX (pa.w[pl], pa.h[pl]) > 512; pl++)
    ;
  {
    int    pw = pa.w[pl], ph = pa.h[pl];
    size_t n = (size_t) pw * ph, i;
    float *ca = fz_alloc_floats (n, 0), *cb = fz_alloc_floats (n, 0);
    double sa = 0.0, sb = 0.0, na = 0.0, nb = 0.0, dx, dy;

    if (! ca || ! cb)
      {
        free (ca);
        free (cb);
        goto done;
      }
    for (i = 0; i < n; i++)
      {
        if (pa.m[pl][i] > 0.5f)
          {
            sa += pa.p[pl][i];
            na++;
          }
        if (pb.m[pl][i] > 0.5f)
          {
            sb += pb.p[pl][i];
            nb++;
          }
      }
    sa = na > 0 ? sa / na : 0.0;
    sb = nb > 0 ? sb / nb : 0.0;
    for (i = 0; i < n; i++)
      {
        ca[i] = pa.m[pl][i] > 0.5f ? pa.p[pl][i] : (float) sa;
        cb[i] = pb.m[pl][i] > 0.5f ? pb.p[pl][i] : (float) sb;
      }
    if (fz_phase_correlate (ca, cb, pw, ph, &dx, &dy, &peak))
      {
        double s = (double) (1 << pl);

        if (fabs (dx * s) <= limit && fabs (dy * s) <= limit)
          {
            t->tx = dx * s;
            t->ty = dy * s;
          }
      }
    free (ca);
    free (cb);
  }

  /* refine, coarse to fine, from a level of about 64 pixels */
  for (l = pa.levels - 1; l > 0 && FZ_MIN (pa.w[l], pa.h[l]) < 48; l--)
    ;
  for (; l >= 0; l--)
    {
      float *na = local_normalize (pa.p[l], pa.w[l], pa.h[l], 4.0, normalize);
      float *nb = local_normalize (pb.p[l], pb.w[l], pb.h[l], 4.0, normalize);
      /* near an edge or a masked pixel, the local normalization saw other
       * pixels than its counterpart in the other image: left out */
      int    margin = FZ_MIN (14, FZ_MIN (pa.w[l], pa.h[l]) / 8);
      float *ma = neighbourhood_mask (pa.m[l], pa.w[l], pa.h[l], 4.0, margin);
      float *mb = neighbourhood_mask (pb.m[l], pb.w[l], pb.h[l], 4.0, margin);
      int    r;

      if (! na || ! nb || ! ma || ! mb)
        {
          free (na);
          free (nb);
          free (ma);
          free (mb);
          goto done;
        }
      if (getenv ("FZ_DEBUG"))
        {
          double s0 = 0, s1 = 0, s2 = 0, s3 = 0;
          size_t q;

          for (q = 0; q < (size_t) pa.w[l] * pa.h[l]; q++)
            {
              s0 += pa.m[l][q];
              s1 += ma[q];
              s2 += pb.m[l][q];
              s3 += mb[q];
            }
          fprintf (stderr, "masks level %d: a %.0f -> %.0f, b %.0f -> %.0f of %d\n", l, s0, s1,
                   s2, s3, pa.w[l] * pa.h[l]);
        }
      r = refine_level (na, ma, nb, mb, pa.w[l], pa.h[l],
                        (double) (1 << l), cx, cy, model, l == 0 ? 8 : 30, t, &ncc);
      free (ma);
      free (mb);
      if (getenv ("FZ_DEBUG"))
        fprintf (stderr, "align level %d (%d x %d): a %.6f b %.6f t %.4f %.4f ncc %.4f\n",
                 l, pa.w[l], pa.h[l], t->a, t->b, t->tx, t->ty, ncc);
      free (na);
      free (nb);
      if (! r)
        goto done;
    }
  if (confidence)
    *confidence = FZ_CLAMP (ncc, 0.0, 1.0);
  ok = 1;
done:
  (void) peak;
  pyr_free (&pa);
  pyr_free (&pb);
  return ok;
}

/* ------------------------------------------------------------- the stack */

/* the luma of a loaded image, and a mask of valid, unclipped pixels */
static int
luma_and_mask (const FzImage *img, const float *valid, float *y, float *m)
{
  size_t    n = (size_t) img->w * img->h;
  float     lo = 1e30f, hi = -1e30f, mlo, mhi;
  ptrdiff_t i;

  fz_luma709 (img, y);
  for (i = 0; i < (ptrdiff_t) n; i++)
    if (valid[i] >= 0.5f)
      {
        lo = FZ_MIN (lo, y[i]);
        hi = FZ_MAX (hi, y[i]);
      }
  /* clipped: at the darkest or the brightest value, when that value is
   * an end of the range (0 or 1) and at least 0.2 % of the pixels sit
   * there, as a clipping sensor leaves them; a few pixels at 0 in a dark
   * picture are not clipping */
  {
    size_t nlo = 0, nhi = 0;

    for (i = 0; i < (ptrdiff_t) n; i++)
      if (valid[i] >= 0.5f)
        {
          nlo += y[i] <= lo + 1e-6f;
          nhi += y[i] >= hi - 1e-6f;
        }
    mlo = lo <= 0.0f && nlo > n / 500 ? lo + 1e-6f : -1e30f;
    mhi = hi >= 1.0f && nhi > n / 500 ? hi - 1e-6f : 1e30f;
  }
#pragma omp parallel for schedule(static)
  for (i = 0; i < (ptrdiff_t) n; i++)
    m[i] = valid[i] >= 0.5f && y[i] > mlo && y[i] < mhi ? 1.0f : 0.0f;
  return 1;
}

int
fz_align (const FzSource *src, const FzAlignParams *p, FzTransform *out,
          double *confidence, FzProgress *prog, char **error)
{
  size_t   n;
  int      ref, k, ok = 0, done = 0;
  FzImage *img = NULL;
  float   *valid = NULL, *yr = NULL, *mr = NULL, *yp = NULL, *mp = NULL;
  float   *yk = NULL, *mk = NULL;
  double   t0 = fz_now ();

  if (! src || src->n < 1 || src->w < 1 || src->h < 1 || ! src->load)
    {
      fz_set_error (error, "no images to align");
      return 0;
    }
  for (k = 0; k < src->n; k++)
    {
      fz_transform_identity (&out[k]);
      if (confidence)
        confidence[k] = 1.0;
    }
  if (p->model == FZ_ALIGN_NONE || src->n < 2)
    return 1;
  ref = p->reference >= 0 && p->reference < src->n ? p->reference : src->n / 2;
  n = (size_t) src->w * src->h;

  img = fz_image_new (src->w, src->h, src->c);
  valid = fz_alloc_floats (n, 0);
  yr = fz_alloc_floats (n, 0);
  mr = fz_alloc_floats (n, 0);
  yp = fz_alloc_floats (n, 0);
  mp = fz_alloc_floats (n, 0);
  yk = fz_alloc_floats (n, 0);
  mk = fz_alloc_floats (n, 0);
  if (! img || ! valid || ! yr || ! mr || ! yp || ! mp || ! yk || ! mk)
    {
      fz_set_error (error, "not enough memory to align %d images of %d x %d",
                    src->n, src->w, src->h);
      goto fail;
    }
  if (! src->load (src->data, ref, img, valid, error))
    {
      fz_set_error (error, "could not read image %d of the stack", ref + 1);
      goto fail;
    }
  luma_and_mask (img, valid, yr, mr);

  /* upwards from the reference, then downwards */
  {
    int dir;

    for (dir = 1; dir >= -1; dir -= 2)
      {
        memcpy (yp, yr, n * sizeof (float));
        memcpy (mp, mr, n * sizeof (float));
        for (k = ref + dir; k >= 0 && k < src->n; k += dir)
          {
            FzTransform t;
            double      conf = 0.0;
            float      *sw;

            if (! src->load (src->data, k, img, valid, error))
              {
                fz_set_error (error, "could not read image %d of the stack", k + 1);
                goto fail;
              }
            luma_and_mask (img, valid, yk, mk);
            /* t: positions in image k - dir to positions in image k */
            if (! fz_align_pair (yp, mp, yk, mk, src->w, src->h, p->model,
                                 p->max_shift, p->normalize, &t, &conf))
              {
                fz_set_error (error, "not enough memory to align image %d", k + 1);
                goto fail;
              }
            fz_transform_compose (&t, &out[k - dir], &out[k]);
            if (confidence)
              confidence[k] = conf;
            sw = yp; yp = yk; yk = sw;
            sw = mp; mp = mk; mk = sw;
            done++;
            fz_report (prog, (double) done / (src->n - 1));
          }
      }
  }
  ok = 1;
fail:
  fz_image_free (img);
  free (valid);
  free (yr);
  free (mr);
  free (yp);
  free (mp);
  free (yk);
  free (mk);
  if (prog)
    prog->seconds_align = fz_now () - t0;
  return ok;
}

/* ---------------------------------------------------------------- warping */

static inline float
cubic_w (float x)
{
  /* Catmull-Rom (Keys, a = -0.5) */
  x = fabsf (x);
  if (x < 1.0f)
    return (1.5f * x - 2.5f) * x * x + 1.0f;
  if (x < 2.0f)
    return ((-0.5f * x + 2.5f) * x - 4.0f) * x + 2.0f;
  return 0.0f;
}

void
fz_warp (const FzImage *src, const float *valid_src, const FzTransform *t,
         FzImage *dst, float *valid_dst)
{
  int       w = src->w, h = src->h, c = src->c;
  size_t    n = (size_t) w * h;
  double    cx = 0.5 * (w - 1), cy = 0.5 * (h - 1);
  ptrdiff_t y;

  if (is_identity (t))
    {
      memcpy (dst->p, src->p, n * c * sizeof (float));
      memcpy (valid_dst, valid_src, n * sizeof (float));
      return;
    }
#pragma omp parallel for schedule(static)
  for (y = 0; y < h; y++)
    {
      int x;

      for (x = 0; x < w; x++)
        {
          double xs, ys;
          int    x0, y0, i, j, ch;
          float  fx, fy, wx[4], wy[4], v = 0.0f;
          size_t o = (size_t) y * w + x;

          fz_transform_apply (t, cx, cy, x, (double) y, &xs, &ys);
          /* far outside: the nearest edge pixel, not valid */
          xs = FZ_CLAMP (xs, -2.0, w + 1.0);
          ys = FZ_CLAMP (ys, -2.0, h + 1.0);
          x0 = (int) floor (xs);
          y0 = (int) floor (ys);
          fx = (float) (xs - x0);
          fy = (float) (ys - y0);
          for (i = 0; i < 4; i++)
            {
              wx[i] = cubic_w (fx - (i - 1));
              wy[i] = cubic_w (fy - (i - 1));
            }
          for (ch = 0; ch < c; ch++)
            {
              const float *pl = src->p + ch * n;
              float        acc = 0.0f;

              for (j = 0; j < 4; j++)
                {
                  const float *row = pl + (size_t) FZ_CLAMP (y0 - 1 + j, 0, h - 1) * w;
                  float        r = 0.0f;

                  for (i = 0; i < 4; i++)
                    r += wx[i] * row[FZ_CLAMP (x0 - 1 + i, 0, w - 1)];
                  acc += wy[j] * r;
                }
              dst->p[ch * n + o] = acc;
            }
          /* validity: bilinear, nothing beyond the edge pixels' centers
           * (half a pixel of grace) */
          if (xs >= -0.5 && ys >= -0.5 && xs <= w - 0.5 && ys <= h - 0.5)
            {
              int   ax = FZ_CLAMP ((int) floor (xs), 0, w - 1);
              int   ay = FZ_CLAMP ((int) floor (ys), 0, h - 1);
              int   bx = FZ_MIN (ax + 1, w - 1), by = FZ_MIN (ay + 1, h - 1);
              float gx = FZ_CLAMP ((float) (xs - ax), 0.0f, 1.0f);
              float gy = FZ_CLAMP ((float) (ys - ay), 0.0f, 1.0f);
              const float *r0 = valid_src + (size_t) ay * w;
              const float *r1 = valid_src + (size_t) by * w;

              v = (r0[ax] + (r0[bx] - r0[ax]) * gx) * (1.0f - gy)
                  + (r1[ax] + (r1[bx] - r1[ax]) * gx) * gy;
            }
          valid_dst[o] = v;
        }
    }
}

struct FzWarpSource
{
  FzSource           src;
  const FzSource    *base;
  const FzTransform *t;
  FzImage           *tmp;
  float             *tmp_valid;
};

static int
warp_load (void *data, int k, FzImage *img, float *valid, char **error)
{
  FzWarpSource *ws = data;

  if (is_identity (&ws->t[k]))
    return ws->base->load (ws->base->data, k, img, valid, error);
  if (! ws->base->load (ws->base->data, k, ws->tmp, ws->tmp_valid, error))
    return 0;
  fz_warp (ws->tmp, ws->tmp_valid, &ws->t[k], img, valid);
  return 1;
}

FzWarpSource *
fz_warp_source_new (const FzSource *base, const FzTransform *t)
{
  FzWarpSource *ws = calloc (1, sizeof *ws);

  if (! ws)
    return NULL;
  ws->base = base;
  ws->t = t;
  ws->tmp = fz_image_new (base->w, base->h, base->c);
  ws->tmp_valid = fz_alloc_floats ((size_t) base->w * base->h, 0);
  if (! ws->tmp || ! ws->tmp_valid)
    {
      fz_warp_source_free (ws);
      return NULL;
    }
  ws->src = *base;
  ws->src.load = warp_load;
  ws->src.data = ws;
  return ws;
}

const FzSource *
fz_warp_source_get (FzWarpSource *ws)
{
  return &ws->src;
}

void
fz_warp_source_free (FzWarpSource *ws)
{
  if (! ws)
    return;
  fz_image_free (ws->tmp);
  free (ws->tmp_valid);
  free (ws);
}

/* ------------------------------------------------------------ common area */

void
fz_common_area (int w, int h, int n, const FzTransform *t,
                int *x, int *y, int *cw, int *ch)
{
  /* on a grid of at most about 1000 cells a side: which cells every image
   * covers, then the largest rectangle of those (the histogram method) */
  int     step = FZ_MAX (1, (FZ_MAX (w, h) + 999) / 1000);
  int     gw = (w + step - 1) / step, gh = (h + step - 1) / step;
  int    *hist = calloc ((size_t) gw + 1, sizeof (int));
  int    *stk = calloc ((size_t) gw + 2, sizeof (int));
  double  cx = 0.5 * (w - 1), cy = 0.5 * (h - 1);
  long    best = -1;
  int     bx = 0, by = 0, bw = w, bh = h, gy, gx, k;

  *x = 0;
  *y = 0;
  *cw = w;
  *ch = h;
  if (! hist || ! stk)
    {
      free (hist);
      free (stk);
      return;
    }
  for (gy = 0; gy < gh; gy++)
    {
      int top = 0;

      for (gx = 0; gx < gw; gx++)
        {
          /* a cell counts if all its corners are covered by every image */
          int in = 1, cxy;

          for (k = 0; k < n && in; k++)
            for (cxy = 0; cxy < 4 && in; cxy++)
              {
                double px = (cxy & 1) ? FZ_MIN (gx * step + step - 1, w - 1) : gx * step;
                double py = (cxy >> 1) ? FZ_MIN (gy * step + step - 1, h - 1) : gy * step;
                double xs, ys;

                fz_transform_apply (&t[k], cx, cy, px, py, &xs, &ys);
                if (xs < -0.01 || ys < -0.01 || xs > w - 0.99 || ys > h - 0.99)
                  in = 0;
              }
          hist[gx] = in ? hist[gx] + 1 : 0;
        }
      hist[gw] = 0;
      /* largest rectangle in the histogram */
      for (gx = 0; gx <= gw; gx++)
        {
          while (top > 0 && hist[stk[top - 1]] >= hist[gx])
            {
              int  hh = hist[stk[--top]];
              int  left = top > 0 ? stk[top - 1] + 1 : 0;
              long area = (long) hh * (gx - left);

              if (area > best)
                {
                  best = area;
                  bx = left;
                  bw = gx - left;
                  bh = hh;
                  by = gy - hh + 1;
                }
            }
          stk[top++] = gx;
        }
    }
  free (hist);
  free (stk);
  if (best > 0)
    {
      *x = bx * step;
      *y = by * step;
      *cw = FZ_MIN (bw * step, w - *x);
      *ch = FZ_MIN (bh * step, h - *y);
    }
  else if (best == 0)
    {
      *cw = 0;
      *ch = 0;
    }
}
