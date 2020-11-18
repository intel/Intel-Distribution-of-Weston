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

#include <string.h>

#include <libweston/libweston.h>

#include "colorspace-unstable-v1-server-protocol.h"

static void
colorspace_destroy_request(struct wl_client *client,
			   struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static void
colorspace_set_request(struct wl_client *client,
		       struct wl_resource *resource,
		       struct wl_resource *surface_resource,
		       uint32_t chromacities)
{
	static uint32_t colorspace_names[] = {
		[ZWP_COLORSPACE_V1_CHROMACITIES_UNDEFINED] = WESTON_CS_UNDEFINED,
		[ZWP_COLORSPACE_V1_CHROMACITIES_BT470M] = WESTON_CS_BT470M,
		[ZWP_COLORSPACE_V1_CHROMACITIES_BT470BG] = WESTON_CS_BT470BG,
		[ZWP_COLORSPACE_V1_CHROMACITIES_SMPTE170M] = WESTON_CS_SMPTE170M,
		[ZWP_COLORSPACE_V1_CHROMACITIES_BT709] = WESTON_CS_BT709,
		[ZWP_COLORSPACE_V1_CHROMACITIES_BT2020] = WESTON_CS_BT2020,
		[ZWP_COLORSPACE_V1_CHROMACITIES_ADOBERGB] = WESTON_CS_ADOBERGB,
		[ZWP_COLORSPACE_V1_CHROMACITIES_DCI_P3] = WESTON_CS_DCI_P3,
		[ZWP_COLORSPACE_V1_CHROMACITIES_PROPHOTORGB] = WESTON_CS_PROPHOTORGB,
		[ZWP_COLORSPACE_V1_CHROMACITIES_CIERGB] = WESTON_CS_CIERGB,
		[ZWP_COLORSPACE_V1_CHROMACITIES_CIEXYZ] = WESTON_CS_CIEXYZ,
		[ZWP_COLORSPACE_V1_CHROMACITIES_AP0] = WESTON_CS_AP0,
		[ZWP_COLORSPACE_V1_CHROMACITIES_AP1] = WESTON_CS_AP1,
	};

	struct weston_surface *surface =
		wl_resource_get_user_data(surface_resource);

	if (chromacities > ZWP_COLORSPACE_V1_CHROMACITIES_AP1)
		chromacities = ZWP_COLORSPACE_V1_CHROMACITIES_UNDEFINED;

	surface->pending.colorspace = colorspace_names[chromacities];
}

static const struct zwp_colorspace_v1_interface
zwp_colorspace_implementation = {
	.destroy = colorspace_destroy_request,
	.set = colorspace_set_request,
};

static void
bind_colorspace(struct wl_client *client,
		void *data, uint32_t version, uint32_t id)
{
	struct weston_compositor *compositor = data;
	struct wl_resource *resource;

	resource = wl_resource_create(client, &zwp_colorspace_v1_interface,
				      version, id);
	if (resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}

	wl_resource_set_implementation(resource, &zwp_colorspace_implementation,
				       compositor, NULL);
}

WL_EXPORT int
weston_colorspace_setup(struct weston_compositor *compositor)
{
	if (!wl_global_create(compositor->wl_display,
			      &zwp_colorspace_v1_interface, 1,
			      compositor, bind_colorspace))
		return -1;

	return 0;
}
