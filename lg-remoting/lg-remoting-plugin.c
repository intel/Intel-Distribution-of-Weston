/*
 * Copyright © 2018 Renesas Electronics Corp.
 *
 * Based on vaapi-recorder by:
 *   Copyright (c) 2012 Intel Corporation. All Rights Reserved.
 *   Copyright © 2013 Intel Corporation
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
 *
 * Authors: IGEL Co., Ltd.
 */

#include "config.h"

#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <fcntl.h>

#include <drm_fourcc.h>

#include "remoting-plugin.h"
#include <libweston/backend-drm.h>
#include <libweston/backend-headless.h>
#include "shared/helpers.h"
#include "shared/timespec-util.h"
#include "backend.h"
#include "libweston-internal.h"
#include <lg-remote-server-protocol.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/dma-buf.h>

struct weston_remoting {
	struct weston_compositor *compositor;
	struct wl_list output_list;
	struct wl_listener destroy_listener;
	const struct weston_drm_virtual_output_api *virtual_output_api;

	struct wl_list resource_list;
	int fd;
	struct weston_buffer *buffer;
};

struct remoted_output {
	struct weston_output *output;
	void (*saved_destroy)(struct weston_output *output);
	int (*saved_enable)(struct weston_output *output);
	int (*saved_disable)(struct weston_output *output);
	int (*saved_start_repaint_loop)(struct weston_output *output);

	struct weston_head *head;

	struct weston_remoting *remoting;
	struct wl_event_source *finish_frame_timer;
	struct wl_list link;
	bool submitted_frame;
	int fence_sync_fd;
	struct wl_event_source *fence_sync_event_source;

	int retry_count;
};

struct mem_free_cb_data {
	struct remoted_output *output;
	void *output_buffer;
};

static int
remoting_output_disable(struct weston_output *output);

static void
remoting_output_buffer_release(struct remoted_output *output, void *buffer)
{
	const struct weston_drm_virtual_output_api *api
		= output->remoting->virtual_output_api;

	api->buffer_released(buffer);
}

static void
remoting_output_destroy(struct weston_output *output);

static void
weston_remoting_destroy(struct wl_listener *l, void *data)
{
	struct weston_remoting *remoting =
		container_of(l, struct weston_remoting, destroy_listener);
	struct remoted_output *output, *next;

	remoting->buffer = NULL;
	wl_list_for_each_safe(output, next, &remoting->output_list, link)
		remoting_output_destroy(output->output);

	wl_list_remove(&remoting->destroy_listener.link);
	free(remoting);
}

static struct weston_remoting *
weston_remoting_get(struct weston_compositor *compositor)
{
	struct wl_listener *listener;
	struct weston_remoting *remoting;

	listener = wl_signal_get(&compositor->destroy_signal,
				 weston_remoting_destroy);
	if (!listener)
		return NULL;

	remoting = wl_container_of(listener, remoting, destroy_listener);
	return remoting;
}

static int
remoting_output_finish_frame_handler(void *data)
{
	struct remoted_output *output = data;
	const struct weston_drm_virtual_output_api *api
		= output->remoting->virtual_output_api;
	struct timespec now;
	int64_t msec;
	struct wl_resource *resource;

	if (output->submitted_frame) {
		struct weston_compositor *c = output->remoting->compositor;
		output->submitted_frame = false;
		weston_compositor_read_presentation_clock(c, &now);
		api->finish_frame(output->output, &now, 0);
		wl_resource_for_each(resource, &output->remoting->resource_list) {
			lg_remote_send_frame_done(resource);
		}
	}

	msec = millihz_to_nsec(output->output->current_mode->refresh) / 1000000;
	wl_event_source_timer_update(output->finish_frame_timer, msec);
	return 0;
}

static struct remoted_output *
lookup_remoted_output(struct weston_output *output)
{
	struct weston_compositor *c = output->compositor;
	struct weston_remoting *remoting = weston_remoting_get(c);
	struct remoted_output *remoted_output;

	wl_list_for_each(remoted_output, &remoting->output_list, link) {
		if (remoted_output->output == output)
			return remoted_output;
	}

	weston_log("%s: %s: could not find output\n", __FILE__, __func__);
	return NULL;
}


static int
remoting_output_fence_sync_handler(int fd, uint32_t mask, void *data)
{
	struct mem_free_cb_data *cb_data = data;
	struct remoted_output *output = cb_data->output;
	struct weston_remoting *remoting = output->remoting;
	struct wl_resource *resource;
	void *frame;
	int size = output->output->current_mode->width*output->output->current_mode->height*4;
	struct dma_buf_sync sync;
	uint8_t *d;

	if(remoting->buffer) {
		d = wl_shm_buffer_get_data(remoting->buffer->shm_buffer);
		wl_shm_buffer_begin_access(remoting->buffer->shm_buffer);

		frame = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED,
				remoting->fd, 0);

		sync.flags = DMA_BUF_SYNC_START;
		sync.flags |= DMA_BUF_SYNC_READ;
		ioctl(remoting->fd, DMA_BUF_IOCTL_SYNC, &sync);

		memcpy(d, frame, size);

		sync.flags = DMA_BUF_SYNC_END;
		ioctl(remoting->fd, DMA_BUF_IOCTL_SYNC, &sync);

		munmap(frame, size);
		wl_resource_for_each(resource, &remoting->resource_list) {
			lg_remote_send_done(resource);
		}
		wl_shm_buffer_end_access(remoting->buffer->shm_buffer);
		remoting->buffer = NULL;
	}

	output->submitted_frame = true;
	wl_event_source_remove(output->fence_sync_event_source);
	close(output->fence_sync_fd);
	close(remoting->fd);

	remoting_output_buffer_release(output, cb_data->output_buffer);
	free(cb_data);

	return 0;
}

static int
remoting_output_frame(struct weston_output *output_base, int fd, int stride,
		      void *output_buffer)
{
	struct remoted_output *output = lookup_remoted_output(output_base);
	struct weston_remoting *remoting = output->remoting;
	const struct weston_drm_virtual_output_api *api
		= output->remoting->virtual_output_api;
	struct wl_event_loop *loop;
	struct mem_free_cb_data *cb_data;

	if (!output)
		return -1;

	cb_data = zalloc(sizeof *cb_data);
	if (!cb_data)
		return -1;

	cb_data->output = output;
	cb_data->output_buffer = output_buffer;

	remoting->fd = fd;
	output->fence_sync_fd = api->get_fence_sync_fd(output->output);
	if (output->fence_sync_fd == -1) {
		output->submitted_frame = true;
		close(fd);
		free(cb_data);
		return 0;
	}


	loop = wl_display_get_event_loop(remoting->compositor->wl_display);
	output->fence_sync_event_source =
		wl_event_loop_add_fd(loop, output->fence_sync_fd,
				     WL_EVENT_READABLE,
				     remoting_output_fence_sync_handler,
				     cb_data);
	return 0;
}

static void
remoting_output_destroy(struct weston_output *output)
{
	struct remoted_output *remoted_output = lookup_remoted_output(output);
	struct weston_mode *mode, *next;

	wl_list_for_each_safe(mode, next, &output->mode_list, link) {
		wl_list_remove(&mode->link);
		free(mode);
	}

	remoted_output->saved_destroy(output);

	wl_list_remove(&remoted_output->link);
	weston_head_release(remoted_output->head);
	free(remoted_output->head);
	free(remoted_output);
}

static int
remoting_output_start_repaint_loop(struct weston_output *output)
{
	struct remoted_output *remoted_output = lookup_remoted_output(output);
	int64_t msec;

	remoted_output->saved_start_repaint_loop(output);

	msec = millihz_to_nsec(remoted_output->output->current_mode->refresh)
			/ 1000000;
	wl_event_source_timer_update(remoted_output->finish_frame_timer, msec);

	return 0;
}

static int
remoting_output_enable(struct weston_output *output)
{
	struct remoted_output *remoted_output = lookup_remoted_output(output);
	struct weston_compositor *c = output->compositor;
	const struct weston_drm_virtual_output_api *api
		= remoted_output->remoting->virtual_output_api;
	struct wl_event_loop *loop;
	int ret;

	api->set_submit_frame_cb(output, remoting_output_frame);

	ret = remoted_output->saved_enable(output);
	if (ret < 0)
		return ret;

	remoted_output->saved_start_repaint_loop = output->start_repaint_loop;
	output->start_repaint_loop = remoting_output_start_repaint_loop;

	loop = wl_display_get_event_loop(c->wl_display);
	remoted_output->finish_frame_timer =
		wl_event_loop_add_timer(loop,
					remoting_output_finish_frame_handler,
					remoted_output);

	return 0;
}

static int
remoting_output_disable(struct weston_output *output)
{
	struct remoted_output *remoted_output = lookup_remoted_output(output);

	wl_event_source_remove(remoted_output->finish_frame_timer);

	return remoted_output->saved_disable(output);
}

static struct weston_output *
remoting_output_create(struct weston_compositor *c, char *name)
{
	struct weston_remoting *remoting = weston_remoting_get(c);
	struct remoted_output *output;
	struct weston_head *head;
	const struct weston_drm_virtual_output_api *api;
	const char *make = "Intel";
	const char *model = "Virtual Display";
	const char *serial_number = "unknown";
	const char *connector_name = "remoting";

	if (!name || !strlen(name))
		return NULL;

	api = remoting->virtual_output_api;

	output = zalloc(sizeof *output);
	if (!output)
		return NULL;

	head = zalloc(sizeof *head);
	if (!head)
		goto err;

	output->output = api->create_output(c, name);
	if (!output->output) {
		weston_log("Can not create virtual output\n");
		goto err;
	}

	output->saved_destroy = output->output->destroy;
	output->output->destroy = remoting_output_destroy;
	output->saved_enable = output->output->enable;
	output->output->enable = remoting_output_enable;
	output->saved_disable = output->output->disable;
	output->output->disable = remoting_output_disable;
	output->remoting = remoting;
	wl_list_insert(remoting->output_list.prev, &output->link);

	weston_head_init(head, connector_name);
	weston_head_set_subpixel(head, WL_OUTPUT_SUBPIXEL_NONE);
	weston_head_set_monitor_strings(head, make, model, serial_number);
	head->compositor = c;

	weston_output_attach_head(output->output, head);
	output->head = head;

	return output->output;

err:
	if (head)
		free(head);
	free(output);
	return NULL;
}

static bool
remoting_output_is_remoted(struct weston_output *output)
{
	struct remoted_output *remoted_output = lookup_remoted_output(output);

	if (remoted_output)
		return true;

	return false;
}

static int
remoting_output_set_mode(struct weston_output *output, const char *modeline)
{
	struct weston_mode *mode;
	int n, width, height, refresh = 0;
	struct weston_head *head = NULL;

	if (!remoting_output_is_remoted(output)) {
		weston_log("Output is not remoted.\n");
		return -1;
	}

	if (!modeline)
		return -1;

	n = sscanf(modeline, "%dx%d@%d", &width, &height, &refresh);
	if (n != 2 && n != 3)
		return -1;

	mode = zalloc(sizeof *mode);
	if (!mode)
		return -1;

	mode->flags = WL_OUTPUT_MODE_CURRENT;
	mode->width = width;
	mode->height = height;
	mode->refresh = (refresh ? refresh : 60) * 1000LL;

	wl_list_insert(output->mode_list.prev, &mode->link);

	output->current_mode = mode;

	while ((head = weston_output_iterate_heads(output, head))) {
		weston_head_set_physical_size(head, width, height);
	}

	return 0;
}

static void
remoting_output_set_gbm_format(struct weston_output *output,
			       const char *gbm_format)
{
}

static void
remoting_output_set_seat(struct weston_output *output, const char *seat)
{
	/* for now, nothing todo */
}

static void
remoting_output_set_host(struct weston_output *output, char *host)
{
}

static void
remoting_output_set_port(struct weston_output *output, int port)
{
}

static void
remoting_output_set_gst_pipeline(struct weston_output *output,
				 char *gst_pipeline)
{
}

static const struct weston_remoting_api remoting_api = {
	remoting_output_create,
	remoting_output_is_remoted,
	remoting_output_set_mode,
	remoting_output_set_gbm_format,
	remoting_output_set_seat,
	remoting_output_set_host,
	remoting_output_set_port,
	remoting_output_set_gst_pipeline,
};

static void remote_capture(struct wl_client *client,
			struct wl_resource *resource,
			struct wl_resource *output_resource,
			struct wl_resource *buffer_resource)
{
	struct weston_remoting *remoting;
	struct weston_buffer *buffer =
		weston_buffer_from_resource(buffer_resource);
	struct weston_output *output =
		weston_head_from_resource(output_resource)->output;

	if (buffer == NULL) {
		wl_resource_post_no_memory(resource);
		return;
	}

	buffer->shm_buffer = wl_shm_buffer_get(buffer_resource);
	if (!buffer->shm_buffer) {
		return;
	}

	remoting = wl_resource_get_user_data(resource);
	remoting->buffer = buffer;
	weston_output_damage(output);
}

static const struct lg_remote_interface remote_implementation = {
	remote_capture,
};

static void unbind_resource(struct wl_resource *resource)
{
	wl_list_remove(wl_resource_get_link(resource));
}

static void
bind_lg_remote(struct wl_client *client,
		void *data, uint32_t version, uint32_t id)
{
	struct weston_remoting *remoting = data;
	struct wl_resource *resource;

	resource = wl_resource_create(client, &lg_remote_interface,
				      version, id);
	if (resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &remote_implementation,
				       remoting, unbind_resource);
	wl_list_insert(&remoting->resource_list, wl_resource_get_link(resource));
}

WL_EXPORT int
weston_module_init(struct weston_compositor *compositor)
{
	int ret;
	struct weston_remoting *remoting;
	const struct weston_drm_virtual_output_api *headless_api = NULL;
	const struct weston_drm_virtual_output_api *drm_api =
		weston_drm_virtual_output_get_api(compositor);

	remoting = zalloc(sizeof *remoting);
	if (!remoting)
		return -1;

	wl_list_init(&remoting->resource_list);

	if (!weston_compositor_add_destroy_listener_once(compositor,
							 &remoting->destroy_listener,
							 weston_remoting_destroy)) {
		free(remoting);
		return 0;
	}

	remoting->virtual_output_api = drm_api;
	if (!remoting->virtual_output_api) {
		headless_api = weston_headless_virtual_output_get_api(compositor);

		if (!headless_api)
			return -1;

		remoting->virtual_output_api = headless_api;
	}

	remoting->compositor = compositor;
	wl_list_init(&remoting->output_list);

	ret = weston_plugin_api_register(compositor, WESTON_REMOTING_API_NAME,
					 &remoting_api, sizeof(remoting_api));

	if (ret < 0) {
		weston_log("Failed to register remoting API.\n");
		goto failed;
	}

	if (!wl_global_create(compositor->wl_display, &lg_remote_interface, 1,
			      remoting, bind_lg_remote))
		goto failed;

	return 0;

failed:
	wl_list_remove(&remoting->destroy_listener.link);
	free(remoting);
	return -1;
}
