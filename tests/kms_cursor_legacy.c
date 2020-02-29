/*
 * Copyright © 2013 Intel Corporation
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

#define _GNU_SOURCE
#include <sched.h>
#include <sys/poll.h>
#include <sys/param.h>
#include <sys/sysctl.h>
#include <sys/user.h>
#include <sys/cpuset.h>
#define cpu_set_t cpuset_t

#include "igt.h"
#include "igt_rand.h"
#include "igt_stats.h"

#if defined(__x86_64__) || defined(__i386__)
#define cpu_relax()	__builtin_ia32_pause()
#else
#define cpu_relax()	asm volatile("": : :"memory")
#endif

#ifndef DRM_CAP_CURSOR_WIDTH
#define DRM_CAP_CURSOR_WIDTH 0x8
#endif

#ifndef DRM_CAP_CURSOR_HEIGHT
#define DRM_CAP_CURSOR_HEIGHT 0x9
#endif

IGT_TEST_DESCRIPTION("Stress legacy cursor ioctl");

static void stress(igt_display_t *display,
		   int pipe, int num_children, unsigned mode,
		   int timeout)
{
	struct drm_mode_cursor arg;
	uint64_t *results;
	bool torture;
	int n;
	unsigned crtc_id[I915_MAX_PIPES], num_crtcs;

	torture = false;
	if (num_children < 0) {
		torture = true;
		num_children = -num_children;
	}

	results = mmap(NULL, 4096, PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
	igt_assert(results != MAP_FAILED);

	memset(&arg, 0, sizeof(arg));
	arg.flags = DRM_MODE_CURSOR_BO;
	arg.crtc_id = 0;
	arg.width = 64;
	arg.height = 64;
	arg.handle = kmstest_dumb_create(display->drm_fd, 64, 64, 32, NULL, NULL);

	if (pipe < 0) {
		num_crtcs = display->n_pipes;
		for_each_pipe(display, n) {
			arg.crtc_id = crtc_id[n] = display->pipes[n].crtc_id;
			do_ioctl(display->drm_fd, DRM_IOCTL_MODE_CURSOR, &arg);
		}
	} else {
		num_crtcs = 1;
		arg.crtc_id = crtc_id[0] = display->pipes[pipe].crtc_id;
		do_ioctl(display->drm_fd, DRM_IOCTL_MODE_CURSOR, &arg);
	}

	arg.flags = mode;
	igt_fork(child, num_children) {
		struct sched_param rt = {.sched_priority = 99 };
		cpu_set_t allowed;
		unsigned long count = 0;

		sched_setscheduler(getpid(), SCHED_RR, &rt);

		CPU_ZERO(&allowed);
		CPU_SET(child, &allowed);
		cpuset_setaffinity(CPU_LEVEL_WHICH, CPU_WHICH_PID, -1, sizeof(cpuset_t), &allowed);

		hars_petruska_f54_1_random_perturb(child);
		igt_until_timeout(timeout) {
			arg.crtc_id = crtc_id[hars_petruska_f54_1_random_unsafe() % num_crtcs];
			do_ioctl(display->drm_fd, DRM_IOCTL_MODE_CURSOR, &arg);
			count++;
		}

		igt_debug("[%d] count=%lu\n", child, count);
		results[child] = count;
	}
	if (torture) {
		igt_fork(child, num_children) {
			struct sched_param rt = {.sched_priority = 1 };
			cpu_set_t allowed;
			unsigned long long count = 0;

			sched_setscheduler(getpid(), SCHED_RR, &rt);

			CPU_ZERO(&allowed);
			CPU_SET(child, &allowed);
			cpuset_setaffinity(CPU_LEVEL_WHICH, CPU_WHICH_PID, -1, sizeof(cpuset_t), &allowed);
			igt_until_timeout(timeout) {
				count++;
				cpu_relax();
			}
			igt_debug("[hog:%d] count=%llu\n", child, count);
		}
	}
	igt_waitchildren();

	if (num_children > 1) {
		igt_stats_t stats;

		igt_stats_init_with_size(&stats, num_children);
		results[num_children] = 0;
		for (int child = 0; child < num_children; child++) {
			igt_stats_push(&stats, results[child]);
			results[num_children] += results[child];
		}
		igt_info("Total updates %llu (median of %d processes is %.2f)\n",
			 (long long)results[num_children],
			 num_children,
			 igt_stats_get_median(&stats));
		igt_stats_fini(&stats);
	} else {
		igt_info("Total updates %llu\n", (long long)results[0]);
	}

	gem_close(display->drm_fd, arg.handle);
	munmap(results, 4096);
}

static igt_output_t *set_fb_on_crtc(igt_display_t *display, int pipe, struct igt_fb *fb_info)
{
	igt_output_t *output;

	for_each_valid_output_on_pipe(display, pipe, output) {
		drmModeModeInfoPtr mode;
		igt_plane_t *primary;

		if (output->pending_crtc_idx_mask)
			continue;

		igt_output_set_pipe(output, pipe);
		mode = igt_output_get_mode(output);

		igt_create_pattern_fb(display->drm_fd,
			      mode->hdisplay, mode->vdisplay,
			      DRM_FORMAT_XRGB8888, I915_TILING_NONE, fb_info);

		primary = igt_output_get_plane(output, IGT_PLANE_PRIMARY);
		igt_plane_set_fb(primary, fb_info);

		return output;
	}

	return NULL;
}

static void set_cursor_on_pipe(igt_display_t *display, enum pipe pipe, struct igt_fb *fb)
{
	igt_plane_t *plane, *cursor = NULL;

	for_each_plane_on_pipe(display, pipe, plane) {
		if (!plane->is_cursor)
			continue;

		cursor = plane;
		break;
	}

	igt_require(cursor);
	igt_plane_set_fb(cursor, fb);
}

static void populate_cursor_args(igt_display_t *display, enum pipe pipe,
				 struct drm_mode_cursor *arg, struct igt_fb *fb)
{
	arg->crtc_id = display->pipes[pipe].crtc_id;
	arg->flags = DRM_MODE_CURSOR_MOVE;
	arg->x = 128;
	arg->y = 128;
	arg->width = fb->width;
	arg->height = fb->height;
	arg->handle = fb->gem_handle;
	arg[1] = *arg;
}

static void do_cleanup_display(igt_display_t *display)
{
	enum pipe pipe;
	igt_output_t *output;
	igt_plane_t *plane;

	for_each_pipe(display, pipe)
		for_each_plane_on_pipe(display, pipe, plane)
			igt_plane_set_fb(plane, NULL);

	for_each_connected_output(display, output)
		igt_output_set_pipe(output, PIPE_NONE);

	igt_display_commit2(display, display->is_atomic ? COMMIT_ATOMIC : COMMIT_LEGACY);
}

static enum pipe find_connected_pipe(igt_display_t *display, bool second)
{
	enum pipe pipe, first = PIPE_NONE;
	igt_output_t *output;
	igt_output_t *first_output = NULL;
	bool found = false;

	for_each_pipe_with_valid_output(display, pipe, output) {
		if (first == pipe || output == first_output)
			continue;

		if (second) {
			first = pipe;
			first_output = output;
			second = false;
			continue;
		}

		found = true;
		break;
	}

	if (first_output)
		igt_require_f(found, "No second valid output found\n");
	else
		igt_require_f(found, "No valid outputs found\n");

	return pipe;
}

static void flip_nonblocking(igt_display_t *display, enum pipe pipe, bool atomic, struct igt_fb *fb)
{
	igt_plane_t *primary = &display->pipes[pipe].planes[IGT_PLANE_PRIMARY];

	if (!atomic) {
		/* Schedule a nonblocking flip for the next vblank */
		do_or_die(drmModePageFlip(display->drm_fd, display->pipes[pipe].crtc_id, fb->fb_id,
					DRM_MODE_PAGE_FLIP_EVENT, fb));
	} else {
		igt_plane_set_fb(primary, fb);
		igt_display_commit_atomic(display, DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT, fb);
	}
}

enum flip_test {
	flip_test_legacy = 0,
	flip_test_varying_size,
	flip_test_toggle_visibility,
	flip_test_atomic,
	flip_test_atomic_transitions,
	flip_test_atomic_transitions_varying_size,
	flip_test_last = flip_test_atomic_transitions_varying_size
};

static bool cursor_slowpath(enum flip_test mode)
{
	/* cursor moving doesn't take slowpath, everything else does. */
	if (mode == flip_test_legacy || mode == flip_test_atomic)
		return false;

	return true;
}

static void transition_nonblocking(igt_display_t *display, enum pipe pipe,
				   struct igt_fb *prim_fb, struct igt_fb *argb_fb,
				   bool hide_sprite)
{
	igt_plane_t *primary = &display->pipes[pipe].planes[IGT_PLANE_PRIMARY];
	igt_plane_t *sprite = &display->pipes[pipe].planes[IGT_PLANE_2];

	if (hide_sprite) {
		igt_plane_set_fb(primary, prim_fb);
		igt_plane_set_fb(sprite, NULL);
	} else {
		igt_plane_set_fb(primary, NULL);
		igt_plane_set_fb(sprite, argb_fb);
	}

	igt_display_commit_atomic(display, DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT, display);
}

static void prepare_flip_test(igt_display_t *display,
			      enum flip_test mode,
			      enum pipe flip_pipe,
			      enum pipe cursor_pipe,
			      struct drm_mode_cursor *arg,
			      const struct igt_fb *prim_fb,
			      struct igt_fb *argb_fb,
			      struct igt_fb *cursor_fb2)
{
	argb_fb->gem_handle = 0;
	cursor_fb2->gem_handle = 0;

	if (mode == flip_test_varying_size ||
	    mode == flip_test_atomic_transitions_varying_size) {
		uint64_t width, height;

		do_or_die(drmGetCap(display->drm_fd, DRM_CAP_CURSOR_WIDTH, &width));
		do_or_die(drmGetCap(display->drm_fd, DRM_CAP_CURSOR_HEIGHT, &height));

		igt_skip_on(width <= 64 && height <= 64);
		igt_create_color_fb(display->drm_fd, width, height,
				    DRM_FORMAT_ARGB8888, 0, 1., 0., .7, cursor_fb2);

		arg[0].flags = arg[1].flags = DRM_MODE_CURSOR_BO;
		arg[1].handle = cursor_fb2->gem_handle;
		arg[1].width = width;
		arg[1].height = height;
	}

	if (mode == flip_test_legacy ||
	    mode == flip_test_atomic) {
		arg[1].x = 192;
		arg[1].y = 192;
	}

	if (mode == flip_test_toggle_visibility) {
		arg[0].flags = arg[1].flags = DRM_MODE_CURSOR_BO;
		arg[1].handle = 0;
		arg[1].width = arg[1].height = 0;
	}

	if (mode == flip_test_atomic_transitions ||
	    mode == flip_test_atomic_transitions_varying_size) {
		igt_require(display->pipes[flip_pipe].n_planes > 1 &&
			    !display->pipes[flip_pipe].planes[IGT_PLANE_2].is_cursor);

		igt_create_color_pattern_fb(display->drm_fd, prim_fb->width, prim_fb->height,
					    DRM_FORMAT_ARGB8888, 0, .1, .1, .1, argb_fb);
	}
}

static void flip(igt_display_t *display,
		int cursor_pipe, int flip_pipe,
		int timeout, enum flip_test mode)
{
	struct drm_mode_cursor arg[2];
	uint64_t *results;
	struct igt_fb fb_info, fb_info2, argb_fb, cursor_fb, cursor_fb2;

	results = mmap(NULL, 4096, PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
	igt_assert(results != MAP_FAILED);

	flip_pipe = find_connected_pipe(display, !!flip_pipe);
	cursor_pipe = find_connected_pipe(display, !!cursor_pipe);

	igt_info("Using pipe %s for page flip, pipe %s for cursor\n",
		  kmstest_pipe_name(flip_pipe), kmstest_pipe_name(cursor_pipe));

	if (mode >= flip_test_atomic)
		igt_require(display->is_atomic);

	igt_require(set_fb_on_crtc(display, flip_pipe, &fb_info));
	if (flip_pipe != cursor_pipe)
		igt_require(set_fb_on_crtc(display, cursor_pipe, &fb_info2));

	igt_create_color_fb(display->drm_fd, fb_info.width, fb_info.height, DRM_FORMAT_ARGB8888, 0, .5, .5, .5, &cursor_fb);

	igt_create_color_fb(display->drm_fd, 64, 64, DRM_FORMAT_ARGB8888, 0, 1., 1., 1., &cursor_fb);
	set_cursor_on_pipe(display, cursor_pipe, &cursor_fb);
	populate_cursor_args(display, cursor_pipe, arg, &cursor_fb);

	prepare_flip_test(display, mode, flip_pipe, cursor_pipe, arg, &fb_info, &argb_fb, &cursor_fb2);

	igt_display_commit2(display, display->is_atomic ? COMMIT_ATOMIC : COMMIT_LEGACY);

	igt_fork(child, 1) {
		unsigned long count = 0;

		igt_until_timeout(timeout) {
			do_ioctl(display->drm_fd, DRM_IOCTL_MODE_CURSOR, &arg[(count & 64)/64]);
			count++;
		}

		igt_debug("cursor count=%lu\n", count);
		results[0] = count;
	}
	igt_fork(child, 1) {
		unsigned long count = 0;

		igt_until_timeout(timeout) {
			char buf[128];

			switch (mode) {
			default:
				flip_nonblocking(display, flip_pipe, mode >= flip_test_atomic, &fb_info);
				break;
			case flip_test_atomic_transitions:
			case flip_test_atomic_transitions_varying_size:
				transition_nonblocking(display, flip_pipe, &fb_info, &argb_fb, count & 1);
				break;
			}

			while (read(display->drm_fd, buf, sizeof(buf)) < 0 &&
			       (errno == EINTR || errno == EAGAIN))
				;
			count++;
		}

		igt_debug("flip count=%lu\n", count);
		results[1] = count;
	}
	igt_waitchildren();

	munmap(results, 4096);

	do_cleanup_display(display);

	igt_remove_fb(display->drm_fd, &fb_info);
	if (flip_pipe != cursor_pipe)
		igt_remove_fb(display->drm_fd, &fb_info2);
	igt_remove_fb(display->drm_fd, &cursor_fb);
	if (argb_fb.gem_handle)
		igt_remove_fb(display->drm_fd, &argb_fb);
	if (cursor_fb2.gem_handle)
		igt_remove_fb(display->drm_fd, &cursor_fb2);
}

static inline uint32_t pipe_select(int pipe)
{
	if (pipe > 1)
		return pipe << DRM_VBLANK_HIGH_CRTC_SHIFT;
	else if (pipe > 0)
		return DRM_VBLANK_SECONDARY;
	else
		return 0;
}

static unsigned get_vblank(int fd, int pipe, unsigned flags)
{
	union drm_wait_vblank vbl;

	memset(&vbl, 0, sizeof(vbl));
	vbl.request.type = DRM_VBLANK_RELATIVE | pipe_select(pipe) | flags;
	if (drmIoctl(fd, DRM_IOCTL_WAIT_VBLANK, &vbl))
		return 0;

	return vbl.reply.sequence;
}

enum basic_flip_cursor {
	FLIP_BEFORE_CURSOR,
	FLIP_AFTER_CURSOR
};

static uint32_t *make_busy(int fd, uint32_t target)
{
	const int gen = intel_gen(intel_get_drm_devid(fd));
	struct drm_i915_gem_exec_object2 obj[2];
	struct drm_i915_gem_relocation_entry reloc[2];
	struct drm_i915_gem_execbuffer2 execbuf;
	uint32_t *batch;
	int i;

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = (uintptr_t)obj;
	execbuf.buffer_count = 2;

	memset(obj, 0, sizeof(obj));
	obj[0].handle = target;
	obj[1].handle = gem_create(fd, 4096);
	batch = gem_mmap__wc(fd, obj[1].handle, 0, 4096, PROT_WRITE);
	gem_set_domain(fd, obj[1].handle,
			I915_GEM_DOMAIN_GTT, I915_GEM_DOMAIN_GTT);


	obj[1].relocs_ptr = (uintptr_t)reloc;
	obj[1].relocation_count = 2;
	memset(reloc, 0, sizeof(reloc));

	reloc[0].target_handle = obj[1].handle; /* recurse */
	reloc[0].presumed_offset = 0;
	reloc[0].offset = sizeof(uint32_t);
	reloc[0].delta = 0;
	reloc[0].read_domains = I915_GEM_DOMAIN_COMMAND;
	reloc[0].write_domain = 0;

	reloc[1].target_handle = target;
	reloc[1].presumed_offset = 0;
	reloc[1].offset = 1024;
	reloc[1].delta = 0;
	reloc[1].read_domains = I915_GEM_DOMAIN_COMMAND;
	reloc[1].write_domain = I915_GEM_DOMAIN_COMMAND;

	i = 0;
	batch[i] = MI_BATCH_BUFFER_START;
	if (gen >= 8) {
		batch[i] |= 1 << 8 | 1;
		batch[++i] = 0;
		batch[++i] = 0;
	} else if (gen >= 6) {
		batch[i] |= 1 << 8;
		batch[++i] = 0;
	} else {
		batch[i] |= 2 << 6;
		batch[++i] = 0;
		if (gen < 4) {
			batch[i] |= 1;
			reloc[0].delta = 1;
		}
	}
	i++;

	gem_execbuf(fd, &execbuf);
	gem_close(fd, obj[1].handle);

	return batch;
}

static void cancel_busy(uint32_t *busy)
{
	*busy = MI_BATCH_BUFFER_END;
	munmap(busy, 4096);
}

static uint32_t *
make_fb_busy(int fd, const struct igt_fb *fb)
{
	uint32_t *busy;

	busy = make_busy(fd, fb->gem_handle);
	igt_assert(gem_bo_busy(fd, fb->gem_handle));

	return busy;
}

static void finish_fb_busy(uint32_t *busy)
{
	cancel_busy(busy);
}

#define BASIC_BUSY 0x1

static void basic_flip_cursor(igt_display_t *display,
			      enum flip_test mode,
			      enum basic_flip_cursor order,
			      unsigned flags)
{
	struct drm_mode_cursor arg[2];
	struct drm_event_vblank vbl;
	struct igt_fb fb_info, cursor_fb, cursor_fb2, argb_fb;
	unsigned vblank_start;
	enum pipe pipe = find_connected_pipe(display, false);
	uint32_t *busy;

	if (mode >= flip_test_atomic)
		igt_require(display->is_atomic);

	igt_require(set_fb_on_crtc(display, pipe, &fb_info));

	igt_create_color_fb(display->drm_fd, 64, 64, DRM_FORMAT_ARGB8888, 0, 1., 1., 1., &cursor_fb);
	set_cursor_on_pipe(display, pipe, &cursor_fb);
	populate_cursor_args(display, pipe, arg, &cursor_fb);

	prepare_flip_test(display, mode, pipe, pipe, arg, &fb_info, &argb_fb, &cursor_fb2);

	igt_display_commit2(display, display->is_atomic ? COMMIT_ATOMIC : COMMIT_LEGACY);

	/* Quick sanity check that we can update a cursor in a single vblank */
	vblank_start = get_vblank(display->drm_fd, pipe, DRM_VBLANK_NEXTONMISS);
	igt_assert_eq(get_vblank(display->drm_fd, pipe, 0), vblank_start);
	do_ioctl(display->drm_fd, DRM_IOCTL_MODE_CURSOR, &arg[0]);
	igt_assert_eq(get_vblank(display->drm_fd, pipe, 0), vblank_start);

	/* Bind the cursor first to warm up */
	do_ioctl(display->drm_fd, DRM_IOCTL_MODE_CURSOR, &arg[0]);

	/* Start with a synchronous query to align with the vblank */
	vblank_start = get_vblank(display->drm_fd, pipe, DRM_VBLANK_NEXTONMISS);

	busy = NULL;
	if (flags & BASIC_BUSY)
		busy = make_fb_busy(display->drm_fd, &fb_info);

	switch (order) {
	case FLIP_BEFORE_CURSOR:
		switch (mode) {
		default:
			flip_nonblocking(display, pipe, mode >= flip_test_atomic, &fb_info);
			break;
		case flip_test_atomic_transitions:
		case flip_test_atomic_transitions_varying_size:
			transition_nonblocking(display, pipe, &fb_info, &argb_fb, 0);
			break;
		}
		igt_assert_eq(get_vblank(display->drm_fd, pipe, 0), vblank_start);

		do_ioctl(display->drm_fd, DRM_IOCTL_MODE_CURSOR, &arg[0]);
		break;

	case FLIP_AFTER_CURSOR:
		do_ioctl(display->drm_fd, DRM_IOCTL_MODE_CURSOR, &arg[0]);
		igt_assert_eq(get_vblank(display->drm_fd, pipe, 0), vblank_start);

		switch (mode) {
		default:
			flip_nonblocking(display, pipe, mode >= flip_test_atomic, &fb_info);
			break;
		case flip_test_atomic_transitions:
		case flip_test_atomic_transitions_varying_size:
			transition_nonblocking(display, pipe, &fb_info, &argb_fb, 0);
			break;
		}
	}

	if (!cursor_slowpath(mode))
		igt_assert_eq(get_vblank(display->drm_fd, pipe, 0), vblank_start);
	else
		igt_assert_lte(get_vblank(display->drm_fd, pipe, 0), vblank_start + 1);

	if (busy) {
		struct pollfd pfd = { display->drm_fd, POLLIN };
		igt_assert(poll(&pfd, 1, 0) == 0);
		finish_fb_busy(busy);
	}

	igt_set_timeout(1, "Stuck page flip");
	igt_ignore_warn(read(display->drm_fd, &vbl, sizeof(vbl)));
	igt_assert_eq(get_vblank(display->drm_fd, pipe, 0), vblank_start + 1);
	igt_reset_timeout();

	do_cleanup_display(display);
	igt_remove_fb(display->drm_fd, &fb_info);
	igt_remove_fb(display->drm_fd, &cursor_fb);

	if (argb_fb.gem_handle)
		igt_remove_fb(display->drm_fd, &argb_fb);
	if (cursor_fb2.gem_handle)
		igt_remove_fb(display->drm_fd, &cursor_fb2);
}

static int
get_cursor_updates_per_vblank(igt_display_t *display, enum pipe pipe,
			      struct drm_mode_cursor *arg)
{
	int target;

	for (target = 65536; target; target /= 2) {
		unsigned vblank_start = get_vblank(display->drm_fd, pipe, DRM_VBLANK_NEXTONMISS);

		igt_assert_eq(get_vblank(display->drm_fd, pipe, 0), vblank_start);

		for (int n = 0; n < target; n++)
			do_ioctl(display->drm_fd, DRM_IOCTL_MODE_CURSOR, arg);
		if (get_vblank(display->drm_fd, pipe, 0) == vblank_start)
			break;
	}

	/*
	  * Divide by 4, to handle variations in amount of vblanks
	  * caused by cpufreq throttling.
	  */
	target /= 4;
	igt_require(target > 1);

	igt_debug("Using a target of %d cursor updates per quarter-vblank\n", target);

	return target;
}

static int sched_getcpu()
{
	struct kinfo_proc ki;
	int mib[] = {
		CTL_KERN,
		KERN_PROC,
		KERN_PROC_PID,
		getpid(),
	};
	u_int miblen = sizeof(mib) / sizeof(mib[0]);
	size_t size = sizeof(ki);
	if (sysctl(mib, miblen, &ki, &size, NULL, 0))
		return 0;

	return ki.ki_oncpu;
}

static void flip_vs_cursor(igt_display_t *display, enum flip_test mode, int nloops)
{
	struct drm_mode_cursor arg[2];
	struct drm_event_vblank vbl;
	struct igt_fb fb_info, cursor_fb, cursor_fb2, argb_fb;
	unsigned vblank_start;
	int target, cpu;
	enum pipe pipe = find_connected_pipe(display, false);
	volatile unsigned long *shared;
	cpu_set_t mask, oldmask;

	if (mode >= flip_test_atomic)
		igt_require(display->is_atomic);

	igt_require(set_fb_on_crtc(display, pipe, &fb_info));

	igt_create_color_fb(display->drm_fd, 64, 64, DRM_FORMAT_ARGB8888, 0, 1., 1., 1., &cursor_fb);
	set_cursor_on_pipe(display, pipe, &cursor_fb);
	populate_cursor_args(display, pipe, arg, &cursor_fb);

	prepare_flip_test(display, mode, pipe, pipe, arg, &fb_info, &argb_fb, &cursor_fb2);

	igt_display_commit2(display, display->is_atomic ? COMMIT_ATOMIC : COMMIT_LEGACY);

	if (nloops)
		target = get_cursor_updates_per_vblank(display, pipe, &arg[0]);
	else
		target = 1;

	vblank_start = get_vblank(display->drm_fd, pipe, DRM_VBLANK_NEXTONMISS);
	igt_assert_eq(get_vblank(display->drm_fd, pipe, 0), vblank_start);
	for (int n = 0; n < target; n++)
		do_ioctl(display->drm_fd, DRM_IOCTL_MODE_CURSOR, &arg[0]);
	igt_assert_eq(get_vblank(display->drm_fd, pipe, 0), vblank_start);

	/*
	 * There are variations caused by using cpu frequency changing. To
	 * eliminate those we force this test to run on the same cpu as an
	 * idle thread that does a busy loop of sched_yield(); The effect is
	 * that we don't throttle the cpu to a lower frequency, and the
	 * variations caused by cpu speed changing are eliminated.
	 */
	if (target > 1) {
		shared = mmap(NULL, 4096, PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
		igt_assert(shared != MAP_FAILED);

		cpu = sched_getcpu();
		igt_assert(cpu >= 0);

		CPU_ZERO(&mask);
		CPU_SET(cpu, &mask);
		cpuset_getaffinity(CPU_LEVEL_WHICH, CPU_WHICH_PID, -1, sizeof(cpuset_t), &oldmask);
		cpuset_setaffinity(CPU_LEVEL_WHICH, CPU_WHICH_PID, -1, sizeof(cpuset_t), &mask);

		shared[0] = 0;

		igt_fork(child, 1) {
			struct sched_param parm = { .sched_priority = 0 };

			igt_assert(sched_setscheduler(0, SCHED_OTHER, &parm) == 0);

			while (!shared[0])
				sched_yield();
		}
	}

	do {
		/* Bind the cursor first to warm up */
		do_ioctl(display->drm_fd, DRM_IOCTL_MODE_CURSOR, &arg[nloops & 1]);

		/* Start with a synchronous query to align with the vblank */
		vblank_start = get_vblank(display->drm_fd, pipe, DRM_VBLANK_NEXTONMISS);
		switch (mode) {
		default:
			flip_nonblocking(display, pipe, mode >= flip_test_atomic, &fb_info);
			break;
		case flip_test_atomic_transitions:
		case flip_test_atomic_transitions_varying_size:
			transition_nonblocking(display, pipe, &fb_info, &argb_fb, (nloops & 2) /2);
			break;
		}

		/* The nonblocking flip should not have delayed us */
		igt_assert_eq(get_vblank(display->drm_fd, pipe, 0), vblank_start);
		for (int n = 0; n < target; n++)
			do_ioctl(display->drm_fd, DRM_IOCTL_MODE_CURSOR, &arg[nloops & 1]);

		/* Nor should it have delayed the following cursor update */
		if (!cursor_slowpath(mode))
			igt_assert_eq(get_vblank(display->drm_fd, pipe, 0), vblank_start);
		else
			igt_assert_lte(get_vblank(display->drm_fd, pipe, 0), vblank_start + 1);

		igt_set_timeout(1, "Stuck page flip");
		igt_ignore_warn(read(display->drm_fd, &vbl, sizeof(vbl)));
		igt_assert_eq(get_vblank(display->drm_fd, pipe, 0), vblank_start + 1);
		igt_reset_timeout();
	} while (nloops--);

	if (target > 1) {
		shared[0] = 1;
		igt_waitchildren();
		munmap((void *)shared, 4096);
		cpuset_setaffinity(CPU_LEVEL_WHICH, CPU_WHICH_PID, -1, sizeof(cpuset_t), &oldmask);
	}

	do_cleanup_display(display);
	igt_remove_fb(display->drm_fd, &fb_info);
	igt_remove_fb(display->drm_fd, &cursor_fb);

	if (argb_fb.gem_handle)
		igt_remove_fb(display->drm_fd, &argb_fb);
	if (cursor_fb2.gem_handle)
		igt_remove_fb(display->drm_fd, &cursor_fb2);
}

static bool skip_on_unsupported_nonblocking_modeset(igt_display_t *display)
{
	enum pipe pipe;
	int ret;

	igt_display_commit_atomic(display, DRM_MODE_ATOMIC_TEST_ONLY | DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);

	ret = igt_display_try_commit_atomic(display, DRM_MODE_ATOMIC_ALLOW_MODESET | DRM_MODE_ATOMIC_NONBLOCK, NULL);

	if (ret == -EINVAL)
		return true;

	igt_assert_eq(ret, 0);

	/* Force the next state to update all crtc's, to synchronize with the nonblocking modeset. */
	for_each_pipe(display, pipe)
		display->pipes[pipe].mode_changed = true;

	return false;
}

static void nonblocking_modeset_vs_cursor(igt_display_t *display, int loops)
{
	struct igt_fb fb_info, cursor_fb;
	igt_output_t *output;
	enum pipe pipe = find_connected_pipe(display, false);
	struct drm_mode_cursor arg[2];
	bool skip_test;
	igt_plane_t *cursor = NULL, *plane;

	igt_require(display->is_atomic);
	igt_require((output = set_fb_on_crtc(display, pipe, &fb_info)));
	igt_create_color_fb(display->drm_fd, 64, 64, DRM_FORMAT_ARGB8888, 0, 1., 1., 1., &cursor_fb);
	set_cursor_on_pipe(display, pipe, &cursor_fb);
	populate_cursor_args(display, pipe, arg, &cursor_fb);
	arg[0].flags |= DRM_MODE_CURSOR_BO;

	for_each_plane_on_pipe(display, pipe, plane) {
		if (!plane->is_cursor)
			continue;

		cursor = plane;
		break;
	}

	igt_skip_on(!cursor);

	if ((skip_test = skip_on_unsupported_nonblocking_modeset(display)))
		goto cleanup;

	/*
	 * Start disabled, because skip_on_unsupported_nonblocking_modeset
	 * will have enabled this pipe. No way around it, since the first
	 * atomic commit may be unreliable with amount of events sent.
	 */
	igt_output_set_pipe(output, PIPE_NONE);
	igt_display_commit2(display, COMMIT_ATOMIC);

	while (loops--) {
		unsigned flags;
		struct pollfd pfd = { display->drm_fd, POLLIN };
		struct drm_event_vblank vbl;

		flags = DRM_MODE_ATOMIC_ALLOW_MODESET;
		flags |= DRM_MODE_ATOMIC_NONBLOCK;
		flags |= DRM_MODE_PAGE_FLIP_EVENT;

		/*
		 * Test that a cursor update after a nonblocking modeset
		 * works as intended. It should block until the modeset completes.
		 */

		igt_output_set_pipe(output, pipe);
		igt_plane_set_fb(cursor, NULL);
		igt_display_commit_atomic(display, flags, NULL);

		igt_assert_eq(0, poll(&pfd, 1, 0));
		igt_assert_eq(0, pfd.revents);

		do_ioctl(display->drm_fd, DRM_IOCTL_MODE_CURSOR, &arg[0]);

		igt_assert_eq(1, poll(&pfd, 1, 0));
		igt_assert_eq(POLLIN, pfd.revents);

		igt_set_timeout(1, "Stuck page flip");
		igt_ignore_warn(read(display->drm_fd, &vbl, sizeof(vbl)));
		igt_reset_timeout();

		igt_output_set_pipe(output, PIPE_NONE);
		igt_display_commit_atomic(display, flags, NULL);

		igt_assert_eq(0, poll(&pfd, 1, 0));
		igt_assert_eq(0, pfd.revents);

		/* Same for cursor on disabled crtc. */
		do_ioctl(display->drm_fd, DRM_IOCTL_MODE_CURSOR, &arg[0]);

		igt_assert_eq(1, poll(&pfd, 1, 0));
		igt_assert_eq(POLLIN, pfd.revents);

		igt_set_timeout(1, "Stuck page flip");
		igt_ignore_warn(read(display->drm_fd, &vbl, sizeof(vbl)));
		igt_reset_timeout();
	}

cleanup:
	do_cleanup_display(display);
	igt_remove_fb(display->drm_fd, &fb_info);
	igt_remove_fb(display->drm_fd, &cursor_fb);

	if (skip_test)
		igt_skip("Nonblocking modeset is not supported by this kernel\n");
}

static void two_screens_flip_vs_cursor(igt_display_t *display, int nloops, bool modeset)
{
	struct drm_mode_cursor arg[2], arg2[2];
	struct drm_event_vblank vbl;
	struct igt_fb fb_info, fb2_info, cursor_fb;
	unsigned vblank_start;
	enum pipe pipe = find_connected_pipe(display, false);
	enum pipe pipe2 = find_connected_pipe(display, true);
	igt_output_t *output2;
	bool skip_test = false;

	if (modeset)
		igt_require(display->is_atomic);

	igt_require(set_fb_on_crtc(display, pipe, &fb_info));
	igt_require((output2 = set_fb_on_crtc(display, pipe2, &fb2_info)));

	igt_create_color_fb(display->drm_fd, 64, 64, DRM_FORMAT_ARGB8888, 0, 1., 1., 1., &cursor_fb);
	set_cursor_on_pipe(display, pipe, &cursor_fb);
	populate_cursor_args(display, pipe, arg, &cursor_fb);

	arg[0].flags = arg[1].flags = DRM_MODE_CURSOR_BO;
	arg[1].handle = 0;
	arg[1].width = arg[1].height = 0;

	set_cursor_on_pipe(display, pipe2, &cursor_fb);
	populate_cursor_args(display, pipe2, arg2, &cursor_fb);

	arg2[0].flags = arg2[1].flags = DRM_MODE_CURSOR_BO;
	arg2[0].handle = 0;
	arg2[0].width = arg2[0].height = 0;

	if (modeset && (skip_test = skip_on_unsupported_nonblocking_modeset(display)))
		goto cleanup;

	igt_display_commit2(display, display->is_atomic ? COMMIT_ATOMIC : COMMIT_LEGACY);

	vblank_start = get_vblank(display->drm_fd, pipe, DRM_VBLANK_NEXTONMISS);
	igt_assert_eq(get_vblank(display->drm_fd, pipe, 0), vblank_start);
	do_ioctl(display->drm_fd, DRM_IOCTL_MODE_CURSOR, &arg[0]);
	do_ioctl(display->drm_fd, DRM_IOCTL_MODE_CURSOR, &arg2[0]);
	igt_assert_eq(get_vblank(display->drm_fd, pipe, 0), vblank_start);

	while (nloops--) {
		/* Start with a synchronous query to align with the vblank */
		vblank_start = get_vblank(display->drm_fd, pipe, DRM_VBLANK_NEXTONMISS);
		do_ioctl(display->drm_fd, DRM_IOCTL_MODE_CURSOR, &arg[nloops & 1]);


		flip_nonblocking(display, pipe, false, &fb_info);

		igt_assert_eq(get_vblank(display->drm_fd, pipe, 0), vblank_start);

		do_ioctl(display->drm_fd, DRM_IOCTL_MODE_CURSOR, &arg[nloops & 1]);
		if (!modeset) {
			do_ioctl(display->drm_fd, DRM_IOCTL_MODE_CURSOR, &arg2[nloops & 1]);
			do_ioctl(display->drm_fd, DRM_IOCTL_MODE_CURSOR, &arg[nloops & 1]);
			do_ioctl(display->drm_fd, DRM_IOCTL_MODE_CURSOR, &arg2[nloops & 1]);
		} else {
			do_ioctl(display->drm_fd, DRM_IOCTL_MODE_CURSOR, &arg[nloops & 1]);

			igt_output_set_pipe(output2, (nloops & 1) ? PIPE_NONE : pipe2);
			igt_display_commit_atomic(display, DRM_MODE_ATOMIC_ALLOW_MODESET | DRM_MODE_ATOMIC_NONBLOCK, NULL);

			do_ioctl(display->drm_fd, DRM_IOCTL_MODE_CURSOR, &arg[nloops & 1]);
		}

		igt_assert_eq(get_vblank(display->drm_fd, pipe, 0), vblank_start);

		igt_set_timeout(1, "Stuck page flip");
		igt_ignore_warn(read(display->drm_fd, &vbl, sizeof(vbl)));
		igt_assert_eq(get_vblank(display->drm_fd, pipe, 0), vblank_start + 1);
		igt_reset_timeout();

		if (modeset) {
			/* wait for pending modeset to complete, to prevent -EBUSY */
			display->pipes[pipe2].mode_changed = true;
			igt_display_commit2(display, COMMIT_ATOMIC);
		}
	}

cleanup:
	do_cleanup_display(display);
	igt_remove_fb(display->drm_fd, &fb_info);
	igt_remove_fb(display->drm_fd, &fb2_info);
	igt_remove_fb(display->drm_fd, &cursor_fb);

	if (skip_test)
		igt_skip("Nonblocking modeset is not supported by this kernel\n");
}

static void cursor_vs_flip(igt_display_t *display, enum flip_test mode, int nloops)
{
	struct drm_mode_cursor arg[2];
	struct drm_event_vblank vbl;
	struct igt_fb fb_info, cursor_fb, cursor_fb2, argb_fb;
	unsigned vblank_start, vblank_last;
	volatile unsigned long *shared;
	long target;
	enum pipe pipe = find_connected_pipe(display, false);
	igt_output_t *output;
	uint32_t vrefresh;

	if (mode >= flip_test_atomic)
		igt_require(display->is_atomic);

	shared = mmap(NULL, 4096, PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
	igt_assert(shared != MAP_FAILED);

	igt_require((output = set_fb_on_crtc(display, pipe, &fb_info)));
	vrefresh = igt_output_get_mode(output)->vrefresh;

	igt_create_color_fb(display->drm_fd, 64, 64, DRM_FORMAT_ARGB8888, 0, 1., 1., 1., &cursor_fb);
	set_cursor_on_pipe(display, pipe, &cursor_fb);
	populate_cursor_args(display, pipe, arg, &cursor_fb);

	prepare_flip_test(display, mode, pipe, pipe, arg, &fb_info, &argb_fb, &cursor_fb2);

	igt_display_commit2(display, display->is_atomic ? COMMIT_ATOMIC : COMMIT_LEGACY);

	target = get_cursor_updates_per_vblank(display, pipe, &arg[0]);

	for (int i = 0; i < nloops; i++) {
		shared[0] = 0;
		igt_fork(child, 1) {
			unsigned long count = 0;
			while (!shared[0]) {
				do_ioctl(display->drm_fd, DRM_IOCTL_MODE_CURSOR, &arg[i & 1]);
				count++;
			}
			igt_debug("child: %lu cursor updates\n", count);
			shared[0] = count;
		}

		switch (mode) {
		default:
			flip_nonblocking(display, pipe, mode >= flip_test_atomic, &fb_info);
			break;
		case flip_test_atomic_transitions:
		case flip_test_atomic_transitions_varying_size:
			transition_nonblocking(display, pipe, &fb_info, &argb_fb, (i & 2) >> 1);
			break;
		}

		igt_assert_eq(read(display->drm_fd, &vbl, sizeof(vbl)), sizeof(vbl));
		vblank_start = vblank_last = vbl.sequence;
		for (int n = 0; n < vrefresh / 2; n++) {
			flip_nonblocking(display, pipe, mode >= flip_test_atomic, &fb_info);

			igt_assert_eq(read(display->drm_fd, &vbl, sizeof(vbl)), sizeof(vbl));
			if (vbl.sequence != vblank_last + 1) {
				igt_info("page flip %d was delayed, missed %d frames\n",
					 n, vbl.sequence - vblank_last - 1);
			}
			vblank_last = vbl.sequence;
		}

		if (!cursor_slowpath(mode))
			igt_assert_lte(vbl.sequence, vblank_start + 5 * vrefresh / 8);

		shared[0] = 1;
		igt_waitchildren();
		igt_assert_f(shared[0] > vrefresh*target / 2,
			     "completed %lu cursor updated in a period of %u flips, "
			     "we expect to complete approximately %lu updates, "
			     "with the threshold set at %lu\n",
			     shared[0], vrefresh / 2,
			     vrefresh*target, vrefresh*target / 2);
	}

	do_cleanup_display(display);
	igt_remove_fb(display->drm_fd, &fb_info);
	igt_remove_fb(display->drm_fd, &cursor_fb);
	munmap((void *)shared, 4096);
	if (argb_fb.gem_handle)
		igt_remove_fb(display->drm_fd, &argb_fb);
	if (cursor_fb2.gem_handle)
		igt_remove_fb(display->drm_fd, &cursor_fb2);
}

static void two_screens_cursor_vs_flip(igt_display_t *display, int nloops, bool modeset)
{
	struct drm_mode_cursor arg[2], arg2[2];
	struct drm_event_vblank vbl;
	struct igt_fb fb_info, fb2_info, cursor_fb;
	unsigned vblank_start, vblank_last;
	volatile unsigned long *shared;
	int target;
	enum pipe pipe = find_connected_pipe(display, false);
	enum pipe pipe2 = find_connected_pipe(display, true);
	igt_output_t *output2;
	bool skip_test = false;

	shared = mmap(NULL, 4096, PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
	igt_assert(shared != MAP_FAILED);

	if (modeset)
		igt_require(display->is_atomic);

	igt_require(set_fb_on_crtc(display, pipe, &fb_info));
	igt_require((output2 = set_fb_on_crtc(display, pipe2, &fb2_info)));

	igt_create_color_fb(display->drm_fd, 64, 64, DRM_FORMAT_ARGB8888, 0, 1., 1., 1., &cursor_fb);
	set_cursor_on_pipe(display, pipe, &cursor_fb);
	populate_cursor_args(display, pipe, arg, &cursor_fb);

	arg[0].flags = arg[1].flags = DRM_MODE_CURSOR_BO;
	arg[1].handle = 0;
	arg[1].width = arg[1].height = 0;

	set_cursor_on_pipe(display, pipe2, &cursor_fb);
	populate_cursor_args(display, pipe2, arg2, &cursor_fb);

	arg2[0].flags = arg2[1].flags = DRM_MODE_CURSOR_BO;
	arg2[0].handle = 0;
	arg2[0].width = arg2[0].height = 0;

	if (modeset && (skip_test = skip_on_unsupported_nonblocking_modeset(display)))
		goto cleanup;

	igt_display_commit2(display, display->is_atomic ? COMMIT_ATOMIC : COMMIT_LEGACY);

	for (target = 65536; target; target /= 2) {
		vblank_start = get_vblank(display->drm_fd, pipe, DRM_VBLANK_NEXTONMISS);
		igt_assert_eq(get_vblank(display->drm_fd, pipe, 0), vblank_start);

		if (!modeset)
			do_ioctl(display->drm_fd, DRM_IOCTL_MODE_CURSOR, &arg2[0]);

		for (int n = 0; n < target; n++) {
			do_ioctl(display->drm_fd, DRM_IOCTL_MODE_CURSOR, &arg[0]);
		}
		if (get_vblank(display->drm_fd, pipe, 0) == vblank_start)
			break;
	}

	/*
	  * Divide by 4, to handle variations in amount of vblanks
	  * caused by cpufreq throttling.
	  */
	target /= 4;

	igt_debug("Using a target of %d cursor updates per quarter-vblank\n",
		  target);

	igt_require(target > 1);

	for (int i = 0; i < nloops; i++) {
		shared[0] = 0;
		igt_fork(child, 1) {
			unsigned long count = 0;

			if (!modeset)
				do_ioctl(display->drm_fd, DRM_IOCTL_MODE_CURSOR, &arg2[i & 1]);
			else {
				igt_output_set_pipe(output2, (i & 1) ? pipe2 : PIPE_NONE);
				igt_display_commit_atomic(display, DRM_MODE_ATOMIC_ALLOW_MODESET |
							  DRM_MODE_ATOMIC_NONBLOCK, NULL);
			}

			while (!shared[0]) {
				do_ioctl(display->drm_fd, DRM_IOCTL_MODE_CURSOR, &arg[i & 1]);
				count++;
			}
			igt_debug("child: %lu cursor updates\n", count);
			shared[0] = count;
		}

		flip_nonblocking(display, pipe, modeset, &fb_info);

		igt_assert_eq(read(display->drm_fd, &vbl, sizeof(vbl)), sizeof(vbl));
		vblank_start = vblank_last = vbl.sequence;
		for (int n = 0; n < 60; n++) {
			flip_nonblocking(display, pipe, modeset, &fb_info);

			igt_assert_eq(read(display->drm_fd, &vbl, sizeof(vbl)), sizeof(vbl));
			if (vbl.sequence != vblank_last + 1) {
				igt_warn("page flip %d was delayed, missed %d frames\n",
					 n, vbl.sequence - vblank_last - 1);
			}
			vblank_last = vbl.sequence;
		}
		igt_assert_eq(vbl.sequence, vblank_start + 60);

		shared[0] = 1;
		igt_waitchildren();
		igt_assert_f(shared[0] > 60*target,
			     "completed %lu cursor updated in a period of 60 flips, "
			     "we expect to complete approximately %lu updateds, "
			     "with the threshold set at %lu\n",
			     shared[0], 2*60ul*target, 60ul*target);
	}

cleanup:
	do_cleanup_display(display);
	igt_remove_fb(display->drm_fd, &fb_info);
	igt_remove_fb(display->drm_fd, &fb2_info);
	igt_remove_fb(display->drm_fd, &cursor_fb);
	munmap((void *)shared, 4096);

	if (skip_test)
		igt_skip("Nonblocking modeset is not supported by this kernel\n");
}

static void flip_vs_cursor_crc(igt_display_t *display, bool atomic)
{
	struct drm_mode_cursor arg[2];
	struct drm_event_vblank vbl;
	struct igt_fb fb_info, cursor_fb;
	unsigned vblank_start;
	enum pipe pipe = find_connected_pipe(display, false);
	igt_pipe_crc_t *pipe_crc;
	igt_crc_t crcs[3];

	if (atomic)
		igt_require(display->is_atomic);

	igt_require(set_fb_on_crtc(display, pipe, &fb_info));

	igt_create_color_fb(display->drm_fd, 64, 64, DRM_FORMAT_ARGB8888, 0, 1., 1., 1., &cursor_fb);
	populate_cursor_args(display, pipe, arg, &cursor_fb);

	igt_display_commit2(display, display->is_atomic ? COMMIT_ATOMIC : COMMIT_LEGACY);

	pipe_crc = igt_pipe_crc_new(pipe, INTEL_PIPE_CRC_SOURCE_AUTO);

	set_cursor_on_pipe(display, pipe, &cursor_fb);
	igt_display_commit2(display, COMMIT_UNIVERSAL);

	/* Collect reference crcs, crcs[0] last. */
	do_ioctl(display->drm_fd, DRM_IOCTL_MODE_CURSOR, &arg[1]);
	igt_pipe_crc_collect_crc(pipe_crc, &crcs[1]);

	do_ioctl(display->drm_fd, DRM_IOCTL_MODE_CURSOR, &arg[0]);
	igt_pipe_crc_collect_crc(pipe_crc, &crcs[0]);

	/* Disable cursor, and immediately queue a flip. Check if resulting crc is correct. */
	for (int i = 1; i >= 0; i--) {
		vblank_start = get_vblank(display->drm_fd, pipe, DRM_VBLANK_NEXTONMISS);

		flip_nonblocking(display, pipe, atomic, &fb_info);
		do_ioctl(display->drm_fd, DRM_IOCTL_MODE_CURSOR, &arg[i]);

		igt_assert_eq(get_vblank(display->drm_fd, pipe, 0), vblank_start);

		igt_set_timeout(1, "Stuck page flip");
		igt_ignore_warn(read(display->drm_fd, &vbl, sizeof(vbl)));
		igt_reset_timeout();

		igt_assert_eq(get_vblank(display->drm_fd, pipe, 0), vblank_start + 1);

		igt_pipe_crc_collect_crc(pipe_crc, &crcs[2]);

		igt_assert_crc_equal(&crcs[i], &crcs[2]);
	}

	do_cleanup_display(display);
	igt_remove_fb(display->drm_fd, &fb_info);
	igt_remove_fb(display->drm_fd, &cursor_fb);
	igt_pipe_crc_free(pipe_crc);
}

static void flip_vs_cursor_busy_crc(igt_display_t *display, bool atomic)
{
	struct drm_mode_cursor arg[2];
	struct drm_event_vblank vbl;
	struct igt_fb fb_info[2], cursor_fb;
	unsigned vblank_start;
	enum pipe pipe = find_connected_pipe(display, false);
	igt_pipe_crc_t *pipe_crc;
	igt_crc_t crcs[3];

	if (atomic)
		igt_require(display->is_atomic);

	igt_require(set_fb_on_crtc(display, pipe, &fb_info[0]));
	igt_create_color_pattern_fb(display->drm_fd, fb_info[0].width, fb_info[0].height,
				    DRM_FORMAT_XRGB8888, LOCAL_I915_FORMAT_MOD_X_TILED, .1, .1, .1, &fb_info[1]);

	igt_create_color_fb(display->drm_fd, 64, 64, DRM_FORMAT_ARGB8888, 0, 1., 1., 1., &cursor_fb);
	populate_cursor_args(display, pipe, arg, &cursor_fb);

	igt_display_commit2(display, display->is_atomic ? COMMIT_ATOMIC : COMMIT_LEGACY);

	pipe_crc = igt_pipe_crc_new(pipe, INTEL_PIPE_CRC_SOURCE_AUTO);

	set_cursor_on_pipe(display, pipe, &cursor_fb);
	igt_display_commit2(display, COMMIT_UNIVERSAL);

	/* Collect reference crcs, crc[0] last for the loop. */
	do_ioctl(display->drm_fd, DRM_IOCTL_MODE_CURSOR, &arg[1]);
	igt_pipe_crc_collect_crc(pipe_crc, &crcs[1]);

	do_ioctl(display->drm_fd, DRM_IOCTL_MODE_CURSOR, &arg[0]);
	igt_pipe_crc_collect_crc(pipe_crc, &crcs[0]);

	/*
	  * Set fb 1 on primary at least once before flipping to force
	  * setting the correct cache level, else we get a stall in the
	  * page flip handler.
	  */
	igt_plane_set_fb(&display->pipes[pipe].planes[IGT_PLANE_PRIMARY], &fb_info[1]);
	igt_display_commit2(display, COMMIT_UNIVERSAL);

	igt_plane_set_fb(&display->pipes[pipe].planes[IGT_PLANE_PRIMARY], &fb_info[0]);
	igt_display_commit2(display, COMMIT_UNIVERSAL);

	/* Disable cursor, and immediately queue a flip. Check if resulting crc is correct. */
	for (int i = 1; i >= 0; i--) {
		uint32_t *busy;

		busy = make_fb_busy(display->drm_fd, &fb_info[1]);

		vblank_start = get_vblank(display->drm_fd, pipe, DRM_VBLANK_NEXTONMISS);

		flip_nonblocking(display, pipe, atomic, &fb_info[1]);
		do_ioctl(display->drm_fd, DRM_IOCTL_MODE_CURSOR, &arg[i]);

		igt_assert_eq(get_vblank(display->drm_fd, pipe, 0), vblank_start);

		igt_pipe_crc_collect_crc(pipe_crc, &crcs[2]);

		finish_fb_busy(busy);

		igt_set_timeout(1, "Stuck page flip");
		igt_ignore_warn(read(display->drm_fd, &vbl, sizeof(vbl)));
		igt_reset_timeout();

		igt_assert_lte(vblank_start + 1, get_vblank(display->drm_fd, pipe, 0));

		igt_plane_set_fb(&display->pipes[pipe].planes[IGT_PLANE_PRIMARY], &fb_info[0]);
		igt_display_commit2(display, COMMIT_UNIVERSAL);

		igt_assert_crc_equal(&crcs[i], &crcs[2]);
	}

	do_cleanup_display(display);
	igt_remove_fb(display->drm_fd, &fb_info[1]);
	igt_remove_fb(display->drm_fd, &fb_info[0]);
	igt_remove_fb(display->drm_fd, &cursor_fb);
	igt_pipe_crc_free(pipe_crc);
}

igt_main
{
	const int ncpus = sysconf(_SC_NPROCESSORS_ONLN);
	igt_display_t display = { .drm_fd = -1 };
	int i;

	igt_skip_on_simulation();

	igt_fixture {
		display.drm_fd = drm_open_driver_master(DRIVER_ANY);
		kmstest_set_vt_graphics_mode();

		igt_display_init(&display, display.drm_fd);
		igt_require(display.n_pipes > 0);
	}

	igt_subtest_group {
		for (int n = 0; n < I915_MAX_PIPES; n++) {
			errno = 0;

			igt_fixture {
				igt_skip_on(n >= display.n_pipes);
			}

			igt_subtest_f("pipe-%s-single-bo", kmstest_pipe_name(n))
				stress(&display, n, 1, DRM_MODE_CURSOR_BO, 20);
			igt_subtest_f("pipe-%s-single-move", kmstest_pipe_name(n))
				stress(&display, n, 1, DRM_MODE_CURSOR_MOVE, 20);

			igt_subtest_f("pipe-%s-forked-bo", kmstest_pipe_name(n))
				stress(&display, n, ncpus, DRM_MODE_CURSOR_BO, 20);
			igt_subtest_f("pipe-%s-forked-move", kmstest_pipe_name(n))
				stress(&display, n, ncpus, DRM_MODE_CURSOR_MOVE, 20);

			igt_subtest_f("pipe-%s-torture-bo", kmstest_pipe_name(n))
				stress(&display, n, -ncpus, DRM_MODE_CURSOR_BO, 20);
			igt_subtest_f("pipe-%s-torture-move", kmstest_pipe_name(n))
				stress(&display, n, -ncpus, DRM_MODE_CURSOR_MOVE, 20);
		}
	}

	igt_subtest("all-pipes-single-bo")
		stress(&display, -1, 1, DRM_MODE_CURSOR_BO, 20);
	igt_subtest("all-pipes-single-move")
		stress(&display, -1, 1, DRM_MODE_CURSOR_MOVE, 20);

	igt_subtest("all-pipes-forked-bo")
		stress(&display, -1, ncpus, DRM_MODE_CURSOR_BO, 20);
	igt_subtest("all-pipes-forked-move")
		stress(&display, -1, ncpus, DRM_MODE_CURSOR_MOVE, 20);

	igt_subtest("all-pipes-torture-bo")
		stress(&display, -1, -ncpus, DRM_MODE_CURSOR_BO, 20);
	igt_subtest("all-pipes-torture-move")
		stress(&display, -1, -ncpus, DRM_MODE_CURSOR_MOVE, 20);

	igt_subtest("nonblocking-modeset-vs-cursor-atomic")
		nonblocking_modeset_vs_cursor(&display, 1);

	igt_subtest("long-nonblocking-modeset-vs-cursor-atomic")
		nonblocking_modeset_vs_cursor(&display, 16);

	igt_subtest("2x-flip-vs-cursor-legacy")
		two_screens_flip_vs_cursor(&display, 8, false);

	igt_subtest("2x-cursor-vs-flip-legacy")
		two_screens_cursor_vs_flip(&display, 4, false);

	igt_subtest("2x-long-flip-vs-cursor-legacy")
		two_screens_flip_vs_cursor(&display, 150, false);

	igt_subtest("2x-long-cursor-vs-flip-legacy")
		two_screens_cursor_vs_flip(&display, 50, false);

	igt_subtest("2x-nonblocking-modeset-vs-cursor-atomic")
		two_screens_flip_vs_cursor(&display, 8, true);

	igt_subtest("2x-cursor-vs-nonblocking-modeset-atomic")
		two_screens_cursor_vs_flip(&display, 4, true);

	igt_subtest("2x-long-nonblocking-modeset-vs-cursor-atomic")
		two_screens_flip_vs_cursor(&display, 150, true);

	igt_subtest("2x-long-cursor-vs-nonblocking-modeset-atomic")
		two_screens_cursor_vs_flip(&display, 50, true);

	igt_subtest("flip-vs-cursor-crc-legacy")
		flip_vs_cursor_crc(&display, false);

	igt_subtest("flip-vs-cursor-crc-atomic")
		flip_vs_cursor_crc(&display, true);

	igt_subtest("flip-vs-cursor-busy-crc-legacy")
		flip_vs_cursor_busy_crc(&display, false);

	igt_subtest("flip-vs-cursor-busy-crc-atomic")
		flip_vs_cursor_busy_crc(&display, true);

	for (i = 0; i <= flip_test_last; i++) {
		const char *modes[flip_test_last+1] = {
			"legacy",
			"varying-size",
			"toggle",
			"atomic",
			"atomic-transitions",
			"atomic-transitions-varying-size"
		};
		const char *prefix = "short-";

		switch (i) {
		case flip_test_legacy:
		case flip_test_varying_size:
		case flip_test_atomic:
			prefix = "basic-";
			break;
		default: break;
		}

		igt_subtest_f("%sflip-before-cursor-%s", prefix, modes[i])
			basic_flip_cursor(&display, i, FLIP_BEFORE_CURSOR, 0);

		if (!cursor_slowpath(i))
			igt_subtest_f("%sbusy-flip-before-cursor-%s", prefix, modes[i])
				basic_flip_cursor(&display, i, FLIP_BEFORE_CURSOR,
						  BASIC_BUSY);

		igt_subtest_f("%sflip-after-cursor-%s", prefix, modes[i])
			basic_flip_cursor(&display, i, FLIP_AFTER_CURSOR, 0);

		igt_subtest_f("flip-vs-cursor-%s", modes[i])
			flip_vs_cursor(&display, i, 150);
		igt_subtest_f("cursor-vs-flip-%s", modes[i])
			cursor_vs_flip(&display, i, 50);

		igt_subtest_f("cursorA-vs-flipA-%s", modes[i])
			flip(&display, 0, 0, 10, i);

		igt_subtest_f("cursorA-vs-flipB-%s", modes[i])
			flip(&display, 0, 1, 10, i);

		igt_subtest_f("cursorB-vs-flipA-%s", modes[i])
			flip(&display, 1, 0, 10, i);

		igt_subtest_f("cursorB-vs-flipB-%s", modes[i])
			flip(&display, 1, 1, 10, i);
	}

	igt_fixture {
		igt_display_fini(&display);
	}
}
