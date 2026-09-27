/* gimp-fusion core: exposure fusion, focus stacking and alignment of a
 * stack of images, on planar float images. No GIMP or GEGL in here: the
 * plug-in, the command line tool and the unit tests all use this.
 *
 * Copyright 2026 David
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef FZ_H
#define FZ_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------- images */

/* A planar float image: channel ch, row y, column x is
 * p[((size_t) ch * h + y) * w + x]. */
typedef struct
{
  int    w, h, c;
  float *p;
} FzImage;

FzImage *fz_image_new   (int w, int h, int c);   /* zeroed; NULL without memory */
void     fz_image_free  (FzImage *img);
float   *fz_image_plane (const FzImage *img, int ch);

/* 1 and 3 channel images: the gray value used for contrast (Rec. 601 luma
 * as Matlab's rgb2gray, which Mertens' own code uses) and for sharpness
 * (Rec. 709 luma) */
void fz_luma601 (const FzImage *img, float *out);
void fz_luma709 (const FzImage *img, float *out);

/* ------------------------------------------------------------ filtering */

/* Gaussian blur of one plane, separable, edges repeated. sigma <= 0 copies.
 * src and dst may be the same. Returns 0 without memory. */
int  fz_gauss_blur (const float *src, float *dst, int w, int h, double sigma);

/* the 4-neighbour Laplacian [0 1 0; 1 -4 1; 0 1 0], edges repeated */
void fz_laplacian (const float *src, float *dst, int w, int h);

/* ------------------------------------------------------------- pyramids */

#define FZ_MAX_LEVELS 32

/* The number of levels Mertens' code uses, floor(log2(min(w, h))), at
 * least 1 */
int  fz_pyramid_levels (int w, int h);

/* Burt and Adelson's reduce and expand with the 5-tap kernel
 * [1 4 6 4 1] / 16, as in Mertens' code: reduce filters with symmetric
 * (half sample) edges and keeps the even samples, (w + 1) / 2 by
 * (h + 1) / 2; expand repeats the edges. */
void fz_downsample   (const float *src, int w, int h, float *dst);
/* dst (W x H, the finer size) += sign * expand (src (w x h)) */
int  fz_upsample_add (const float *src, int w, int h, float *dst, int W, int H,
                      float sign);

typedef struct
{
  int    levels, c;
  int    w[FZ_MAX_LEVELS], h[FZ_MAX_LEVELS];
  float *lev[FZ_MAX_LEVELS];      /* planar, c channels */
} FzPyramid;

/* a zeroed pyramid of the given number of levels (0: fz_pyramid_levels) */
FzPyramid *fz_pyramid_new      (int w, int h, int c, int levels);
void       fz_pyramid_free     (FzPyramid *pyr);
/* collapses a Laplacian pyramid in place: level 0 is the image then */
int        fz_pyramid_collapse (FzPyramid *pyr);
/* Laplacian pyramid of img into a new pyramid (for tests) */
FzPyramid *fz_laplacian_pyramid (const FzImage *img, int levels);

/* ---------------------------------------------------------------- sources */

/* Where the images of the stack come from, one at a time, so that a stack
 * of any length needs the memory of a few images only. load() fills img
 * (w x h x c, already allocated) and valid (w x h: 1 where the image has
 * data, 0 where not, e.g. transparent or outside the layer; values in
 * between weigh the pixel down). Pixels that are not valid should still
 * hold plausible colors (the nearest valid ones), so that no false edges
 * appear. Returns 0 on failure and may set *error (g_malloc'ed style: the
 * caller frees it with free()). */
typedef struct
{
  int    n, w, h, c;             /* images, size, channels (1 or 3) */
  int  (*load) (void *data, int k, FzImage *img, float *valid, char **error);
  void  *data;
} FzSource;

typedef struct
{
  /* fraction done, 0 to 1 */
  void (*progress) (void *data, double fraction);
  void  *data;
  /* statistics, for tests and benchmarks: filled in if not NULL */
  double seconds_align, seconds_fuse;
} FzProgress;

/* ------------------------------------------------------- exposure fusion */

typedef struct
{
  double contrast;     /* exponents of the three quality measures */
  double saturation;
  double exposure;
  double sigma;        /* width of the well-exposedness curve: 0.2 */
  double center;       /* its center: 0.5 */
  int    levels;       /* 0: as the paper's code, floor(log2(min(w, h))) */
  int    clip;         /* clip the result to 0..1 */
} FzExposureParams;

void fz_exposure_params_default (FzExposureParams *p);

/* the (unnormalized) weight map of one image, W = C^wc S^ws E^we, times
 * the validity, plus nothing: Mertens adds 1e-12 before normalizing, which
 * fz_fuse_exposures does */
int fz_exposure_weights (const FzImage *img, const float *valid,
                         const FzExposureParams *p, float *weight);

/* Fuses the stack into out (w x h x c, allocated by the caller). alpha
 * (w x h, may be NULL) gets the largest validity of the images. Returns 0
 * on failure with *error set. */
int fz_fuse_exposures (const FzSource *src, const FzExposureParams *p,
                       FzImage *out, float *alpha, FzProgress *prog,
                       char **error);

/* --------------------------------------------------------- focus stacking */

typedef enum
{
  FZ_FOCUS_PYRAMID,    /* sharpest image per pixel, blended by pyramids */
  FZ_FOCUS_COEFF,      /* sharpest Laplacian coefficient per level */
  FZ_FOCUS_SOFT,       /* weighted by sharpness^power, blended by pyramids */
  FZ_FOCUS_PIXELS      /* sharpest image per pixel, no blending */
} FzFocusMethod;

typedef enum
{
  FZ_MEASURE_LAPLACIAN,   /* local energy of the Laplacian */
  FZ_MEASURE_VARIANCE     /* local variance */
} FzMeasure;

typedef struct
{
  FzFocusMethod method;
  FzMeasure     measure;
  double        noise;        /* sigma of a blur before measuring, pixels */
  double        window;       /* sigma of the measuring window, pixels */
  double        power;        /* FZ_FOCUS_SOFT: exponent */
  double        depth_smooth; /* sigma for smoothing the depth map; 0: none */
  int           levels;       /* 0: automatic */
  int           clip;
} FzFocusParams;

void fz_focus_params_default (FzFocusParams *p);

/* the sharpness of one image (w x h); invalid pixels get -1 */
int fz_focus_measure (const FzImage *img, const float *valid,
                      const FzFocusParams *p, float *measure);

/* Fuses a focus stack. depth (w x h, may be NULL) gets the position of
 * the sharpest image at each pixel, 0 to n - 1, with a sub-image
 * position from a parabola through the neighbours, smoothed by
 * depth_smooth; -1 where no image is valid. */
int fz_focus_stack (const FzSource *src, const FzFocusParams *p,
                    FzImage *out, float *alpha, float *depth,
                    FzProgress *prog, char **error);

/* ------------------------------------------------------------- alignment */

typedef enum
{
  FZ_ALIGN_NONE,
  FZ_ALIGN_TRANSLATION,
  FZ_ALIGN_SCALE,           /* translation and scale */
  FZ_ALIGN_SIMILARITY       /* translation, rotation and scale */
} FzAlignModel;

/* Maps a position in the reference frame to the image's own pixels, about
 * the center (cx, cy) of the canvas:
 *   x' - cx = a (x - cx) - b (y - cy) + tx
 *   y' - cy = b (x - cx) + a (y - cy) + ty
 * so a = s cos(theta), b = s sin(theta). Pixel centers are at integers. */
typedef struct
{
  double a, b, tx, ty;
} FzTransform;

void fz_transform_identity (FzTransform *t);
void fz_transform_apply    (const FzTransform *t, double cx, double cy,
                            double x, double y, double *xo, double *yo);
/* t = first applied after second: (t1 o t2)(p) = t1 (t2 (p)) */
void fz_transform_compose  (const FzTransform *t1, const FzTransform *t2,
                            FzTransform *out);
void fz_transform_invert   (const FzTransform *t, FzTransform *out);

typedef struct
{
  FzAlignModel model;
  int          reference;   /* the image the others are aligned to; -1: middle */
  int          max_shift;   /* percent of the smaller side: 0 = 25 */
} FzAlignParams;

/* Finds for every image the transform from the reference frame to it
 * (identity for the reference), by aligning each image to its neighbour
 * towards the reference and chaining: phase correlation for the shift,
 * then Gauss-Newton on local contrast, coarse to fine, for the model's
 * parameters. confidence (n, may be NULL) gets a quality in 0..1 per
 * image (1 for the reference). */
int fz_align (const FzSource *src, const FzAlignParams *p, FzTransform *out,
              double *confidence, FzProgress *prog, char **error);

/* Estimates the transform between two planes of the same size (the
 * building block of fz_align): t maps positions in a to positions in b.
 * mask_a and mask_b may be NULL. */
int fz_align_pair (const float *a, const float *mask_a,
                   const float *b, const float *mask_b, int w, int h,
                   FzAlignModel model, int max_shift, FzTransform *t,
                   double *confidence);

/* phase correlation of two planes of the same size: the shift (dx, dy)
 * with a(x, y) ~ b(x + dx, y + dy), to a fraction of a pixel; peak gets
 * the height of the correlation peak (0..1) */
int fz_phase_correlate (const float *a, const float *b, int w, int h,
                        double *dx, double *dy, double *peak);

/* A source that gives the images of base, warped by the transforms
 * (bicubic; outside the image the edge repeats and valid is 0). The
 * returned source keeps pointers to base and t: keep them alive. */
typedef struct FzWarpSource FzWarpSource;
FzWarpSource   *fz_warp_source_new  (const FzSource *base, const FzTransform *t);
const FzSource *fz_warp_source_get  (FzWarpSource *ws);
void            fz_warp_source_free (FzWarpSource *ws);

/* warps one image (for tests): dst(x) = src(t(x)) */
void fz_warp (const FzImage *src, const float *valid_src, const FzTransform *t,
              FzImage *dst, float *valid_dst);

/* the largest axis-aligned rectangle (x, y, w, h) inside the canvas where
 * every transformed image covers the canvas: a conservative crop */
void fz_common_area (int w, int h, int n, const FzTransform *t,
                     int *x, int *y, int *cw, int *ch);

/* ----------------------------------------------------------------- misc */

/* discrete Fourier transform, in place, n a power of two; inverse
 * without 1/n (for tests and phase correlation) */
void fz_fft (double *re, double *im, int n, int inverse);
int  fz_fft2 (double *re, double *im, int w, int h, int inverse);

void fz_set_threads (int n);   /* 0: as many as OpenMP likes */
char *fz_strdup_printf (const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#endif
