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

#ifndef WESTON_COLORSPACE_H
#define WESTON_COLORSPACE_H

#include "matrix.h"

#ifdef  __cplusplus
extern "C" {
#endif

/** A CIE 1931 color space*/
struct cie_xy {
	double x;
	double y;
};

struct color_primaries {
	struct cie_xy r;
	struct cie_xy g;
	struct cie_xy b;
	struct cie_xy white_point;
};

struct weston_colorspace {
	struct color_primaries primaries;
	const char *name;
	const char *whitepoint_name;
};

enum weston_colorspace_enums {
	WESTON_CS_BT470M,
	WESTON_CS_BT470BG,
	WESTON_CS_SMPTE170M,
	WESTON_CS_SMPTE240M,
	WESTON_CS_BT709,
	WESTON_CS_BT2020,
	WESTON_CS_SRGB,
	WESTON_CS_ADOBERGB,
	WESTON_CS_DCI_P3,
	WESTON_CS_PROPHOTORGB,
	WESTON_CS_CIERGB,
	WESTON_CS_CIEXYZ,
	WESTON_CS_AP0,
	WESTON_CS_AP1,
	WESTON_CS_UNDEFINED
};

const struct weston_colorspace *
weston_colorspace_lookup(enum weston_colorspace_enums colorspace);

#ifdef  __cplusplus
}
#endif

#endif
