/* fusion-cli: the gimp-fusion core on the command line, for tests and
 * benchmarks. Reads and writes PFM (portable float map) files, gray or RGB.
 *
 *   fusion-cli exposures [options] -o out.pfm in1.pfm in2.pfm ...
 *   fusion-cli focus     [options] -o out.pfm [--depth=d.pfm] in1.pfm ...
 *   fusion-cli align     [options] in1.pfm in2.pfm ...
 *   fusion-cli weights   [options] -o w.pfm in.pfm
 *   fusion-cli pyramid   [--levels=N] -o out.pfm in.pfm   (decompose + collapse)
 *   fusion-cli warp      --transform=a,b,tx,ty -o out.pfm in.pfm
 *
 * Options: --contrast= --saturation= --exposure= --sigma= --levels=
 * --no-clip; --method=pyramid|coeff|soft|pixels --measure=laplacian|variance
 * --noise= --window= --power= --depth-smooth=; --align=none|translation|
 * scale|similarity --reference=K --normalize=0|1; --threads=N; --stats (time and peak
 * memory on stderr); --synthetic=W,H,N,C instead of input files: a stack
 * made up while it is read (for benchmarks: no files, no disk).
 *
 * Copyright 2026 David
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#define _GNU_SOURCE
#include "fz.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ------------------------------------------------------------------ PFM */

static FzImage *
pfm_read (const char *path, char **error)
{
  FILE    *f = fopen (path, "rb");
  char     kind[3] = { 0 };
  int      w, h, c, y, x, ch;
  double   scale;
  float   *row;
  FzImage *img;

  if (! f)
    {
      *error = fz_strdup_printf ("cannot open %s", path);
      return NULL;
    }
  if (fscanf (f, "%2s %d %d %lf", kind, &w, &h, &scale) != 4 || fgetc (f) == EOF ||
      (strcmp (kind, "PF") && strcmp (kind, "Pf")) || w <= 0 || h <= 0 ||
      w > 100000 || h > 100000)
    {
      fclose (f);
      *error = fz_strdup_printf ("%s is not a PFM file", path);
      return NULL;
    }
  c = kind[1] == 'F' ? 3 : 1;
  img = fz_image_new (w, h, c);
  row = malloc (sizeof (float) * w * c);
  if (! img || ! row)
    {
      fclose (f);
      fz_image_free (img);
      free (row);
      *error = fz_strdup_printf ("not enough memory for %s", path);
      return NULL;
    }
  /* bottom row first; little endian when the scale is negative (the only
   * kind this writes, and what x86 writes) */
  for (y = h - 1; y >= 0; y--)
    {
      if (fread (row, sizeof (float) * c, w, f) != (size_t) w)
        {
          fclose (f);
          fz_image_free (img);
          free (row);
          *error = fz_strdup_printf ("%s is cut short", path);
          return NULL;
        }
      for (x = 0; x < w; x++)
        for (ch = 0; ch < c; ch++)
          fz_image_plane (img, ch)[(size_t) y * w + x] = row[x * c + ch];
    }
  (void) scale;
  fclose (f);
  free (row);
  return img;
}

static int
pfm_write (const char *path, const float *planes, int w, int h, int c)
{
  FILE  *f = fopen (path, "wb");
  float *row = malloc (sizeof (float) * w * c);
  int    y, x, ch, ok = 1;

  if (! f || ! row)
    {
      if (f)
        fclose (f);
      free (row);
      return 0;
    }
  fprintf (f, "%s\n%d %d\n-1.0\n", c == 3 ? "PF" : "Pf", w, h);
  for (y = h - 1; y >= 0; y--)
    {
      for (x = 0; x < w; x++)
        for (ch = 0; ch < c; ch++)
          row[x * c + ch] = planes[((size_t) ch * h + y) * w + x];
      if (fwrite (row, sizeof (float) * c, w, f) != (size_t) w)
        ok = 0;
    }
  free (row);
  if (fclose (f) != 0)
    ok = 0;
  return ok;
}

/* -------------------------------------------------------------- sources */

typedef struct
{
  char **paths;
  int    synthetic;     /* 1: exposures, 2: focus */
  int    n;             /* the number of images */
  int    loads;
} Files;

/* a made up scene, quick to compute: a smooth radiance field with texture
 * (for exposures, times 2^(k - n/2)), or texture that gets softer away from
 * a depth per image (for focus) */
static float
hash01 (unsigned x, unsigned y, unsigned s)
{
  unsigned h = x * 374761393u + y * 668265263u + s * 2246822519u;

  h = (h ^ (h >> 13)) * 1274126177u;
  return (float) ((h ^ (h >> 16)) & 0xffffff) / 16777216.0f;
}

static int
synthetic_load (Files *fs, const FzSource *src, int k, FzImage *img, float *valid)
{
  int       w = src->w, h = src->h, c = src->c;
  ptrdiff_t y;

#pragma omp parallel for schedule(static)
  for (y = 0; y < h; y++)
    {
      int x, ch;

      for (x = 0; x < w; x++)
        {
          float  base = 0.5f + 0.45f * sinf (x * 0.003f) * cosf (y * 0.002f);
          float  tex = hash01 (x / 3, y / 3, 7) - 0.5f;
          size_t i = (size_t) y * w + x;

          for (ch = 0; ch < c; ch++)
            {
              float v;

              if (fs->synthetic == 1)
                {
                  float e = exp2f ((float) k - 0.5f * (src->n - 1));
                  float rad = expf (4.0f * (base - 0.5f)) * (1.0f + 0.3f * tex) *
                              (1.0f + 0.1f * ch);

                  v = fminf (1.0f, powf (rad * e * 0.3f, 1.0f / 2.2f));
                }
              else
                {
                  float d = (float) (src->n - 1) * (0.5f + 0.5f * sinf (x * 0.001f + y * 0.0007f));
                  float s = 1.0f / (1.0f + fabsf (d - k));

                  v = base + 0.3f * s * tex;
                }
              img->p[(size_t) ch * w * h + i] = v;
            }
          valid[i] = 1.0f;
        }
    }
  return 1;
}

static int
files_load (void *data, int k, FzImage *img, float *valid, char **error)
{
  Files   *fs = data;
  FzImage *f;
  size_t   n = (size_t) img->w * img->h, i;

  fs->loads++;
  if (fs->synthetic)
    {
      FzSource s = { 0 };

      s.w = img->w;
      s.h = img->h;
      s.c = img->c;
      s.n = fs->n;
      return synthetic_load (fs, &s, k, img, valid);
    }
  f = pfm_read (fs->paths[k], error);
  if (! f)
    return 0;
  if (f->w != img->w || f->h != img->h || f->c != img->c)
    {
      *error = fz_strdup_printf ("%s is %d x %d x %d, not %d x %d x %d", fs->paths[k],
                                 f->w, f->h, f->c, img->w, img->h, img->c);
      fz_image_free (f);
      return 0;
    }
  memcpy (img->p, f->p, n * img->c * sizeof (float));
  for (i = 0; i < n; i++)
    valid[i] = 1.0f;
  fz_image_free (f);
  return 1;
}

/* ------------------------------------------------------------------ main */

static double
now (void)
{
  struct timespec ts;

  clock_gettime (CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + 1e-9 * ts.tv_nsec;
}

static int
usage (void)
{
  fprintf (stderr,
           "usage: fusion-cli exposures|focus|align|weights|pyramid|warp [options] "
           "[-o out.pfm] in.pfm...\n(see the comment at the top of tools/fusion-cli.c)\n");
  return 2;
}

/* the value of --name=value, or NULL */
static const char *
optval (const char *a, const char *name)
{
  size_t n = strlen (name);

  return ! strncmp (a, name, n) && a[n] == '=' ? a + n + 1 : NULL;
}

static int
choose (const char *v, const char *const *names, int *out)
{
  int i;

  for (i = 0; names[i]; i++)
    if (! strcmp (v, names[i]))
      {
        *out = i;
        return 1;
      }
  return 0;
}

int
main (int argc, char **argv)
{
  static const char *const methods[] = { "pyramid", "coeff", "soft", "pixels", NULL };
  static const char *const measures[] = { "laplacian", "variance", NULL };
  static const char *const models[] = { "none", "translation", "scale", "similarity", NULL };
  FzExposureParams ep;
  FzFocusParams    fp;
  FzAlignParams    ap = { FZ_ALIGN_NONE, -1, 0, -1 };
  const char      *cmd, *outp = NULL, *depthp = NULL;
  char           **inputs;
  int              nin = 0, i, stats = 0, status = 1, sw = 0, sh = 0, sn = 0, sc = 0;
  FzTransform      warp_t = { 1, 0, 0, 0 };
  char            *error = NULL;
  Files            fs = { 0 };
  FzSource         src = { 0 };
  FzProgress       prog = { 0 };
  FzTransform     *tr = NULL;
  FzWarpSource    *ws = NULL;
  double           t0 = now (), t_align = 0.0, *conf = NULL;

  if (argc < 2)
    return usage ();
  cmd = argv[1];
  fz_exposure_params_default (&ep);
  fz_focus_params_default (&fp);
  inputs = calloc ((size_t) argc, sizeof (char *));
  for (i = 2; i < argc; i++)
    {
      const char *a = argv[i], *o;
      int         v;

      if (! strcmp (a, "-o") && i + 1 < argc)
        outp = argv[++i];
      else if ((o = optval (a, "--contrast")))
        ep.contrast = atof (o);
      else if ((o = optval (a, "--saturation")))
        ep.saturation = atof (o);
      else if ((o = optval (a, "--exposure")))
        ep.exposure = atof (o);
      else if ((o = optval (a, "--sigma")))
        ep.sigma = atof (o);
      else if ((o = optval (a, "--levels")))
        ep.levels = fp.levels = atoi (o);
      else if (! strcmp (a, "--no-clip"))
        ep.clip = fp.clip = 0;
      else if ((o = optval (a, "--method")) && choose (o, methods, &v))
        fp.method = v;
      else if ((o = optval (a, "--measure")) && choose (o, measures, &v))
        fp.measure = v;
      else if ((o = optval (a, "--noise")))
        fp.noise = atof (o);
      else if ((o = optval (a, "--window")))
        fp.window = atof (o);
      else if ((o = optval (a, "--power")))
        fp.power = atof (o);
      else if ((o = optval (a, "--depth-smooth")))
        fp.depth_smooth = atof (o);
      else if ((o = optval (a, "--depth")))
        depthp = o;
      else if ((o = optval (a, "--align")) && choose (o, models, &v))
        ap.model = v;
      else if ((o = optval (a, "--model")) && choose (o, models, &v))
        ap.model = v;
      else if ((o = optval (a, "--normalize")))
        ap.normalize = atoi (o);
      else if ((o = optval (a, "--reference")))
        ap.reference = atoi (o);
      else if ((o = optval (a, "--threads")))
        fz_set_threads (atoi (o));
      else if ((o = optval (a, "--transform")))
        {
          if (sscanf (o, "%lf,%lf,%lf,%lf", &warp_t.a, &warp_t.b,
                      &warp_t.tx, &warp_t.ty) != 4)
            return usage ();
        }
      else if ((o = optval (a, "--synthetic")))
        {
          if (sscanf (o, "%d,%d,%d,%d", &sw, &sh, &sn, &sc) != 4 ||
              sw < 1 || sh < 1 || sn < 1 || (sc != 1 && sc != 3))
            return usage ();
        }
      else if (! strcmp (a, "--stats"))
        stats = 1;
      else if (a[0] == '-' && a[1] == '-')
        {
          fprintf (stderr, "fusion-cli: unknown option %s\n", a);
          return 2;
        }
      else
        inputs[nin++] = argv[i];
    }

  if (sw)
    {
      src.n = sn;
      src.w = sw;
      src.h = sh;
      src.c = sc;
      fs.synthetic = ! strcmp (cmd, "focus") ? 2 : 1;
    }
  else
    {
      FzImage *first;

      if (nin < 1)
        return usage ();
      first = pfm_read (inputs[0], &error);
      if (! first)
        {
          fprintf (stderr, "fusion-cli: %s\n", error);
          return 1;
        }
      src.n = nin;
      src.w = first->w;
      src.h = first->h;
      src.c = first->c;
      fz_image_free (first);
    }
  fs.paths = inputs;
  src.load = files_load;
  src.data = &fs;
  fs.n = src.n;

  if (! strcmp (cmd, "phase") && nin == 2)
    {
      FzImage *a = pfm_read (inputs[0], &error), *b = a ? pfm_read (inputs[1], &error) : NULL;
      double   dx, dy, peak;

      if (a && b && fz_phase_correlate (a->p, b->p, a->w, a->h, &dx, &dy, &peak))
        {
          printf ("%.4f %.4f %.4f\n", dx, dy, peak);
          status = 0;
        }
      fz_image_free (a);
      fz_image_free (b);
      goto done;
    }
  if (! strcmp (cmd, "pyramid") || ! strcmp (cmd, "weights") || ! strcmp (cmd, "warp"))
    {
      FzImage *img = pfm_read (inputs[0], &error);

      if (! img || ! outp)
        {
          fprintf (stderr, "fusion-cli: %s\n", error ? error : "no -o");
          goto done;
        }
      if (! strcmp (cmd, "pyramid"))
        {
          FzPyramid *pyr = fz_laplacian_pyramid (img, ep.levels);

          if (! pyr || ! fz_pyramid_collapse (pyr) ||
              ! pfm_write (outp, pyr->lev[0], img->w, img->h, img->c))
            fprintf (stderr, "fusion-cli: pyramid failed\n");
          else
            status = 0;
          fz_pyramid_free (pyr);
        }
      else if (! strcmp (cmd, "weights"))
        {
          float *w = malloc (sizeof (float) * img->w * img->h);

          if (w && fz_exposure_weights (img, NULL, &ep, w) &&
              pfm_write (outp, w, img->w, img->h, 1))
            status = 0;
          free (w);
        }
      else
        {
          FzImage *o = fz_image_new (img->w, img->h, img->c);
          float   *v = malloc (sizeof (float) * img->w * img->h);
          float   *vo = malloc (sizeof (float) * img->w * img->h);
          size_t   k;

          if (o && v && vo)
            {
              for (k = 0; k < (size_t) img->w * img->h; k++)
                v[k] = 1.0f;
              fz_warp (img, v, &warp_t, o, vo);
              if (pfm_write (outp, o->p, o->w, o->h, o->c))
                status = 0;
            }
          fz_image_free (o);
          free (v);
          free (vo);
        }
      fz_image_free (img);
      goto done;
    }

  /* alignment first, if asked for (the align command always aligns) */
  if (! strcmp (cmd, "align") && ap.model == FZ_ALIGN_NONE)
    ap.model = FZ_ALIGN_SIMILARITY;
  /* local normalization for exposures (and by default for align), plain
   * images for focus stacks */
  if (ap.normalize < 0)
    ap.normalize = strcmp (cmd, "focus") != 0;
  if (ap.model != FZ_ALIGN_NONE)
    {
      double ta = now ();

      tr = calloc ((size_t) src.n, sizeof (FzTransform));
      conf = calloc ((size_t) src.n, sizeof (double));
      if (! tr || ! conf || ! fz_align (&src, &ap, tr, conf, &prog, &error))
        {
          fprintf (stderr, "fusion-cli: %s\n", error ? error : "alignment failed");
          goto done;
        }
      t_align = now () - ta;
      if (! strcmp (cmd, "align"))
        {
          for (i = 0; i < src.n; i++)
            printf ("%d %.9f %.9f %.6f %.6f %.4f\n", i, tr[i].a, tr[i].b, tr[i].tx,
                    tr[i].ty, conf[i]);
          status = 0;
          goto done;
        }
      ws = fz_warp_source_new (&src, tr);
      if (! ws)
        {
          fprintf (stderr, "fusion-cli: not enough memory\n");
          goto done;
        }
    }

  if (! strcmp (cmd, "exposures") || ! strcmp (cmd, "focus"))
    {
      FzImage        *out = fz_image_new (src.w, src.h, src.c);
      float          *depth = NULL;
      const FzSource *s = ws ? fz_warp_source_get (ws) : &src;
      int             ok;

      if (depthp)
        depth = malloc (sizeof (float) * src.w * src.h);
      if (! out || (depthp && ! depth))
        {
          fprintf (stderr, "fusion-cli: not enough memory\n");
          fz_image_free (out);
          free (depth);
          goto done;
        }
      if (! strcmp (cmd, "exposures"))
        ok = fz_fuse_exposures (s, &ep, out, NULL, &prog, &error);
      else
        ok = fz_focus_stack (s, &fp, out, NULL, depth, &prog, &error);
      if (! ok)
        fprintf (stderr, "fusion-cli: %s\n", error ? error : "failed");
      else if (outp && ! pfm_write (outp, out->p, out->w, out->h, out->c))
        fprintf (stderr, "fusion-cli: cannot write %s\n", outp);
      else if (depthp && ! pfm_write (depthp, depth, src.w, src.h, 1))
        fprintf (stderr, "fusion-cli: cannot write %s\n", depthp);
      else
        status = 0;
      fz_image_free (out);
      free (depth);
    }
  else if (strcmp (cmd, "align"))
    return usage ();

done:
  if (stats)
    {
      /* VmHWM: the peak of this process's own memory (ru_maxrss would
       * survive an execve) */
      FILE  *f = fopen ("/proc/self/status", "r");
      char   line[256];
      double mb = -1.0;

      while (f && fgets (line, sizeof line, f))
        if (! strncmp (line, "VmHWM:", 6))
          mb = atof (line + 6) / 1024.0;
      if (f)
        fclose (f);
      fprintf (stderr, "stats: seconds %.3f align %.3f fuse %.3f peak_mb %.1f loads %d\n",
               now () - t0, t_align, prog.seconds_fuse, mb, fs.loads);
    }
  fz_warp_source_free (ws);
  free (tr);
  free (conf);
  free (inputs);
  free (error);
  return status;
}
