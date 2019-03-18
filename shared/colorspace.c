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

#include <stdlib.h>
#include <string.h>

#ifdef IN_WESTON
#include <wayland-server.h>
#else
#define WL_EXPORT
#endif

#include "helpers.h"
#include <libweston/colorspace.h>

static const struct weston_colorspace bt470m = {
	.primaries.r = { 0.670f, 0.330f, },
	.primaries.g = { 0.210f, 0.710f, },
	.primaries.b = { 0.140f, 0.080f, },
	.primaries.white_point = { 0.3101f, 0.3162f, },
	.name = "BT.470 M",
	.whitepoint_name = "C",
};

static const struct weston_colorspace bt470bg = {
	.primaries.r = { 0.640f, 0.330f, },
	.primaries.g = { 0.290f, 0.600f, },
	.primaries.b = { 0.150f, 0.060f, },
	.primaries.white_point = { 0.3127f, 0.3290f, },
	.name = "BT.470 B/G",
	.whitepoint_name = "D65",
};

static const struct weston_colorspace smpte170m = {
	.primaries.r = { 0.630f, 0.340f, },
	.primaries.g = { 0.310f, 0.595f, },
	.primaries.b = { 0.155f, 0.070f, },
	.primaries.white_point = { 0.3127f, 0.3290f, },
	.name = "SMPTE 170M",
	.whitepoint_name = "D65",
};

static const struct weston_colorspace smpte240m = {
	.primaries.r = { 0.630f, 0.340f, },
	.primaries.g = { 0.310f, 0.595f, },
	.primaries.b = { 0.155f, 0.070f, },
	.primaries.white_point = { 0.3127f, 0.3290f, },
	.name = "SMPTE 240M",
	.whitepoint_name = "D65",
};

static const struct weston_colorspace bt709 = {
	.primaries.r = { 0.640f, 0.330f, },
	.primaries.g = { 0.300f, 0.600f, },
	.primaries.b = { 0.150f, 0.060f, },
	.primaries.white_point = { 0.3127f, 0.3290f, },
	.name = "BT.709",
	.whitepoint_name = "D65",
};

static const struct weston_colorspace bt2020 = {
	.primaries.r = { 0.708f, 0.292f, },
	.primaries.g = { 0.170f, 0.797f, },
	.primaries.b = { 0.131f, 0.046f, },
	.primaries.white_point = { 0.3127f, 0.3290f, },
	.name = "BT.2020",
	.whitepoint_name = "D65",
};

static const struct weston_colorspace srgb = {
	.primaries.r = { 0.640f, 0.330f, },
	.primaries.g = { 0.300f, 0.600f, },
	.primaries.b = { 0.150f, 0.060f, },
	.primaries.white_point = { 0.3127f, 0.3290f, },
	.name = "sRGB",
	.whitepoint_name = "D65",
};

static const struct weston_colorspace adobergb = {
	.primaries.r = { 0.640f, 0.330f, },
	.primaries.g = { 0.210f, 0.710f, },
	.primaries.b = { 0.150f, 0.060f, },
	.primaries.white_point = { 0.3127f, 0.3290f, },
	.name = "AdobeRGB",
	.whitepoint_name = "D65",
};

static const struct weston_colorspace dci_p3 = {
	.primaries.r = { 0.680f, 0.320f, },
	.primaries.g = { 0.265f, 0.690f, },
	.primaries.b = { 0.150f, 0.060f, },
	.primaries.white_point = { 0.3127f, 0.3290f, },
	.name = "DCI-P3 D65",
	.whitepoint_name = "D65",
};

static const struct weston_colorspace prophotorgb = {
	.primaries.r = { 0.7347f, 0.2653f, },
	.primaries.g = { 0.1596f, 0.8404f, },
	.primaries.b = { 0.0366f, 0.0001f, },
	.primaries.white_point = { .3457, .3585 },
	.name = "ProPhoto RGB",
	.whitepoint_name = "D50",
};

static const struct weston_colorspace ciergb = {
	.primaries.r = { 0.7347f, 0.2653f, },
	.primaries.g = { 0.2738f, 0.7174f, },
	.primaries.b = { 0.1666f, 0.0089f, },
	.primaries.white_point = { 1.0f / 3.0f, 1.0f / 3.0f, },
	.name = "CIE RGB",
	.whitepoint_name = "E",
};

static const struct weston_colorspace ciexyz = {
	.primaries.r = { 1.0f, 0.0f, },
	.primaries.g = { 0.0f, 1.0f, },
	.primaries.b = { 0.0f, 0.0f, },
	.primaries.white_point = { 1.0f / 3.0f, 1.0f / 3.0f, },
	.name = "CIE XYZ",
	.whitepoint_name = "E",
};

const struct weston_colorspace ap0 = {
	.primaries.r = { 0.7347f,  0.2653f, },
	.primaries.g = { 0.0000f,  1.0000f, },
	.primaries.b = { 0.0001f, -0.0770f, },
	.primaries.white_point = { .32168f, .33767f, },
	.name = "ACES .primaries #0",
	.whitepoint_name = "D60",
};

const struct weston_colorspace ap1 = {
	.primaries.r = { 0.713f, 0.393f, },
	.primaries.g = { 0.165f, 0.830f, },
	.primaries.b = { 0.128f, 0.044f, },
	.primaries.white_point = { 0.32168f, 0.33767f, },
	.name = "ACES .primaries #1",
	.whitepoint_name = "D60",
};

static const struct weston_colorspace * const colorspaces[] = {
	[WESTON_CS_BT470M] = &bt470m,
	[WESTON_CS_BT470BG] = &bt470bg,
	[WESTON_CS_SMPTE170M] = &smpte170m,
	[WESTON_CS_SMPTE240M] = &smpte240m,
	[WESTON_CS_BT709] = &bt709,
	[WESTON_CS_BT2020] = &bt2020,
	[WESTON_CS_SRGB] = &srgb,
	[WESTON_CS_ADOBERGB] = &adobergb,
	[WESTON_CS_DCI_P3] = &dci_p3,
	[WESTON_CS_PROPHOTORGB] = &prophotorgb,
	[WESTON_CS_CIERGB] = &ciergb,
	[WESTON_CS_CIEXYZ] = &ciexyz,
	[WESTON_CS_AP0] = &ap0,
	[WESTON_CS_AP1] = &ap1,
};

WL_EXPORT const struct weston_colorspace *
weston_colorspace_lookup(enum weston_colorspace_enums colorspace)
{
	if (colorspace < 0 || colorspace >= WESTON_CS_UNDEFINED)
		return NULL;

	return colorspaces[colorspace];
}
