/*
 * Copyright © 2017 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial
 * portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "config.h"

#include <stdio.h>
#include <math.h>
#include <assert.h>

#ifdef IN_WESTON
#include <wayland-server.h>
#else
#define WL_EXPORT
#endif

#include "csc.h"

static void xy_to_xyz(struct weston_vector *xyz,
		      const struct cie_xy *xy,
		      float luminance)
{
	float xy_z = 1.0f - xy->x - xy->y;
	float xy_y_inv = 1.0f / xy->y;

	xyz->f[0] = luminance * xy->x * xy_y_inv;
	xyz->f[1] = luminance;
	xyz->f[2] = luminance * xy_z * xy_y_inv;
	xyz->f[3] = 1.0f;
}

static void
rgb_to_xyz_matrix(struct weston_matrix *matrix,
		  const struct weston_colorspace *cs)
{
	struct weston_vector r, g, b, w;
	struct weston_matrix p, p_inv;
	int err;

	xy_to_xyz(&w, &cs->primaries.white_point, 1.0f);
	xy_to_xyz(&r, &cs->primaries.r, cs->primaries.r.y);
	xy_to_xyz(&g, &cs->primaries.g, cs->primaries.g.y);
	xy_to_xyz(&b, &cs->primaries.b, cs->primaries.b.y);

	if (cs->primaries.r.x == 1.0f && cs->primaries.r.y == 0.0f &&
	    cs->primaries.g.x == 0.0f && cs->primaries.g.y == 1.0f &&
	    cs->primaries.b.x == 0.0f && cs->primaries.b.y == 0.0f) {
		r.f[0] = 1.0f;
		r.f[1] = 0.0f;
		r.f[2] = 0.0f;
		g.f[0] = 0.0f;
		g.f[1] = 1.0f;
		g.f[2] = 0.0f;
		b.f[0] = 0.0f;
		b.f[1] = 0.0f;
		b.f[2] = 1.0f;
	}

	weston_matrix_init(&p);

	p.d[0 * 4 + 0] = r.f[0];
	p.d[1 * 4 + 0] = g.f[0];
	p.d[2 * 4 + 0] = b.f[0];
	p.d[0 * 4 + 1] = r.f[1];
	p.d[1 * 4 + 1] = g.f[1];
	p.d[2 * 4 + 1] = b.f[1];
	p.d[0 * 4 + 2] = r.f[2];
	p.d[1 * 4 + 2] = g.f[2];
	p.d[2 * 4 + 2] = b.f[2];

	err = weston_matrix_invert(&p_inv, &p);
	assert(err == 0);

	weston_matrix_transform(&p_inv, &w);

	weston_matrix_diag(matrix, &w);

	weston_matrix_multiply(matrix, &p);
}

static void
xyz_to_lms_matrix(struct weston_matrix *matrix)
{
	weston_matrix_init(matrix);

#if 0
	/* von Kries */
	matrix->d[0 * 4 + 0] =  0.4002f;
	matrix->d[1 * 4 + 0] =  0.7076f;
	matrix->d[2 * 4 + 0] = -0.0808f;

	matrix->d[0 * 4 + 1] = -0.2263f;
	matrix->d[1 * 4 + 1] =  1.1653f;
	matrix->d[2 * 4 + 1] =  0.0457f;

	matrix->d[0 * 4 + 2] =  0.0000f;
	matrix->d[1 * 4 + 2] =  0.0000f;
	matrix->d[2 * 4 + 2] =  0.9182f;
#endif
#if 1
	/* Bradford */
	matrix->d[0 * 4 + 0] =  0.8951f;
	matrix->d[1 * 4 + 0] =  0.2664f;
	matrix->d[2 * 4 + 0] = -0.1614f;

	matrix->d[0 * 4 + 1] = -0.7502f;
	matrix->d[1 * 4 + 1] =  1.7135f;
	matrix->d[2 * 4 + 1] =  0.0367f;

	matrix->d[0 * 4 + 2] =  0.0389f;
	matrix->d[1 * 4 + 2] = -0.0685f;
	matrix->d[2 * 4 + 2] =  1.0296f;
#endif
}

static void
cat_matrix(struct weston_matrix *matrix,
	   const struct weston_colorspace *dst,
	   const struct weston_colorspace *src)
{
	struct weston_matrix xyz_to_lms;
	struct weston_vector w_xyz_dst;
	struct weston_vector w_xyz_src;
	struct weston_vector w_lms_dst;
	struct weston_vector w_lms_src;

	xy_to_xyz(&w_xyz_dst, &dst->primaries.white_point, 1.0f);
	xy_to_xyz(&w_xyz_src, &src->primaries.white_point, 1.0f);

	xyz_to_lms_matrix(&xyz_to_lms);

	w_lms_dst = w_xyz_dst;
	weston_matrix_transform(&xyz_to_lms, &w_xyz_dst);

	w_lms_src = w_xyz_src;
	weston_matrix_transform(&xyz_to_lms, &w_xyz_src);

	weston_matrix_init(matrix);

	matrix->d[0 * 4 + 0] = w_lms_dst.f[0] / w_lms_src.f[0];
	matrix->d[1 * 4 + 1] = w_lms_dst.f[1] / w_lms_src.f[1];
	matrix->d[2 * 4 + 2] = w_lms_dst.f[2] / w_lms_src.f[2];
	matrix->d[3 * 4 + 3] = 1.0f;
}

WL_EXPORT void
weston_csc_matrix(struct weston_matrix *matrix,
		  const struct weston_colorspace *dst,
		  const struct weston_colorspace *src,
		  float luminance_scale)
{
	struct weston_matrix rgb_to_xyz_src;
	struct weston_matrix xyz_to_lms;
	struct weston_matrix cat;
	struct weston_matrix lms_to_xyz;
	struct weston_matrix rgb_to_xyz_dst;
	struct weston_matrix xyz_to_rgb_dst;
	int err;

	rgb_to_xyz_matrix(&rgb_to_xyz_src, src);
	rgb_to_xyz_matrix(&rgb_to_xyz_dst, dst);
	err = weston_matrix_invert(&xyz_to_rgb_dst, &rgb_to_xyz_dst);
	assert(err == 0);

	xyz_to_lms_matrix(&xyz_to_lms);
	err = weston_matrix_invert(&lms_to_xyz, &xyz_to_lms);
	assert(err == 0);

	cat_matrix(&cat, dst, src);

#if 0
	printf("RGB to XYZ\n");
	weston_matrix_print(&rgb_to_xyz_src);
	printf("XYZ to LMS\n");
	weston_matrix_print(&xyz_to_lms);
	printf("CAT\n");
	weston_matrix_print(&cat);
	printf("LMS to XYZ\n");
	weston_matrix_print(&lms_to_xyz);
	printf("XYZ to RGB\n");
	weston_matrix_print(&xyz_to_rgb_dst);
#endif

	weston_matrix_init(matrix);

	weston_matrix_multiply(matrix, &rgb_to_xyz_src);
	weston_matrix_multiply(matrix, &xyz_to_lms);
	weston_matrix_multiply(matrix, &cat);
	weston_matrix_multiply(matrix, &lms_to_xyz);
	weston_matrix_multiply(matrix, &xyz_to_rgb_dst);

	weston_matrix_scale(matrix,
			    luminance_scale,
			    luminance_scale,
			    luminance_scale);
}
