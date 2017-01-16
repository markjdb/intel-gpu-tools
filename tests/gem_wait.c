/*
 * Copyright © 2012 Intel Corporation
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
 * Authors:
 *    Ben Widawsky <ben@bwidawsk.net>
 *
 */

#include "igt.h"

static int __gem_wait(int fd, struct drm_i915_gem_wait *w)
{
	int err;

	err = 0;
	if (igt_ioctl(fd, DRM_IOCTL_I915_GEM_WAIT, w))
		err = -errno;

	return err;
}

static void invalid_flags(int fd)
{
	struct drm_i915_gem_wait wait;

	memset(&wait, 0, sizeof(wait));
	wait.bo_handle = gem_create(fd, 4096);
	wait.timeout_ns = 1;
	/* NOTE: This test intentionally tests for just the next available flag.
	 * Don't "fix" this testcase without the ABI testcases for new flags
	 * first. */
	wait.flags = 1;

	igt_assert_eq(__gem_wait(fd, &wait), -EINVAL);

	gem_close(fd, wait.bo_handle);
}

static void invalid_buf(int fd)
{
	struct drm_i915_gem_wait wait;

	memset(&wait, 0, sizeof(wait));
	igt_assert_eq(__gem_wait(fd, &wait), -ENOENT);
}

#define BUSY 1
#define HANG 2
static void basic(int fd, unsigned engine, unsigned flags)
{
	igt_spin_t *spin = igt_spin_batch_new(fd, engine, 0);
	struct drm_i915_gem_wait wait = { spin->handle };

#ifndef __FreeBSD__
	igt_assert_eq(__gem_wait(fd, &wait), -ETIME);
#else
	igt_assert_eq(__gem_wait(fd, &wait), -ETIMEDOUT);
#endif

	if (flags & BUSY) {
		struct timespec tv = {};
		int timeout;

		timeout = 120;
		if ((flags & HANG) == 0) {
			igt_spin_batch_end(spin);
			timeout = 1;
		}

#ifndef __FreeBSD__
		while (__gem_wait(fd, &wait) == -ETIME)
			igt_assert(igt_seconds_elapsed(&tv) < timeout);
#else
		while (__gem_wait(fd, &wait) == -ETIMEDOUT)
			igt_assert(igt_seconds_elapsed(&tv) < timeout);
#endif
	} else {
		igt_spin_batch_set_timeout(spin, NSEC_PER_SEC);

		wait.timeout_ns = NSEC_PER_SEC / 2; /* 0.5s */
#ifndef __FreeBSD__
		igt_assert_eq(__gem_wait(fd, &wait), -ETIME);
#else
		igt_assert_eq(__gem_wait(fd, &wait), -ETIMEDOUT);
#endif
		igt_assert_eq_s64(wait.timeout_ns, 0);

		if ((flags & HANG) == 0) {
			wait.timeout_ns = NSEC_PER_SEC; /* 1.0s */
			igt_assert_eq(__gem_wait(fd, &wait), 0);
			igt_assert(wait.timeout_ns > 0);
		} else {
			wait.timeout_ns = -1;
			igt_assert_eq(__gem_wait(fd, &wait), 0);
			igt_assert(wait.timeout_ns == -1);
		}

		wait.timeout_ns = 0;
		igt_assert_eq(__gem_wait(fd, &wait), 0);
		igt_assert(wait.timeout_ns == 0);
	}

	igt_spin_batch_free(fd, spin);
}

igt_main
{
	const struct intel_execution_engine *e;
	int fd = -1;

	igt_skip_on_simulation();

	igt_fixture {
		fd = drm_open_driver_master(DRIVER_INTEL);
	}

	igt_subtest("invalid-flags")
		invalid_flags(fd);

	igt_subtest("invalid-buf")
		invalid_buf(fd);

	igt_subtest_group {
		igt_fixture {
			igt_fork_hang_detector(fd);
			igt_fork_signal_helper();
		}

		igt_subtest("basic-busy-all") {
			gem_quiescent_gpu(fd);
			basic(fd, -1, BUSY);
		}
		igt_subtest("basic-wait-all") {
			gem_quiescent_gpu(fd);
			basic(fd, -1, 0);
		}

		for (e = intel_execution_engines; e->name; e++) {
			igt_subtest_group {
				igt_subtest_f("busy-%s", e->name) {
					gem_quiescent_gpu(fd);
					basic(fd, e->exec_id | e->flags, BUSY);
				}
				igt_subtest_f("wait-%s", e->name) {
					gem_quiescent_gpu(fd);
					basic(fd, e->exec_id | e->flags, 0);
				}
			}
		}

		igt_fixture {
			igt_stop_signal_helper();
			igt_stop_hang_detector();
		}
	}

	igt_subtest_group {
		igt_hang_t hang;

		igt_fixture {
			hang = igt_allow_hang(fd, 0, 0);
			igt_fork_signal_helper();
		}

		igt_subtest("hang-busy-all") {
			gem_quiescent_gpu(fd);
			basic(fd, -1, BUSY | HANG);
		}
		igt_subtest("hang-wait-all") {
			gem_quiescent_gpu(fd);
			basic(fd, -1, HANG);
		}

		for (e = intel_execution_engines; e->name; e++) {
			igt_subtest_f("hang-busy-%s", e->name) {
				gem_quiescent_gpu(fd);
				basic(fd, e->exec_id | e->flags, HANG | BUSY);
			}
			igt_subtest_f("hang-wait-%s", e->name) {
				gem_quiescent_gpu(fd);
				basic(fd, e->exec_id | e->flags, HANG);
			}
		}

		igt_fixture {
			igt_stop_signal_helper();
			igt_disallow_hang(fd, hang);
		}
	}

	igt_fixture {
		close(fd);
	}
}
