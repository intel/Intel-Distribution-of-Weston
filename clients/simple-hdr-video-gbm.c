/*
 * Copyright © 2019 Harish Krupo
 * Copyright © 2019 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "config.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cairo.h>
#include <math.h>
#include <assert.h>
#include <sys/mman.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>

#include <linux/input.h>
#include <wayland-client.h>

#include <wayland-egl.h>
#include <GLES2/gl2.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/mastering_display_metadata.h>

#include <xf86drm.h>
#include <drm_fourcc.h>

#include <gbm.h>

#ifdef HAVE_PANGO
#include <pango/pangocairo.h>
#endif

#include "colorspace-unstable-v1-client-protocol.h"
#include "hdr-metadata-unstable-v1-client-protocol.h"
#include "linux-dmabuf-unstable-v1-client-protocol.h"

#include "shared/os-compatibility.h"
#include "shared/helpers.h"
#include "shared/platform.h"
#include "shared/xalloc.h"
#include <libweston/zalloc.h>
#include "window.h"

#define DBG(fmt, ...) \
	fprintf(stderr, "%d:%s " fmt, __LINE__, __func__, ##__VA_ARGS__)

#define NUM_BUFFERS 3

#ifndef DRM_FORMAT_MOD_LINEAR
#define DRM_FORMAT_MOD_LINEAR 0
#endif

#ifndef DRM_FORMAT_P010
#define DRM_FORMAT_P010         fourcc_code('P', '0', '1', '0') /* 2x2 subsampled Cb:Cr plane 10 bits per channel */
#endif

#ifndef DRM_FORMAT_P012
#define DRM_FORMAT_P012		fourcc_code('P', '0', '1', '2') /* 2x2 subsampled Cr:Cb plane, 12 bit per channel */
#endif

#ifndef DRM_FORMAT_P016
#define DRM_FORMAT_P016		fourcc_code('P', '0', '1', '6') /* 2x2 subsampled Cr:Cb plane, 16 bit per channel */
#endif

static int32_t option_help;
static int32_t option_fullscreen;
static int32_t option_subtitle;

static const struct weston_option options[] = {
	{ WESTON_OPTION_BOOLEAN, "fullscreen", 'f', &option_fullscreen },
	{ WESTON_OPTION_BOOLEAN, "subtitle", 's', &option_subtitle },
	{ WESTON_OPTION_BOOLEAN, "help", 'h', &option_help },
};

static const char help_text[] =
"Usage: %s [options] FILENAME\n"
"\n"
"  -f, --fullscreen\t\tRun in fullscreen mode\n"
"  -s, --subtitle\t\tShow subtiles\n"
"  -h, --help\t\tShow this help text\n"
"\n";

struct app;
struct buffer;

struct buffer {
	struct wl_buffer *buffer;
	int busy;

	int drm_fd;
	uint32_t gem_handle;
	int dmabuf_fd;
	uint8_t *mmap;

	int width;
	int height;
	int bpp;
	unsigned long stride;
	int format;
	AVFrame *prev_frame;

	struct gbm_device *gbm;
	int cpp;
	struct gbm_bo *bo;
};

struct subtitle {

	struct wl_surface *wl_surface;
	int width;
	int height;

	struct widget *widget;
	uint32_t time;
	struct wl_callback *frame_cb;
	struct app *app;
	struct buffer buffers[NUM_BUFFERS];
	struct buffer *prev_buffer;
};

struct video {
	AVFormatContext *fmt_ctx;
	AVCodecParserContext *parser;
	AVCodecContext *codec;
	AVPacket *pkt;
	int stream_index;

	struct buffer buffers[NUM_BUFFERS];
	struct buffer *prev_buffer;
};

struct app {
	struct display *display;
	struct window *window;
	struct widget *widget;
	struct video video;

	struct subtitle *subtitle;

	struct zwp_colorspace_v1 *colorspace;
	struct zwp_hdr_metadata_v1 *hdr_metadata;

	struct zwp_hdr_surface_v1 *hdr_surface;
	struct zwp_linux_dmabuf_v1 *dmabuf;
};

static int
create_dmabuf_buffer(struct app *app, struct buffer *buffer,
		     int width, int height, int format);

static void
gbm_fini(struct buffer *my_buf);

static void
destroy_dmabuf_buffer(struct buffer *buffer)
{
	if (buffer->buffer) {
		wl_buffer_destroy(buffer->buffer);
		close(buffer->dmabuf_fd);
		gbm_fini(buffer);
	}
}

static void
subtitle_resize_handler(struct widget *widget,
			int32_t width, int32_t height, void *data)
{
	struct subtitle *sub = data;
	struct app *app = sub->app;
	struct rectangle allocation;
	struct wl_surface *surface;
	uint32_t format;
	int i;

	widget_get_allocation(sub->widget, &allocation);

	surface = widget_get_wl_surface(widget);

	// clear surface's buffer
	wl_surface_attach(surface, NULL, 0, 0);
	for (i = 0; i < NUM_BUFFERS; i++) {
		destroy_dmabuf_buffer(&sub->buffers[i]);
	}

	format = DRM_FORMAT_ARGB8888;

	for (i = 0; i < NUM_BUFFERS; i++) {
		create_dmabuf_buffer(app, &sub->buffers[i],
				     allocation.width,
				     allocation.height,
				     format);

	}

}

static struct buffer *
subtitle_next_buffer(struct subtitle *sub)
{
	int i;

	for (i = 0; i < NUM_BUFFERS; i++)
		if (!sub->buffers[i].busy)
			return &sub->buffers[i];

	return NULL;
}

#ifdef HAVE_PANGO
static PangoLayout *
create_layout(cairo_t *cr, const char *title)
{
	PangoLayout *layout;
	PangoFontDescription *desc;

	layout = pango_cairo_create_layout(cr);
	pango_layout_set_text(layout, title, -1);
	desc = pango_font_description_from_string("Sans Bold 19");
	pango_layout_set_font_description(layout, desc);
	pango_font_description_free(desc);
	pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
	pango_layout_set_alignment(layout, PANGO_ALIGN_LEFT);
	pango_layout_set_auto_dir (layout, FALSE);
	pango_layout_set_single_paragraph_mode (layout, TRUE);
	pango_layout_set_width (layout, -1);

	return layout;
}
#endif

static void
fill_subtitle(struct buffer *buffer)
{
	cairo_surface_t *surface;
	cairo_t* cr;
	char *title = "Sample subtitle string";
	PangoLayout *title_layout;

	assert(buffer->mmap);

	surface = cairo_image_surface_create_for_data(buffer->mmap,
						      CAIRO_FORMAT_ARGB32,
						      buffer->width,
						      buffer->height,
						      buffer->stride);
	cr = cairo_create(surface);
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
#ifdef HAVE_PANGO
	cairo_set_source_rgba(cr, 0, 0, 0, 0);
#else
	cairo_set_source_rgba(cr, 1, 1, 1, 1);
#endif

	cairo_paint(cr);

#ifdef HAVE_PANGO
	/* cairo_set_operator(cr, CAIRO_OPERATOR_OVER); */
	title_layout = create_layout(cr, title);
	cairo_move_to(cr, 0, 0);
	cairo_set_source_rgb(cr, 1, 1, 1);
	pango_cairo_show_layout(cr, title_layout);
#endif

	cairo_destroy(cr);
	cairo_surface_destroy(surface);
}

static void
subtitle_redraw_handler(struct widget *widget, void *data)
{
	struct subtitle *sub = data;
	struct rectangle allocation;
	struct buffer *buffer;
	struct wl_surface *surface;
	void *map_data = NULL;
	uint32_t dst_stride;

	widget_get_allocation(sub->widget, &allocation);
	buffer = subtitle_next_buffer(sub);

	buffer->mmap = gbm_bo_map(buffer->bo, 0, 0, buffer->width, buffer->height,
				  GBM_BO_TRANSFER_WRITE, &dst_stride, &map_data);
	if (!buffer->mmap) {
		fprintf(stderr, "failed to map failed\n");
		return;
	}

	fill_subtitle(buffer);

	gbm_bo_unmap(buffer->bo, map_data);

	surface = widget_get_wl_surface(widget);
	wl_surface_attach(surface, buffer->buffer, 0, 0);
	wl_surface_damage(surface, 0, 0, allocation.width, allocation.height);
	wl_surface_commit(surface);
	buffer->busy = 1;

	widget_schedule_redraw(sub->widget);
}

static struct subtitle *
subtitle_create(struct app *app)
{
	struct subtitle *sub;

	sub = xzalloc(sizeof *sub);
	sub->app = app;

	sub->widget = window_add_subsurface(app->window, sub,
					    SUBSURFACE_SYNCHRONIZED);

	widget_set_use_cairo(sub->widget, 0);
	widget_set_resize_handler(sub->widget, subtitle_resize_handler);
	widget_set_redraw_handler(sub->widget, subtitle_redraw_handler);

	return sub;
}

static void
subtitle_destroy(struct subtitle *sub)
{
	int i;

	for (i = 0; i < NUM_BUFFERS; i++) {
		destroy_dmabuf_buffer(&sub->buffers[i]);
	}

	widget_destroy(sub->widget);
	free(sub);
}

static void
buffer_release(void *data, struct wl_buffer *buffer)
{
	struct buffer *mybuf = data;
	mybuf->busy = 0;
	if (mybuf->prev_frame)
		av_frame_free(&mybuf->prev_frame);

	mybuf->prev_frame = NULL;
}

static const struct wl_buffer_listener buffer_listener = {
	buffer_release
};

static uint32_t av_format_to_drm_format(int format)
{
	switch (format) {
	case AV_PIX_FMT_YUV420P:
		return DRM_FORMAT_YUV420;
	case AV_PIX_FMT_YUV420P10BE:
	case AV_PIX_FMT_YUV420P10LE:
		return DRM_FORMAT_P010;
	case AV_PIX_FMT_YUV420P12BE:
	case AV_PIX_FMT_YUV420P12LE:
		return DRM_FORMAT_P012;
	case AV_PIX_FMT_YUV420P16BE:
	case AV_PIX_FMT_YUV420P16LE:
		return DRM_FORMAT_P016;
	default:
		return -1;
	}
}

static inline void
ensure_hdr_surface(struct app *app)
{
	struct window *window = app->window;
	struct wl_surface *surface;

	assert(app->hdr_metadata);
	if (app->hdr_surface)
		return;

	surface = window_get_wl_surface(window);
	app->hdr_surface = zwp_hdr_metadata_v1_get_hdr_surface(app->hdr_metadata,
							       surface);
}

static inline void
destroy_hdr_surface(struct app *app)
{
	if (app->hdr_surface) {
		zwp_hdr_surface_v1_destroy(app->hdr_surface);
		app->hdr_surface = NULL;
	}
}

static bool
decode(struct video *s, AVFrame *frame)
{
	int r;

	if (s->pkt->size == 0)
		return false;

	if (s->pkt->stream_index != s->stream_index)
		return false;

	r = avcodec_send_packet(s->codec, s->pkt);
	if (r < 0)
		return false;

	r = avcodec_receive_frame(s->codec, frame);
	if (r < 0)
		return false;

	return true;
}

static AVFrame *
demux_and_decode(struct video *s)
{
	AVFrame *frame;
	bool ret;

	frame = av_frame_alloc();
	if (!frame)
		return NULL;

	for (;;) {
		int r;

		r = av_read_frame(s->fmt_ctx, s->pkt);
		if (r < 0)
			break;

		ret = decode(s, frame);

		av_packet_unref(s->pkt);

		if (ret)
			break;
	}

	if (!ret)
		av_frame_free(&frame);

	return frame;
}

static struct buffer *
video_next_buffer(struct video *s)
{
	int i;

	for (i = 0; i < NUM_BUFFERS; i++)
		if (!s->buffers[i].busy)
			return &s->buffers[i];

	return NULL;
}

static void
copy_420p10_to_p010(struct buffer *buffer, AVFrame *frame)
{
	int height, linesize;
	uint16_t *yplane, *dsty, *uplane, *vplane, *dstuv;
	int size;

	// copy plane 0
	height = buffer->height;
	yplane = (uint16_t *) frame->data[0];
	dsty = (uint16_t *) buffer->mmap;
	linesize = frame->linesize[0];

	size = (linesize * height) / 2;
	// bitshift and copy y plane
	for (int i = 0; i < size; i++) {
		dsty[i] = yplane[i] << 6;
	}

	// copy plane 1 and 2 alternatingly from source
	height = buffer->height / 2;
	uplane = (uint16_t *) frame->data[1];
	vplane = (uint16_t *) frame->data[2];
	dstuv = (uint16_t *) (buffer->mmap + buffer->stride * buffer->height);

	// both uplane and vplane would have same linesize
	linesize = frame->linesize[1];

	size = (linesize * height) / 2;

	// bit shift and copy u, v alternatingly
	for (int i = 0; i < size; i++) {
		dstuv[2 * i] = uplane[i] << 6;
		dstuv[(2 * i) + 1] = vplane[i] << 6;
	}
}

static int
fill_buffer(struct buffer *buffer, AVFrame *frame) {
	int linesize;
	int height;
	int plane_heights[4] = {0};
	int offsets[4] = {0};
	int n_planes = 0;
	uint8_t *src, *dst;
	int i;
	void *map_data = NULL;
	uint32_t dst_stride;

	buffer->mmap = gbm_bo_map(buffer->bo, 0, 0, buffer->width, buffer->height,
				  GBM_BO_TRANSFER_WRITE, &dst_stride, &map_data);
	if (!buffer->mmap) {
		fprintf(stderr, "Unable to mmap buffer\n");
		return 0;
	}

	switch (buffer->format) {
	case DRM_FORMAT_YUV420:
		plane_heights[0] = buffer->height;
		plane_heights[1] = buffer->height / 2;
		plane_heights[2] = buffer->height / 2;
		offsets[0] = 0;
		offsets[1] = buffer->height;
		offsets[2] = buffer->height * 3 / 2;
		n_planes = 3;
		break;
	case DRM_FORMAT_P010:
		copy_420p10_to_p010(buffer, frame);
		gbm_bo_unmap(buffer->bo, map_data);
		return 1;
	}

	for (i = 0; i < n_planes; i++) {
		height = plane_heights[i];
		linesize = frame->linesize[i];

		src = frame->data[i];
		dst = buffer->mmap + frame->linesize[0] * offsets[i];

		for (;height > 0; height--) {
			memcpy(dst, src, linesize);
			dst += linesize;
			src += linesize;
		}
	}

	gbm_bo_unmap(buffer->bo, map_data);
	return 1;
}

static void
redraw_handler(struct widget *widget, void *data)
{
	struct app *app = data;
	struct video *video = &app->video;
	struct buffer *buffer;
	struct wl_buffer *wlbuffer;
	struct wl_surface *surface;
	AVFrame *frame;
	AVFrameSideData *sd;
	AVMasteringDisplayMetadata *hdr_metadata;
	AVContentLightMetadata *ll_metadata;
	int max_cll = -1, max_fall = -1;
	wl_fixed_t rx, ry, gx, gy, bx, by, wx, wy, max_luma, min_luma;

	frame = demux_and_decode(&app->video);
	if (!frame) {
		fprintf(stderr, "no more frames?\n");
		return;
	}

	sd = av_frame_get_side_data(frame, AV_FRAME_DATA_CONTENT_LIGHT_LEVEL);
	if (sd) {
		ll_metadata = (AVContentLightMetadata *)sd->data;
		max_cll = ll_metadata->MaxCLL;
		max_fall = ll_metadata->MaxFALL;
	}
	sd = av_frame_get_side_data(frame, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA);
	if (sd) {
		hdr_metadata = (AVMasteringDisplayMetadata *)sd->data;
		if (hdr_metadata->has_luminance && hdr_metadata->has_primaries) {
			ensure_hdr_surface(app);
			rx = wl_fixed_from_double(av_q2d(hdr_metadata->display_primaries[0][0]));
			ry = wl_fixed_from_double(av_q2d(hdr_metadata->display_primaries[0][1]));
			gx = wl_fixed_from_double(av_q2d(hdr_metadata->display_primaries[1][0]));
			gy = wl_fixed_from_double(av_q2d(hdr_metadata->display_primaries[1][1]));
			bx = wl_fixed_from_double(av_q2d(hdr_metadata->display_primaries[2][0]));
			by = wl_fixed_from_double(av_q2d(hdr_metadata->display_primaries[2][1]));
			wx = wl_fixed_from_double(av_q2d(hdr_metadata->white_point[0]));
			wy = wl_fixed_from_double(av_q2d(hdr_metadata->white_point[1]));
			max_luma = wl_fixed_from_double(av_q2d(hdr_metadata->max_luminance));
			min_luma = wl_fixed_from_double(av_q2d(hdr_metadata->min_luminance));
			zwp_hdr_surface_v1_set(
				app->hdr_surface,
				rx,
				ry,
				gx,
				gy,
				bx,
				by,
				wx,
				wy,
				max_luma,
				min_luma,
				max_cll == -1 ? 0 : max_cll,
				max_fall == -1 ? 0 : max_fall);

			zwp_hdr_surface_v1_set_eotf(
				app->hdr_surface,
				ZWP_HDR_SURFACE_V1_EOTF_ST_2084_PQ);
		}
	} else {
		// No metadata for this frame. Destroy the created hdr surface.
		destroy_hdr_surface(app);
	}

	buffer = video_next_buffer(video);

	// If no free buffers available schedule redraw and return;
	if(!buffer) {
		widget_schedule_redraw(widget);
		return;
	}

	if (!fill_buffer(buffer, frame)) {
		fprintf(stderr, "failed to fill buffer\n");
		return;
	}

	wlbuffer = buffer->buffer;

	surface = widget_get_wl_surface(widget);
	wl_surface_attach(surface, wlbuffer, 0, 0);
	wl_surface_damage(surface, 0, 0, frame->width, frame->height);
	wl_surface_commit(surface);
	widget_schedule_redraw(widget);
	buffer->busy = 1;
	buffer->prev_frame = frame;
}

/*
 * +---------------------------+
 * |   |                       |
 * |   |                       |
 * |   |vm   Video             |
 * |   |                       |
 * |   |                       |
 * |___+-------------------+   |
 * | hm| Subtitle          |   |
 * |   +-------------------+   |
 * |                           |
 * +---------------------------+
 *
 * hm : horizontal margin
 * vm : vertical margin
 */

static void
resize_handler(struct widget *widget,
	       int32_t width, int32_t height, void *data)
{
	struct app *app = data;
	struct rectangle area;

	// margin is in percentage
	int vm = 85, hm = 40;
	int x, y, w, h;
	int mhorizontal, mvertical;

	if (app->subtitle) {
		widget_get_allocation(widget, &area);

		mhorizontal = area.width * hm / 100;
		mvertical = area.height * vm / 100;

		x = area.x + mhorizontal;
		y = area.y + mvertical;
		w = area.width * 2 / 10; // 20% of total width
		h = area.height / 20; // 5% of total height

		widget_set_allocation(app->subtitle->widget,
				      x, y, w, h);
	}

}

static void
keyboard_focus_handler(struct window *window,
		       struct input *device, void *data)
{
	struct app *app = data;

	window_schedule_redraw(app->window);
}

static void
key_handler(struct window *window, struct input *input, uint32_t time,
	    uint32_t key, uint32_t sym,
	    enum wl_keyboard_key_state state, void *data)
{
	struct app *app = data;
	struct rectangle winrect;

	if (state == WL_KEYBOARD_KEY_STATE_RELEASED)
		return;

	switch (sym) {
	case XKB_KEY_Up:
		window_get_allocation(window, &winrect);
		winrect.height -= 100;
		if (winrect.height < 150)
			winrect.height = 150;
		window_schedule_resize(window, winrect.width, winrect.height);
		break;
	case XKB_KEY_Down:
		window_get_allocation(window, &winrect);
		winrect.height += 100;
		if (winrect.height > 600)
			winrect.height = 600;
		window_schedule_resize(window, winrect.width, winrect.height);
		break;
	case XKB_KEY_Escape:
		display_exit(app->display);
		break;
	}
}

static void video_close(struct video *s)
{
	av_parser_close(s->parser);
	avcodec_free_context(&s->codec);
	av_packet_free(&s->pkt);
}

static const enum zwp_colorspace_v1_chromacities chromacities[] = {
	[AVCOL_PRI_BT709] = ZWP_COLORSPACE_V1_CHROMACITIES_BT709,
	[AVCOL_PRI_BT470M] = ZWP_COLORSPACE_V1_CHROMACITIES_BT470M,
	[AVCOL_PRI_BT470BG] = ZWP_COLORSPACE_V1_CHROMACITIES_BT470BG,
	[AVCOL_PRI_SMPTE170M] = ZWP_COLORSPACE_V1_CHROMACITIES_SMPTE170M,
	[AVCOL_PRI_SMPTE240M] = ZWP_COLORSPACE_V1_CHROMACITIES_SMPTE170M,
	[AVCOL_PRI_SMPTE431] = ZWP_COLORSPACE_V1_CHROMACITIES_DCI_P3,
	[AVCOL_PRI_SMPTE432] = ZWP_COLORSPACE_V1_CHROMACITIES_DCI_P3,
	[AVCOL_PRI_SMPTE428] = ZWP_COLORSPACE_V1_CHROMACITIES_CIEXYZ,
	[AVCOL_PRI_BT2020] = ZWP_COLORSPACE_V1_CHROMACITIES_BT2020,
};

static enum zwp_colorspace_v1_chromacities
video_chromacities(struct video *s)
{
	if (s->codec->color_primaries >= ARRAY_LENGTH(chromacities))
		return ZWP_COLORSPACE_V1_CHROMACITIES_UNDEFINED;

	return chromacities[s->codec->color_primaries];
}

static bool video_open(struct app *app,
		       struct video *s,
		       const char *filename)
{
	AVCodec *codec = NULL;
	AVStream *stream;
	int r;
	char buf[4096] = {};
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(58, 9, 100)
	av_register_all();
#endif

	r = avformat_open_input(&s->fmt_ctx, filename, NULL, NULL);
	if (r < 0)
		return false;

	r = avformat_find_stream_info(s->fmt_ctx, NULL);
	if (r < 0)
		return false;

	r = av_find_best_stream(s->fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
	if (r < 0)
		return false;

	stream = s->fmt_ctx->streams[r];
	s->stream_index = r;

	s->codec = avcodec_alloc_context3(codec);
	if (!s->codec)
		return false;

	s->codec->opaque = s;

	r = avcodec_parameters_to_context(s->codec, stream->codecpar);
	if (r < 0)
		return false;

	r = avcodec_open2(s->codec, codec, NULL);
	if (r < 0)
		return false;

	s->parser = av_parser_init(codec->id);
	if (!s->parser)
		return false;

	avcodec_string(buf, sizeof(buf), s->codec, false);
	buf[sizeof(buf)-1] = '\0';
	puts(buf);

	s->pkt = av_packet_alloc();

	return true;
}

static void
dmabuf_modifiers(void *data, struct zwp_linux_dmabuf_v1 *zwp_linux_dmabuf,
		 uint32_t format, uint32_t modifier_hi, uint32_t modifier_lo)
{
}

static void
dmabuf_format(void *data, struct zwp_linux_dmabuf_v1 *zwp_linux_dmabuf, uint32_t format)
{
	/* XXX: deprecated */
}


static const struct zwp_linux_dmabuf_v1_listener dmabuf_listener = {
	dmabuf_format,
	dmabuf_modifiers
};

static void
global_handler(struct display *display, uint32_t id,
	       const char *interface, uint32_t version, void *data)
{

	struct app *app = data;

	if (strcmp(interface, "zwp_colorspace_v1") == 0) {
		app->colorspace =
			display_bind(display, id,
				     &zwp_colorspace_v1_interface, 1);
	} else if (strcmp(interface, "zwp_hdr_metadata_v1") == 0) {
		app->hdr_metadata =
			display_bind(display, id,
				     &zwp_hdr_metadata_v1_interface, 1);
	} else if (strcmp(interface, "zwp_linux_dmabuf_v1") == 0) {
		if (version < 3)
			return;
		app->dmabuf =
			display_bind(display, id,
				     &zwp_linux_dmabuf_v1_interface, 3);
		zwp_linux_dmabuf_v1_add_listener(app->dmabuf,
						 &dmabuf_listener,
						 app);
	}

}

static void
global_handler_remove(struct display *display, uint32_t id,
		      const char *interface, uint32_t version, void *data)
{
}

static int
gbm_init(struct buffer *my_buf)
{
	my_buf->drm_fd = open("/dev/dri/renderD128", O_RDWR);
	if (my_buf->drm_fd < 0)
		return 0;

	if (!my_buf->gbm)
		my_buf->gbm = gbm_create_device(my_buf->drm_fd);

	if (!my_buf->gbm)
		return 0;

	return 1;
}

static void
gbm_fini(struct buffer *my_buf)
{
	gbm_device_destroy(my_buf->gbm);
	close(my_buf->drm_fd);
}

static int
create_dmabuf_buffer(struct app *app, struct buffer *buffer,
		     int width, int height, int format)
{
	struct zwp_linux_buffer_params_v1 *params;
	uint64_t modifier = DRM_FORMAT_MOD_LINEAR;
	uint32_t flags = 0;
	unsigned buf_w, buf_h;
	int pixel_format;

	memset(buffer, 0, sizeof(*buffer));
	if (!gbm_init(buffer)) {
		fprintf(stderr, "drm_connect failed\n");
		goto error;
	}

	buffer->width = width;
	buffer->height = height;
	buffer->format = format;

	switch (format) {
	case DRM_FORMAT_NV12:
		pixel_format = GBM_FORMAT_GR88;
		buf_w = width / 2;
		buf_h = height * 3 / 2;
		buffer->cpp = 1;
		break;
	case DRM_FORMAT_YUV420:
		pixel_format = GBM_FORMAT_GR88;
		buf_w = width / 2;
		buf_h = height * 2;    /* U/V not interleaved */
		buffer->cpp = 1;
		break;
	case DRM_FORMAT_P010:
		pixel_format = GBM_FORMAT_GR88;
		buf_w = width;
		buf_h = height * 3 / 2;
		buffer->cpp = 2;
		break;
	case DRM_FORMAT_ARGB8888:
		pixel_format = GBM_FORMAT_ARGB8888;
		buf_w = width;
		buf_h = height;
		buffer->cpp = 1;
		break;
	default:
		buffer->height = height;
		buffer->cpp = 1;
	}

	buffer->bo = gbm_bo_create_with_modifiers(buffer->gbm, buf_w, buf_h, pixel_format, &modifier, 1);
	if (!buffer->bo) {
		fprintf(stderr, "error: unable to allocate bo\n");
		goto error1;
	}

	buffer->dmabuf_fd = gbm_bo_get_fd(buffer->bo);
	buffer->stride = gbm_bo_get_stride(buffer->bo);

	if (buffer->dmabuf_fd < 0) {
		fprintf(stderr, "error: dmabuf_fd < 0\n");
		goto error2;
	}

	params = zwp_linux_dmabuf_v1_create_params(app->dmabuf);
	zwp_linux_buffer_params_v1_add(params,
				       buffer->dmabuf_fd,
				       0, /* plane_idx */
				       0, /* offset */
				       buffer->stride,
				       modifier >> 32,
				       modifier & 0xffffffff);

	switch (format) {
	case DRM_FORMAT_NV12:
		/* add the second plane params */
		zwp_linux_buffer_params_v1_add(params,
					       buffer->dmabuf_fd,
					       1,
					       buffer->stride * buffer->height,
					       buffer->stride,
					       modifier >> 32,
					       modifier & 0xffffffff);
		break;
	case DRM_FORMAT_YUV420:
		/* add the second plane params */
		zwp_linux_buffer_params_v1_add(params,
					       buffer->dmabuf_fd,
					       1,
					       buffer->stride * buffer->height,
					       buffer->stride / 2,
					       modifier >> 32,
					       modifier & 0xffffffff);

		/* add the third plane params */
		zwp_linux_buffer_params_v1_add(params,
					       buffer->dmabuf_fd,
					       2,
					       buffer->stride * buffer->height * 3 / 2,
					       buffer->stride / 2,
					       modifier >> 32,
					       modifier & 0xffffffff);
		break;
	case DRM_FORMAT_P010:
		/* add the second plane params */
		zwp_linux_buffer_params_v1_add(params,
					       buffer->dmabuf_fd,
					       1,
					       buffer->stride * buffer->height,
					       buffer->stride,
					       modifier >> 32,
					       modifier & 0xffffffff);
		break;
	default:
		break;
	}

	buffer->buffer = zwp_linux_buffer_params_v1_create_immed(params,
								 buffer->width,
								 buffer->height,
								 format,
								 flags);
	wl_buffer_add_listener(buffer->buffer, &buffer_listener, buffer);

	return 0;

error2:
	gbm_bo_destroy(buffer->bo);
error1:
	gbm_fini(buffer);
error:
	return -1;
}

static struct app *
video_create(struct display *display, const char *filename)
{
	struct app *app;
	struct wl_surface *surface;
	struct wl_display *wldisplay;
	uint32_t i, width, height, format;
	int ret;
	struct buffer *buffer;

	app = xzalloc(sizeof *app);

	app->display = display;
	display_set_user_data(app->display, app);
	display_set_global_handler(display, global_handler);
	display_set_global_handler_remove(display, global_handler_remove);

	// Ensure that we have received the DMABUF format and modifier support
	wldisplay = display_get_display(display);
	wl_display_roundtrip(wldisplay);

	app->window = window_create(app->display);
	app->widget = window_add_widget(app->window, app);
	window_set_title(app->window, "Wayland Simple HDR video");

	window_set_key_handler(app->window, key_handler);
	window_set_user_data(app->window, app);
	window_set_keyboard_focus_handler(app->window, keyboard_focus_handler);

	widget_set_redraw_handler(app->widget, redraw_handler);
	widget_set_resize_handler(app->widget, resize_handler);

	widget_set_use_cairo(app->widget, 0);

	if (!video_open(app, &app->video, filename))
		goto err;

	surface = window_get_wl_surface(app->window);
	zwp_colorspace_v1_set(app->colorspace,
			      surface,
			      video_chromacities(&app->video));

	if (option_subtitle)
		app->subtitle = subtitle_create(app);

	width = app->video.codec->width;
	height = app->video.codec->height;
	format = av_format_to_drm_format(app->video.codec->pix_fmt);

	if (option_fullscreen) {
		window_set_fullscreen(app->window, 1);
	} else {
		/* if not fullscreen, resize as per the video size */
		widget_schedule_resize(app->widget, width, height);
	}

	for (i = 0; i < NUM_BUFFERS; i++) {
		buffer = &app->video.buffers[i];
		ret = create_dmabuf_buffer(app, buffer, width, height, format);

		if (ret < 0)
			goto err;
	}

	return app;

err:
	free(app);
	return NULL;
}

static void
video_destroy(struct app *app)
{
	if (app->subtitle)
		subtitle_destroy(app->subtitle);

	video_close(&app->video);

	widget_destroy(app->widget);
	window_destroy(app->window);
	free(app);
}

int
main(int argc, char *argv[])
{
	struct display *display;
	struct app *app;

	parse_options(options, ARRAY_LENGTH(options), &argc, argv);
	if (option_help) {
		printf(help_text, argv[0]);
		return 0;
	}

	display = display_create(&argc, argv);
	if (display == NULL) {
		fprintf(stderr, "failed to create display: %m\n");
		return -1;
	}

	if (!display_has_subcompositor(display)) {
		fprintf(stderr, "compositor does not support "
			"the subcompositor extension\n");
		return -1;
	}

	app = video_create(display, argv[argc - 1]);
	if (!app) {
		fprintf(stderr, "Failed to initialize!");
		exit(EXIT_FAILURE);
	}

	display_run(display);

	video_destroy(app);
	display_destroy(display);

	return 0;
}
