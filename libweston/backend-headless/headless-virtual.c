#include "config.h"

#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>

#include "headless-internal.h"
#include "renderer-gl/gl-renderer.h"

static int
headless_virtual_output_start_repaint_loop(struct weston_output *output_base)
{
	weston_output_finish_frame(output_base, NULL,
				   WP_PRESENTATION_FEEDBACK_INVALID);

	return 0;
}

static int
headless_virtual_output_submit_frame(struct headless_output *output,
				     void *_fb)
{
	struct headless_backend *b = to_headless_backend(output->base.compositor);
	struct headless_fb* fb = _fb;
	int fd, ret;

	assert(fb->num_planes == 1);
	ret = drmPrimeHandleToFD(b->drm_fd, fb->handles[0], DRM_CLOEXEC, &fd);
	if (ret) {
		weston_log("drmPrimeHandleFD failed, errno=%d\n", errno);
		return -1;
	}

	headless_fb_ref(fb);
	ret = output->virtual_submit_frame(&output->base, fd, fb->strides[0],
					   fb);
	if (ret < 0) {
		headless_fb_unref(fb);
		close(fd);
	}

	return ret;
}

static int
headless_virtual_output_repaint(struct weston_output *output_base,
			   	pixman_region32_t *damage,
			   	void *repaint_data)
{
	struct headless_output *output = to_headless_output(output_base);

	assert(output->virtual);

	/* Drop frame if there isn't free buffers */
	if (!gbm_surface_has_free_buffers(output->gbm_surface)) {
		weston_log("%s: Drop frame!!\n", __func__);
		return -1;
	}

	headless_output_repaint_gbm(output, damage);
	if (headless_virtual_output_submit_frame(output, output->curr_fb) < 0)
		return -1;

	return 0;
}

static void
headless_virtual_output_deinit(struct weston_output *base)
{
	struct headless_output *output = to_headless_output(base);

	headless_output_disable_gl_gbm(output);
}

static void
headless_virtual_output_destroy(struct weston_output *base)
{
	struct headless_output *output = to_headless_output(base);

	assert(output->virtual);

	if (output->base.enabled)
		headless_virtual_output_deinit(&output->base);

	weston_output_release(&output->base);

	free(output);
}

static int
headless_virtual_output_enable(struct weston_output *output_base)
{
	struct headless_output *output = to_headless_output(output_base);
	struct headless_backend *b = to_headless_backend(output_base->compositor);
	struct wl_event_loop *loop;

	assert(output->virtual);

	if (b->renderer_type != HEADLESS_GL_GBM) {
		weston_log("Cannot enable Virtual outputs without GBM\n");
		goto err;
	}

	if (!output->virtual_submit_frame) {
		weston_log("The virtual_submit_frame hook is not set\n");
		goto err;
	}

	if (output->finish_frame_timer)
		wl_event_source_remove(output->finish_frame_timer);

	loop = wl_display_get_event_loop(b->compositor->wl_display);
	output->finish_frame_timer =
		wl_event_loop_add_timer(loop, finish_frame_handler, output);

	if (headless_output_enable_gl_gbm(output) < 0) {
		weston_log("Failed to init output gl state\n");
		goto err;
	}

	output->base.start_repaint_loop = headless_virtual_output_start_repaint_loop;
	output->base.repaint = headless_virtual_output_repaint;
	output->base.assign_planes = NULL;
	output->base.set_dpms = NULL;
	output->base.switch_mode = NULL;
	output->base.gamma_size = 0;
	output->base.set_gamma = NULL;

	return 0;
err:
	wl_event_source_remove(output->finish_frame_timer);
	return -1;
}

static int
headless_virtual_output_disable(struct weston_output *base)
{
	struct headless_output *output = to_headless_output(base);

	assert(output->virtual);

	if (output->base.enabled)
		headless_virtual_output_deinit(&output->base);

	return 0;
}

static struct weston_output *
headless_virtual_output_create(struct weston_compositor *c, char *name)
{
	struct headless_output *output;

	output = zalloc(sizeof *output);
	if (!output)
		return NULL;

	output->virtual = true;
	output->gbm_bo_flags = GBM_BO_USE_LINEAR | GBM_BO_USE_RENDERING;

	weston_output_init(&output->base, c, name);

	output->base.enable = headless_virtual_output_enable;
	output->base.destroy = headless_virtual_output_destroy;
	output->base.disable = headless_virtual_output_disable;
	output->base.attach_head = NULL;

	weston_compositor_add_pending_output(&output->base, c);

	return &output->base;
}

static uint32_t
headless_virtual_output_set_gbm_format(struct weston_output *base,
				       const char *gbm_format)
{
	struct headless_output *output = to_headless_output(base);

	output->gbm_format = DRM_FORMAT_XRGB8888;

	return output->gbm_format;
}

static void
headless_virtual_output_set_submit_frame_cb(struct weston_output *output_base,
				       	    submit_frame_cb cb)
{
	struct headless_output *output = to_headless_output(output_base);

	output->virtual_submit_frame = cb;
}

static int
headless_virtual_output_get_fence_fd(struct weston_output *output_base)
{
	struct headless_backend *b = to_headless_backend(output_base->compositor);

	return b->glri->create_fence_fd(output_base);
}

static void
headless_virtual_output_buffer_released(void *_fb)
{
	struct headless_fb* fb = _fb;

	headless_fb_unref(fb);
}

static void
headless_virtual_output_finish_frame(struct weston_output *output_base,
				     struct timespec *stamp,
				     uint32_t presented_flags)
{
	weston_output_finish_frame(output_base, stamp, presented_flags);
}

static const struct weston_drm_virtual_output_api virt_api = {
	headless_virtual_output_create,
	headless_virtual_output_set_gbm_format,
	headless_virtual_output_set_submit_frame_cb,
	headless_virtual_output_get_fence_fd,
	headless_virtual_output_buffer_released,
	headless_virtual_output_finish_frame
};

int headless_backend_init_virtual_output_api(struct weston_compositor *compositor)
{
	return weston_plugin_api_register(compositor,
					  WESTON_HEADLESS_VIRTUAL_OUTPUT_API_NAME,
					  &virt_api, sizeof(virt_api));
}

