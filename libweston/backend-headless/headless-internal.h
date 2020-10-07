#include "config.h"

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/time.h>
#include <stdbool.h>
#include <drm_fourcc.h>

#include <libweston/libweston.h>
#include <libweston/backend.h>
#include <libweston/backend-headless.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

#ifdef BUILD_HEADLESS_GBM
#include <gbm.h>
#endif

#ifdef BUILD_HEADLESS_VIRTUAL
#include <libweston/backend-drm.h>
#endif

#include <libudev.h>
#include "libinput-seat.h"

#include "shared/helpers.h"
#include "linux-explicit-synchronization.h"
#include "linux-dmabuf.h"
#include "presentation-time-server-protocol.h"
#include <libweston/windowed-output-api.h>

enum headless_renderer_type {
	HEADLESS_NOOP,
	HEADLESS_PIXMAN,
	HEADLESS_GL,
	HEADLESS_GL_GBM,
};

struct headless_backend {
	struct weston_backend base;
	struct weston_compositor *compositor;

	struct weston_seat fake_seat;
	enum headless_renderer_type renderer_type;

	struct gl_renderer_interface *glri;

	int drm_fd;
	struct gbm_device *gbm;

	struct udev *udev;
	struct udev_input input;
	struct wl_listener session_listener;
};

struct headless_head {
	struct weston_head base;
};

struct headless_fb {
	int refcnt;

	uint32_t handles[4];
	uint32_t strides[4];
	int num_planes;
	uint32_t format;
	uint64_t modifier;
	int width, height;
	int fd;
	struct weston_buffer_reference buffer_ref;
	struct weston_buffer_release_reference buffer_release_ref;

	struct gbm_bo *bo;
	struct gbm_surface *gbm_surface;
};

struct headless_output {
	struct weston_output base;

	struct weston_mode mode;
	struct wl_event_source *finish_frame_timer;
	uint32_t *image_buf;
	pixman_image_t *image;

	struct gbm_surface *gbm_surface;
	uint32_t gbm_format;
	uint32_t gbm_bo_flags;

	struct headless_fb *prev_fb, *curr_fb;

	bool virtual;
#ifdef BUILD_HEADLESS_VIRTUAL
	submit_frame_cb virtual_submit_frame;
#endif
};

int
finish_frame_handler(void *data);

static const uint32_t headless_formats[] = {
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_ARGB8888,
};

static inline struct headless_head *
to_headless_head(struct weston_head *base)
{
	return container_of(base, struct headless_head, base);
}

static inline struct headless_output *
to_headless_output(struct weston_output *base)
{
	return container_of(base, struct headless_output, base);
}

static inline struct headless_backend *
to_headless_backend(struct weston_compositor *base)
{
	return container_of(base->backend, struct headless_backend, base);
}

#ifdef BUILD_HEADLESS_GBM
bool
gbm_create_device_headless(struct headless_backend *b);

struct headless_fb*
headless_fb_ref(struct headless_fb *fb);

void
headless_fb_unref(struct headless_fb *fb);

void
headless_fb_destroy_gbm(struct gbm_bo *bo, void *data);

struct headless_fb *
headless_fb_get_from_bo(struct gbm_bo *bo, struct headless_backend *b);

int
headless_output_repaint_gbm(struct headless_output *output,
                            pixman_region32_t *damage);

int
headless_gl_renderer_init_gbm(struct headless_backend *b);

int
headless_output_enable_gl_gbm(struct headless_output *output);

void
headless_output_disable_gl_gbm(struct headless_output *output);
#else
inline static bool
gbm_create_device_headless(struct headless_backend *b)
{
	return false;
}

inline static struct headless_fb*
headless_fb_ref(struct headless_fb *fb)
{
	return NULL;
}

inline static void
headless_fb_unref(struct headless_fb *fb)
{
}

inline static void
headless_fb_destroy_gbm(struct gbm_bo *bo, void *data)
{
}

inline static struct headless_fb *
headless_fb_get_from_bo(struct gbm_bo *bo, struct headless_backend *b)
{
	return NULL;
}

inline static int
headless_output_repaint_gbm(struct headless_output *output,
                            pixman_region32_t *damage);
{
	return 0;
}

inline static int
headless_gl_renderer_init_gbm(struct headless_backend *b)
{
	return 0;
}

inline static int
headless_output_enable_gl_gbm(struct headless_output *output)
{
	return 0;
}

inline static void
headless_output_disable_gl_gbm(struct headless_output *output)
{
}
#endif

#ifdef BUILD_HEADLESS_VIRTUAL
int
headless_backend_init_virtual_output_api(struct weston_compositor *ec);
#else
inline static int
headless_backend_init_virtual_output_api(struct weston_compositor *ec)
{
	return 0;
}
#endif
