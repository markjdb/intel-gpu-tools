/*
 * Copyright © 2016 Intel Corporation
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
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 */

#include "igt.h"
#include "drmtest.h"
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

IGT_TEST_DESCRIPTION("Test atomic mode setting with multiple planes ");

#define MAX_CRCS          1
#define SIZE_PLANE      256
#define SIZE_CURSOR     128
#define LOOP_FOREVER     -1

typedef struct {
	float red;
	float green;
	float blue;
} color_t;

typedef struct {
	int drm_fd;
	igt_display_t display;
	igt_pipe_crc_t *pipe_crc;
	igt_plane_t *plane[IGT_MAX_PLANES];
	struct igt_fb fb[IGT_MAX_PLANES];
} data_t;

typedef struct {
	data_t *data;
	igt_crc_t reference_crc;
} test_position_t;

/* Command line parameters. */
struct {
	int iterations;
	bool user_seed;
	int seed;
} opt = {
	.iterations = 1,
	.user_seed = false,
	.seed = 1,
};

static inline uint32_t pipe_select(int pipe)
{
	if (pipe > 1)
		return pipe << DRM_VBLANK_HIGH_CRTC_SHIFT;
	else if (pipe > 0)
		return DRM_VBLANK_SECONDARY;
	else
		return 0;
}

static unsigned int get_vblank(int fd, int pipe, unsigned int flags)
{
	union drm_wait_vblank vbl;

	memset(&vbl, 0, sizeof(vbl));
	vbl.request.type = DRM_VBLANK_RELATIVE | pipe_select(pipe) | flags;
	if (drmIoctl(fd, DRM_IOCTL_WAIT_VBLANK, &vbl))
		return 0;

	return vbl.reply.sequence;
}

/*
 * Common code across all tests, acting on data_t
 */
static void test_init(data_t *data, enum pipe pipe)
{
	data->pipe_crc = igt_pipe_crc_new(pipe, INTEL_PIPE_CRC_SOURCE_AUTO);
}

static void test_fini(data_t *data, igt_output_t *output, int max_planes)
{
	for (int i = IGT_PLANE_PRIMARY; i <= max_planes; i++)
		igt_plane_set_fb(data->plane[i], NULL);

	/* reset the constraint on the pipe */
	igt_output_set_pipe(output, PIPE_ANY);

	igt_pipe_crc_free(data->pipe_crc);
}

static void
test_grab_crc(data_t *data, igt_output_t *output, enum pipe pipe, bool atomic,
	      color_t *color, uint64_t tiling, igt_crc_t *crc /* out */)
{
	drmModeModeInfo *mode;
	int ret, n;

	igt_output_set_pipe(output, pipe);

	data->plane[IGT_PLANE_PRIMARY] = igt_output_get_plane(output, IGT_PLANE_PRIMARY);

	mode = igt_output_get_mode(output);

	igt_create_color_fb(data->drm_fd, mode->hdisplay, mode->vdisplay,
			    DRM_FORMAT_XRGB8888,
			    LOCAL_DRM_FORMAT_MOD_NONE,
			    color->red, color->green, color->blue,
			    &data->fb[IGT_PLANE_PRIMARY]);

	igt_plane_set_fb(data->plane[IGT_PLANE_PRIMARY], &data->fb[IGT_PLANE_PRIMARY]);

	ret = igt_display_try_commit2(&data->display,
				      atomic ? COMMIT_ATOMIC : COMMIT_LEGACY);
	igt_skip_on(ret != 0);

	igt_pipe_crc_start(data->pipe_crc);
	n = igt_pipe_crc_get_crcs(data->pipe_crc, 1, &crc);
	igt_assert_eq(n, 1);
}

/*
 * Multiple plane position test.
 *   - We start by grabbing a reference CRC of a full blue fb being scanned
 *     out on the primary plane
 *   - Then we scannout number of planes:
 *      * the primary plane uses a blue fb with a black rectangle hole
 *      * planes, on top of the primary plane, with a blue fb that is set-up
 *        to cover the black rectangles of the primary plane fb
 *     The resulting CRC should be identical to the reference CRC
 */

static void
create_fb_for_mode_position(data_t *data, drmModeModeInfo *mode,
			    color_t *color, int *rect_x, int *rect_y,
			    int *rect_w, int *rect_h, uint64_t tiling,
			    int max_planes)
{
	unsigned int fb_id;
	cairo_t *cr;

	fb_id = igt_create_fb(data->drm_fd,
			      mode->hdisplay, mode->vdisplay,
			      DRM_FORMAT_XRGB8888,
			      tiling,
			      &data->fb[IGT_PLANE_PRIMARY]);
	igt_assert(fb_id);

	cr = igt_get_cairo_ctx(data->drm_fd, &data->fb[IGT_PLANE_PRIMARY]);
	igt_paint_color(cr, rect_x[0], rect_y[0],
			mode->hdisplay, mode->vdisplay,
			color->red, color->green, color->blue);

	for (int i = IGT_PLANE_2; i <= max_planes; i++)
		igt_paint_color(cr, rect_x[i], rect_y[i],
				rect_w[i], rect_h[i], 0.0, 0.0, 0.0);

	igt_assert(cairo_status(cr) == 0);
	cairo_destroy(cr);
}


static void
prepare_planes(data_t *data, enum pipe pipe, color_t *color,
	       uint64_t tiling, int max_planes, igt_output_t *output)
{
	drmModeModeInfo *mode;
	int x[IGT_MAX_PLANES];
	int y[IGT_MAX_PLANES];
	int size[IGT_MAX_PLANES];
	int i;

	igt_output_set_pipe(output, pipe);

	mode = igt_output_get_mode(output);

	/* planes with random positions */
	x[IGT_PLANE_PRIMARY] = 0;
	y[IGT_PLANE_PRIMARY] = 0;
	for (i = IGT_PLANE_2; i <= max_planes; i++) {
		if (i == IGT_PLANE_CURSOR)
			size[i] = SIZE_CURSOR;
		else
			size[i] = SIZE_PLANE;

		x[i] = rand() % (mode->hdisplay - size[i]);
		y[i] = rand() % (mode->vdisplay - size[i]);

		data->plane[i] = igt_output_get_plane(output, i);

		igt_create_color_fb(data->drm_fd,
				    size[i], size[i],
				    data->plane[i]->is_cursor ? DRM_FORMAT_ARGB8888 : DRM_FORMAT_XRGB8888,
				    data->plane[i]->is_cursor ? LOCAL_DRM_FORMAT_MOD_NONE : tiling,
				    color->red, color->green, color->blue,
				    &data->fb[i]);

		igt_plane_set_position(data->plane[i], x[i], y[i]);
		igt_plane_set_fb(data->plane[i], &data->fb[i]);
	}

	/* primary plane */
	data->plane[IGT_PLANE_PRIMARY] = igt_output_get_plane(output, IGT_PLANE_PRIMARY);
	create_fb_for_mode_position(data, mode, color, x, y,
				    size, size, tiling, max_planes);
	igt_plane_set_fb(data->plane[IGT_PLANE_PRIMARY], &data->fb[IGT_PLANE_PRIMARY]);
}

static void
test_atomic_plane_position_with_output(data_t *data, enum pipe pipe,
				       igt_output_t *output, int max_planes,
				       uint64_t tiling)
{
	char buf[256];
	struct drm_event *e = (void *)buf;
	test_position_t test = { .data = data };
	color_t blue  = { 0.0f, 0.0f, 1.0f };
	igt_crc_t *crc = NULL;
	unsigned int vblank_start;
	int i, n, ret;
	int iterations = opt.iterations < 1 ? 1 : opt.iterations;
	bool loop_forever;
	char info[256];

	if (opt.iterations == LOOP_FOREVER) {
		loop_forever = true;
		sprintf(info, "forever");
	} else {
		loop_forever = false;
		sprintf(info, "for %d %s",
			iterations, iterations > 1 ? "iterations" : "iteration");
	}

	igt_info("Testing connector %s using pipe %s with %d planes %s with seed %d\n",
		 igt_output_name(output), kmstest_pipe_name(pipe), max_planes,
		 info, opt.seed);

	test_init(data, pipe);

	test_grab_crc(data, output, pipe, true, &blue, tiling,
		      &test.reference_crc);

	i = 0;
	while (i < iterations || loop_forever) {
		prepare_planes(data, pipe, &blue, tiling, max_planes, output);

		vblank_start = get_vblank(data->display.drm_fd, pipe, DRM_VBLANK_NEXTONMISS);

		igt_display_commit_atomic(&data->display,
					  DRM_MODE_PAGE_FLIP_EVENT,
					  &data->display);

		igt_set_timeout(1, "Stuck on page flip");

		ret = read(data->display.drm_fd, buf, sizeof(buf));
		igt_assert(ret >= 0);

		igt_assert_eq(get_vblank(data->display.drm_fd, pipe, 0), vblank_start + 1);
		igt_assert_eq(e->type, DRM_EVENT_FLIP_COMPLETE);
		igt_reset_timeout();

		n = igt_pipe_crc_get_crcs(data->pipe_crc, MAX_CRCS, &crc);

		igt_assert_eq(n, MAX_CRCS);

		igt_assert_crc_equal(&test.reference_crc, crc);

		i++;
	}

	igt_pipe_crc_stop(data->pipe_crc);

	test_fini(data, output, max_planes);
}

static void
test_legacy_plane_position_with_output(data_t *data, enum pipe pipe,
				       igt_output_t *output, int max_planes,
				       uint64_t tiling)
{
	test_position_t test = { .data = data };
	color_t blue  = { 0.0f, 0.0f, 1.0f };
	igt_crc_t *crc;
	int i, n;
	int iterations = opt.iterations < 1 ? 1 : opt.iterations;
	bool loop_forever;
	char info[256];

	if (opt.iterations == LOOP_FOREVER) {
		loop_forever = true;
		sprintf(info, "forever");
	} else {
		loop_forever = false;
		sprintf(info, "for %d %s",
			iterations, iterations > 1 ? "iterations" : "iteration");
	}

	igt_info("Testing connector %s using pipe %s with %d planes %s with seed %d\n",
		 igt_output_name(output), kmstest_pipe_name(pipe), max_planes,
		 info, opt.seed);

	test_init(data, pipe);

	test_grab_crc(data, output, pipe, false, &blue, tiling,
		      &test.reference_crc);

	i = 0;
	while (i < iterations || loop_forever) {
		prepare_planes(data, pipe, &blue, tiling, max_planes, output);

		igt_display_commit2(&data->display, COMMIT_LEGACY);

		n = igt_pipe_crc_get_crcs(data->pipe_crc, MAX_CRCS, &crc);

		igt_assert_eq(n, MAX_CRCS);

		igt_assert_crc_equal(&test.reference_crc, crc);

		i++;
	}

	igt_pipe_crc_stop(data->pipe_crc);

	test_fini(data, output, max_planes);
}

static void
test_plane_position(data_t *data, enum pipe pipe, bool atomic, int max_planes,
		    uint64_t tiling)
{
	igt_output_t *output;
	int connected_outs;
	int devid = intel_get_drm_devid(data->drm_fd);

	if (atomic)
		igt_require(data->display.is_atomic);

	igt_skip_on(pipe >= data->display.n_pipes);
	igt_skip_on(max_planes >= data->display.pipes[pipe].n_planes);

	if ((tiling == LOCAL_I915_FORMAT_MOD_Y_TILED ||
	     tiling == LOCAL_I915_FORMAT_MOD_Yf_TILED))
		igt_require(AT_LEAST_GEN(devid, 9));

	if (!opt.user_seed)
		opt.seed = time(NULL);

	srand(opt.seed);

	connected_outs = 0;
	for_each_valid_output_on_pipe(&data->display, pipe, output) {
		if (atomic)
			test_atomic_plane_position_with_output(data, pipe,
							       output,
							       max_planes,
							       tiling);
		else
			test_legacy_plane_position_with_output(data, pipe,
							       output,
							       max_planes,
							       tiling);

		connected_outs++;
	}

	igt_skip_on(connected_outs == 0);

}

static void
run_tests_for_pipe_plane(data_t *data, enum pipe pipe, int max_planes)
{
	igt_subtest_f("legacy-pipe-%s-tiling-none-planes-%d",
		      kmstest_pipe_name(pipe), max_planes)
		test_plane_position(data, pipe, false, max_planes,
				    LOCAL_DRM_FORMAT_MOD_NONE);

	igt_subtest_f("atomic-pipe-%s-tiling-none-planes-%d",
		      kmstest_pipe_name(pipe), max_planes)
		test_plane_position(data, pipe, true, max_planes,
				    LOCAL_I915_FORMAT_MOD_X_TILED);

	igt_subtest_f("legacy-pipe-%s-tiling-x-planes-%d",
		      kmstest_pipe_name(pipe), max_planes)
		test_plane_position(data, pipe, false, max_planes,
				    LOCAL_I915_FORMAT_MOD_X_TILED);

	igt_subtest_f("atomic-pipe-%s-tiling-x-planes-%d",
		      kmstest_pipe_name(pipe), max_planes)
		test_plane_position(data, pipe, true, max_planes,
				    LOCAL_I915_FORMAT_MOD_X_TILED);

	igt_subtest_f("legacy-pipe-%s-tiling-y-planes-%d",
		      kmstest_pipe_name(pipe), max_planes)
		test_plane_position(data, pipe, false, max_planes,
				    LOCAL_I915_FORMAT_MOD_Y_TILED);

	igt_subtest_f("atomic-pipe-%s-tiling-y-planes-%d",
		      kmstest_pipe_name(pipe), max_planes)
		test_plane_position(data, pipe, true, max_planes,
				    LOCAL_I915_FORMAT_MOD_Y_TILED);

	igt_subtest_f("legacy-pipe-%s-tiling-yf-planes-%d",
		      kmstest_pipe_name(pipe), max_planes)
		test_plane_position(data, pipe, false, max_planes,
				    LOCAL_I915_FORMAT_MOD_Yf_TILED);

	igt_subtest_f("atomic-pipe-%s-tiling-yf-planes-%d",
		      kmstest_pipe_name(pipe), max_planes)
		test_plane_position(data, pipe, true, max_planes,
				    LOCAL_I915_FORMAT_MOD_Yf_TILED);
}

static void
run_tests_for_pipe(data_t *data, enum pipe pipe)
{
	for (int planes = IGT_PLANE_PRIMARY; planes < IGT_MAX_PLANES; planes++)
		run_tests_for_pipe_plane(data, pipe, planes);
}

static data_t data;

static int opt_handler(int option, int option_index, void *input)
{
	switch (option) {
	case 'i':
		opt.iterations = strtol(optarg, NULL, 0);

		if (opt.iterations < LOOP_FOREVER || opt.iterations == 0) {
			igt_info("incorrect number of iterations\n");
			igt_assert(false);
		}

		break;
	case 's':
		opt.user_seed = true;
		opt.seed = strtol(optarg, NULL, 0);
		break;
	default:
		igt_assert(false);
	}

	return 0;
}

const char *help_str =
	"  --iterations Number of iterations for test coverage. -1 loop forever, default 64 iterations\n"
	"  --seed       Seed for random number generator\n";

int main(int argc, char *argv[])
{
	struct option long_options[] = {
		{ "iterations", required_argument, NULL, 'i'},
		{ "seed",    required_argument, NULL, 's'},
		{ 0, 0, 0, 0 }
	};

	igt_subtest_init_parse_opts(&argc, argv, "", long_options, help_str,
				    opt_handler, NULL);

	igt_skip_on_simulation();

	igt_fixture {
		data.drm_fd = drm_open_driver_master(DRIVER_INTEL);

		kmstest_set_vt_graphics_mode();

		igt_require_pipe_crc();
		igt_display_init(&data.display, data.drm_fd);
	}

	for (int pipe = 0; pipe < I915_MAX_PIPES; pipe++)
		run_tests_for_pipe(&data, pipe);

	igt_fixture {
		igt_display_fini(&data.display);
	}

	igt_exit();
}
