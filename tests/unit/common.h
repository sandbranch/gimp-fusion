/* shared helpers of the unit tests
 *
 * Copyright 2026 David
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef TEST_COMMON_H
#define TEST_COMMON_H

#define _GNU_SOURCE
#include "fz.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

extern int test_failures, test_passes;

/* prints PASS or FAIL with the message */
void check (int ok, const char *fmt, ...) __attribute__ ((format (printf, 2, 3)));
int  test_finish (void);

/* a deterministic pseudo random number in [0, 1) */
double urand (unsigned *state);

/* a smooth, textured test pattern in about 0..1, defined everywhere (so
 * that shifted copies can be sampled exactly): sums of sinusoids and
 * blobs, seed chooses them */
double pattern (double x, double y, unsigned seed);

/* an image of the pattern, sampled at t (x) for every pixel x (identity:
 * NULL); c channels, each a little different */
FzImage *pattern_image (int w, int h, int c, unsigned seed, const FzTransform *t);

double max_abs_diff (const float *a, const float *b, size_t n);
double psnr (const float *a, const float *b, size_t n);   /* peak 1 */

/* a source over an array of images, all valid unless valid[k] given */
typedef struct
{
  FzImage **img;
  float   **valid;
  int       loads;
} ArraySource;
int  array_load (void *data, int k, FzImage *img, float *valid, char **error);
void array_source (FzSource *src, ArraySource *as, FzImage **img, float **valid, int n);

#endif
