/*
 * Copyright © 2018 Intel Corporation
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
#include <libweston/hdr_metadata_defs.h>
#include "hdr-metadata-unstable-v1-server-protocol.h"

#define STATIC_METADATA(x) data->metadata.static_metadata.x

static void
hdr_surface_set_metadata(struct wl_client *client,
			 struct wl_resource *surface_resource,
			 wl_fixed_t display_primary_r_x,
			 wl_fixed_t display_primary_r_y,
			 wl_fixed_t display_primary_g_x,
			 wl_fixed_t display_primary_g_y,
			 wl_fixed_t display_primary_b_x,
			 wl_fixed_t display_primary_b_y,
			 wl_fixed_t white_point_x,
			 wl_fixed_t white_point_y,
			 wl_fixed_t max_luminance,
			 wl_fixed_t min_luminance,
			 uint32_t max_cll,
			 uint32_t max_fall)
{
	struct weston_surface *surface =
		wl_resource_get_user_data(surface_resource);

	struct weston_hdr_metadata *data = surface->pending.hdr_metadata;
	data->metadata_type = HDR_METADATA_TYPE1;
	STATIC_METADATA(primaries.r.x) = wl_fixed_to_double(display_primary_r_x);
	STATIC_METADATA(primaries.r.y) = wl_fixed_to_double(display_primary_r_y);
	STATIC_METADATA(primaries.g.x) = wl_fixed_to_double(display_primary_g_x);
	STATIC_METADATA(primaries.g.y) = wl_fixed_to_double(display_primary_g_y);
	STATIC_METADATA(primaries.b.x) = wl_fixed_to_double(display_primary_b_x);
	STATIC_METADATA(primaries.b.y) = wl_fixed_to_double(display_primary_b_y);
	STATIC_METADATA(primaries.white_point.x) = wl_fixed_to_double(white_point_x);
	STATIC_METADATA(primaries.white_point.y) = wl_fixed_to_double(white_point_y);
	STATIC_METADATA(max_luminance) = wl_fixed_to_double(max_luminance);
	STATIC_METADATA(min_luminance) = wl_fixed_to_double(min_luminance);
	STATIC_METADATA(max_cll) = max_cll;
	STATIC_METADATA(max_fall) = max_fall;
}

static void
hdr_surface_set_eotf(struct wl_client *client,
		     struct wl_resource *surface_resource,
		     uint32_t eotf)
{
	enum hdr_metadata_eotf internal_eotf = WESTON_EOTF_TRADITIONAL_GAMMA_SDR;
	struct weston_surface *surface =
		wl_resource_get_user_data(surface_resource);

	struct weston_hdr_metadata *data = surface->pending.hdr_metadata;


	switch (eotf) {
	case ZWP_HDR_SURFACE_V1_EOTF_ST_2084_PQ:
		internal_eotf = WESTON_EOTF_ST2084;
		break;
	case ZWP_HDR_SURFACE_V1_EOTF_HLG:
		internal_eotf = WESTON_EOTF_HLG;
		break;
	}

	data->metadata_type = HDR_METADATA_TYPE1;
	STATIC_METADATA(eotf) = internal_eotf;
}

static void
hdr_surface_destroy(struct wl_client *client,
		    struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static const struct zwp_hdr_surface_v1_interface
zwp_hdr_surface_implementation = {
	.destroy = hdr_surface_destroy,
	.set = hdr_surface_set_metadata,
	.set_eotf = hdr_surface_set_eotf,
};


static void
hdr_metadata_destroy_request(struct wl_client *client,
			     struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static void
destroy_hdr_surface(struct wl_resource *resource)
{
	struct weston_surface *surface =
		wl_resource_get_user_data(resource);

	if (!surface)
		return;

	surface->hdr_surface_resource = NULL;
	if (surface->pending.hdr_metadata)
		free(surface->pending.hdr_metadata);
	surface->pending.hdr_metadata = NULL;
}

static void
hdr_metadata_get_hdr_surface(struct wl_client *client,
			     struct wl_resource *hdr_metadata,
			     uint32_t id,
			     struct wl_resource *surface_resource)
{
	int version = wl_resource_get_version(hdr_metadata);
	struct weston_surface *surface =
		wl_resource_get_user_data(surface_resource);

	struct wl_resource *resource;

	if (surface->hdr_surface_resource) {
		wl_resource_post_error(hdr_metadata,
				       ZWP_HDR_METADATA_V1_ERROR_HDR_SURFACE_EXISTS,
				       "a hdr surface for that surface already exists");
		return;
	}

	resource = wl_resource_create(client, &zwp_hdr_surface_v1_interface,
				      version, id);
	if (resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}

	wl_resource_set_implementation(resource, &zwp_hdr_surface_implementation,
				       surface, destroy_hdr_surface);

	surface->hdr_surface_resource = resource;
	surface->pending.hdr_metadata =
		zalloc(sizeof(struct weston_hdr_metadata));

	if (!surface->pending.hdr_metadata) {
		wl_client_post_no_memory(client);
		return;
	}
}

static const struct zwp_hdr_metadata_v1_interface
zwp_hdr_metadata_implementation = {
	.destroy = hdr_metadata_destroy_request,
	.get_hdr_surface = hdr_metadata_get_hdr_surface,
};

static void
bind_hdr_metadata(struct wl_client *client,
		void *data, uint32_t version, uint32_t id)
{
	struct wl_resource *resource;
	resource = wl_resource_create(client, &zwp_hdr_metadata_v1_interface,
				      version, id);
	if (resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}

	wl_resource_set_implementation(resource,
				       &zwp_hdr_metadata_implementation,
				       NULL, NULL);
}

WL_EXPORT int
weston_hdr_metadata_setup(struct weston_compositor *compositor)
{
	if (!wl_global_create(compositor->wl_display,
			      &zwp_hdr_metadata_v1_interface, 1,
			      compositor, bind_hdr_metadata))
		return -1;

	return 0;
}
