/* gimp-fusion core: what the core's files share
 *
 * Copyright 2026 David
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef FZ_PRIVATE_H
#define FZ_PRIVATE_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "fz.h"

#include <time.h>

#ifdef _OPENMP
#include <omp.h>
#endif

#define FZ_CLAMP(v, lo, hi) ((v) < (lo) ? (lo) : (v) > (hi) ? (hi) : (v))
#define FZ_MIN(a, b) ((a) < (b) ? (a) : (b))
#define FZ_MAX(a, b) ((a) > (b) ? (a) : (b))

float  *fz_alloc_floats (size_t n, int zero);
void    fz_set_error    (char **error, const char *fmt, ...)
  __attribute__ ((format (printf, 2, 3)));
double  fz_now          (void);
void    fz_report       (FzProgress *prog, double fraction);

/* acc += Gaussian pyramid (weight) * Laplacian pyramid (img), level by
 * level; img (acc->c planes of w x h) and weight (w x h, NULL: 1) are used
 * as scratch space and hold garbage afterwards */
int fz_pyramid_accumulate (FzPyramid *acc, float *img, float *weight);

#endif
