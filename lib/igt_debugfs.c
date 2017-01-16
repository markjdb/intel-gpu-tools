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

#include <inttypes.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <i915_drm.h>

#include "drmtest.h"
#include "igt_aux.h"
#include "igt_kms.h"
#include "igt_debugfs.h"
#include "igt_sysfs.h"

/**
 * SECTION:igt_debugfs
 * @short_description: Support code for debugfs features
 * @title: debugfs
 * @include: igt.h
 *
 * This library provides helpers to access debugfs features. On top of some
 * basic functions to access debugfs files with e.g. igt_debugfs_open() it also
 * provides higher-level wrappers for some debugfs features.
 *
 * # Pipe CRC Support
 *
 * This library wraps up the kernel's support for capturing pipe CRCs into a
 * neat and tidy package. For the detailed usage see all the functions which
 * work on #igt_pipe_crc_t. This is supported on all platforms and outputs.
 *
 * Actually using pipe CRCs to write modeset tests is a bit tricky though, so
 * there is no way to directly check a CRC: Both the details of the plane
 * blending, color correction and other hardware and how exactly the CRC is
 * computed at each tap point vary by hardware generation and are not disclosed.
 *
 * The only way to use #igt_crc_t CRCs therefore is to compare CRCs among each
 * another either for equality or difference. Otherwise CRCs must be treated as
 * completely opaque values. Note that not even CRCs from different pipes or tap
 * points on the same platform can be compared. Hence only use
 * igt_assert_crc_equal() to inspect CRC values captured by the same
 * #igt_pipe_crc_t object.
 *
 * # Other debugfs interface wrappers
 *
 * This covers the miscellaneous debugfs interface wrappers:
 *
 * - drm/i915 supports interfaces to evict certain classes of gem buffer
 *   objects, see igt_drop_caches_set().
 *
 * - drm/i915 supports an interface to disable prefaulting, useful to test
 *   slow paths in ioctls. See igt_disable_prefault().
 */

/*
 * General debugfs helpers
 */

typedef struct {
	char root[128];
	char dri_path[128];
} igt_debugfs_t;

static bool is_mountpoint(const char *path)
{
	char buf[strlen(path) + 4];
	dev_t dot_dev, dotdot_dev;
	struct stat st;

	igt_assert_lt(snprintf(buf, sizeof(buf), "%s/.", path), sizeof(buf));
	igt_assert_eq(stat(buf, &st), 0);
	dot_dev = st.st_dev;

	igt_assert_lt(snprintf(buf, sizeof(buf), "%s/..", path), sizeof(buf));
	igt_assert_eq(stat(buf, &st), 0);
	dotdot_dev = st.st_dev;

	return dot_dev != dotdot_dev;
}

/**
 * igt_debugfs_mount:
 *
 * This attempts to locate where debugfs is mounted on the filesystem,
 * and if not found, will then try to mount debugfs at /sys/kernel/debug.
 *
 * Returns:
 * The path to the debugfs mount point (e.g. /sys/kernel/debug)
 */
const char *igt_debugfs_mount(void)
{
#ifndef __FreeBSD__
	struct stat st;

	if (stat("/debug/dri", &st) == 0)
		return "/debug";

	if (stat("/sys/kernel/debug/dri", &st) == 0)
		return "/sys/kernel/debug";

	igt_assert(is_mountpoint("/sys/kernel/debug") ||
		   mount("debug", "/sys/kernel/debug", "debugfs", 0, 0) == 0);

	return "/sys/kernel/debug";
#else
	return "/debug";
#endif
}

static bool __igt_debugfs_init(igt_debugfs_t *debugfs)
{
	struct stat st;
	int n;

	strcpy(debugfs->root, igt_debugfs_mount());
	for (n = 0; n < 16; n++) {
		int len = sprintf(debugfs->dri_path, "%s/dri/%d", debugfs->root, n);
		sprintf(debugfs->dri_path + len, "/i915_error_state");
		if (stat(debugfs->dri_path, &st) == 0) {
			debugfs->dri_path[len] = '\0';
			return true;
		}
	}

	debugfs->dri_path[0] = '\0';
	return false;
}

static igt_debugfs_t *__igt_debugfs_singleton(void)
{
	static igt_debugfs_t singleton;
	static bool init_done = false;

	if (init_done)
		return &singleton;

	if (__igt_debugfs_init(&singleton)) {
		init_done = true;
		return &singleton;
	} else {
		return NULL;
	}
}

/**
 * igt_debugfs_open:
 * @filename: name of the debugfs node to open
 * @mode: mode bits as used by open()
 *
 * This opens a debugfs file as a Unix file descriptor. The filename should be
 * relative to the drm device's root, i.e. without "drm/<minor>".
 *
 * Returns:
 * The Unix file descriptor for the debugfs file or -1 if that didn't work out.
 */
int igt_debugfs_open(const char *filename, int mode)
{
	char buf[1024];
	igt_debugfs_t *debugfs = __igt_debugfs_singleton();

	if (!debugfs)
		return -1;

	sprintf(buf, "%s/%s", debugfs->dri_path, filename);
	return open(buf, mode);
}

/**
 * igt_debugfs_fopen:
 * @filename: name of the debugfs node to open
 * @mode: mode string as used by fopen()
 *
 * This opens a debugfs file as a libc FILE. The filename should be
 * relative to the drm device's root, i.e. without "drm/<minor>".
 *
 * Returns:
 * The libc FILE pointer for the debugfs file or NULL if that didn't work out.
 */
FILE *igt_debugfs_fopen(const char *filename,
			const char *mode)
{
	char buf[1024];

	igt_debugfs_t *debugfs = __igt_debugfs_singleton();

	if (!debugfs)
		return NULL;

	sprintf(buf, "%s/%s", debugfs->dri_path, filename);
	return fopen(buf, mode);
}

/**
 * __igt_debugfs_read:
 * @filename: file name
 * @buf: buffer where the contents will be stored, allocated by the caller
 * @buf_size: size of the buffer
 *
 * This function opens the debugfs file, reads it, stores the content in the
 * provided buffer, then closes the file. Users should make sure that the buffer
 * provided is big enough to fit the whole file, plus one byte.
 */
void __igt_debugfs_read(const char *filename, char *buf, int buf_size)
{
	FILE *file;
	size_t n_read;

	file = igt_debugfs_fopen(filename, "r");
	igt_assert(file);

	n_read = fread(buf, 1, buf_size - 1, file);
	igt_assert(n_read > 0);
	igt_assert(feof(file));

	buf[n_read] = '\0';

	igt_assert(fclose(file) == 0);
}

/**
 * igt_debugfs_search:
 * @filename: file name
 * @substring: string to search for in @filename
 *
 * Searches each line in @filename for the substring specified in @substring.
 *
 * Returns: True if the @substring is found to occur in @filename
 */
bool igt_debugfs_search(const char *filename, const char *substring)
{
	FILE *file;
	size_t n = 0;
	char *line = NULL;
	bool matched = false;

	file = igt_debugfs_fopen(filename, "r");
	igt_assert(file);

	while (getline(&line, &n, file) >= 0) {
		matched = (strstr(line, substring) != NULL);
		if (matched)
			break;
	}

	free(line);
	fclose(file);

	return matched;
}

/*
 * Pipe CRC
 */

/**
 * igt_assert_crc_equal:
 * @a: first pipe CRC value
 * @b: second pipe CRC value
 *
 * Compares two CRC values and fails the testcase if they don't match with
 * igt_fail(). Note that due to CRC collisions CRC based testcase can only
 * assert that CRCs match, never that they are different. Otherwise there might
 * be random testcase failures when different screen contents end up with the
 * same CRC by chance.
 */
void igt_assert_crc_equal(const igt_crc_t *a, const igt_crc_t *b)
{
	int i;

	for (i = 0; i < a->n_words; i++)
		igt_assert_eq_u32(a->crc[i], b->crc[i]);
}

/**
 * igt_crc_to_string:
 * @crc: pipe CRC value to print
 *
 * This formats @crc into a string buffer which is owned by igt_crc_to_string().
 * The next call will override the buffer again, which makes this multithreading
 * unsafe.
 *
 * This should only ever be used for diagnostic debug output.
 */
char *igt_crc_to_string(igt_crc_t *crc)
{
	char buf[128];

	sprintf(buf, "%08x %08x %08x %08x %08x", crc->crc[0],
		crc->crc[1], crc->crc[2], crc->crc[3], crc->crc[4]);

	return strdup(buf);
}

#define MAX_CRC_ENTRIES 10
#define MAX_LINE_LEN (10 + 11 * MAX_CRC_ENTRIES + 1)

/* (6 fields, 8 chars each, space separated (5) + '\n') */
#define LEGACY_LINE_LEN       (6 * 8 + 5 + 1)

struct _igt_pipe_crc {
	int ctl_fd;
	int crc_fd;
	int flags;
	bool is_legacy;

	enum pipe pipe;
	enum intel_pipe_crc_source source;
};

static const char *pipe_crc_sources[] = {
	"none",
	"plane1",
	"plane2",
	"pf",
	"pipe",
	"TV",
	"DP-B",
	"DP-C",
	"DP-D",
	"auto"
};

static const char *pipe_crc_source_name(enum intel_pipe_crc_source source)
{
        return pipe_crc_sources[source];
}

static bool igt_pipe_crc_do_start(igt_pipe_crc_t *pipe_crc)
{
	char buf[64];

	/* Stop first just to make sure we don't have lingering state left. */
	igt_pipe_crc_stop(pipe_crc);

	if (pipe_crc->is_legacy)
		sprintf(buf, "pipe %s %s", kmstest_pipe_name(pipe_crc->pipe),
			pipe_crc_source_name(pipe_crc->source));
	else
		sprintf(buf, "%s", pipe_crc_source_name(pipe_crc->source));

	errno = 0;
	igt_assert_eq(write(pipe_crc->ctl_fd, buf, strlen(buf)), strlen(buf));
	if (errno != 0)
		return false;

	if (!pipe_crc->is_legacy) {
		sprintf(buf, "crtc-%d/crc/data", pipe_crc->pipe);
		errno = 0;
		pipe_crc->crc_fd = igt_debugfs_open(buf, pipe_crc->flags);
		if (pipe_crc->crc_fd == -1 && errno == EINVAL)
			return false;
		igt_assert_eq(errno, 0);
	}

	return true;
}

static void igt_pipe_crc_pipe_off(int fd, enum pipe pipe)
{
	char buf[32];

	sprintf(buf, "pipe %s none", kmstest_pipe_name(pipe));
	igt_assert_eq(write(fd, buf, strlen(buf)), strlen(buf));
}

static void igt_pipe_crc_reset(void)
{
	igt_debugfs_t *debugfs = __igt_debugfs_singleton();
	int fd;
	struct dirent *dirent;
	char buf[128];
	const char *cmd = "none";
	bool done = false;
	DIR *dir;

	dir = opendir(debugfs->dri_path);
	if (dir) {
		while ((dirent = readdir(dir))) {
			if (strcmp(dirent->d_name, "crtc-") != 0)
				continue;

			sprintf(buf, "%s/%s/crc/control", debugfs->dri_path,
				dirent->d_name);
			fd = open(buf, O_WRONLY);
			if (fd == -1)
				continue;

			igt_assert_eq(write(fd, cmd, strlen(cmd)), strlen(cmd));
			done = true;

			close(fd);
		}
		closedir(dir);
	}

	if (done)
		return;

	fd = igt_debugfs_open("i915_display_crc_ctl", O_WRONLY);
	if (fd != -1) {
		igt_pipe_crc_pipe_off(fd, PIPE_A);
		igt_pipe_crc_pipe_off(fd, PIPE_B);
		igt_pipe_crc_pipe_off(fd, PIPE_C);

		close(fd);
	}
}

static void pipe_crc_exit_handler(int sig)
{
	igt_pipe_crc_reset();
}

/**
 * igt_require_pipe_crc:
 *
 * Convenience helper to check whether pipe CRC capturing is supported by the
 * kernel. Uses igt_skip to automatically skip the test/subtest if this isn't
 * the case.
 */
void igt_require_pipe_crc(void)
{
	const char *cmd = "pipe A none";
	FILE *ctl;
	size_t written;
	int ret;

	ctl = igt_debugfs_fopen("crtc-0/crc/control", "r+");
	if (!ctl) {
		ctl = igt_debugfs_fopen("i915_display_crc_ctl", "r+");
		igt_require_f(ctl,
			      "No display_crc_ctl found, kernel too old\n");
		written = fwrite(cmd, 1, strlen(cmd), ctl);
		ret = fflush(ctl);
		igt_require_f((written == strlen(cmd) && ret == 0) || errno != ENODEV,
			      "CRCs not supported on this platform\n");
	}

	fclose(ctl);
}

static igt_pipe_crc_t *
pipe_crc_new(enum pipe pipe, enum intel_pipe_crc_source source, int flags)
{
	igt_pipe_crc_t *pipe_crc;
	char buf[128];

	igt_install_exit_handler(pipe_crc_exit_handler);

	pipe_crc = calloc(1, sizeof(struct _igt_pipe_crc));

	sprintf(buf, "crtc-%d/crc/control", pipe);
	pipe_crc->ctl_fd = igt_debugfs_open(buf, O_WRONLY);
	if (pipe_crc->ctl_fd == -1) {
		pipe_crc->ctl_fd = igt_debugfs_open("i915_display_crc_ctl",
						    O_WRONLY);
		igt_assert(pipe_crc->ctl_fd != -1);
		pipe_crc->is_legacy = true;
	}

	if (pipe_crc->is_legacy) {
		sprintf(buf, "i915_pipe_%s_crc", kmstest_pipe_name(pipe));
		pipe_crc->crc_fd = igt_debugfs_open(buf, flags);
		igt_assert(pipe_crc->crc_fd != -1);
		igt_debug("Using legacy frame CRC ABI\n");
	} else {
		pipe_crc->crc_fd = -1;
		igt_debug("Using generic frame CRC ABI\n");
	}

	pipe_crc->pipe = pipe;
	pipe_crc->source = source;
	pipe_crc->flags = flags;

	return pipe_crc;
}

/**
 * igt_pipe_crc_new:
 * @pipe: display pipe to use as source
 * @source: CRC tap point to use as source
 *
 * This sets up a new pipe CRC capture object for the given @pipe and @source
 * in blocking mode.
 *
 * Returns: A pipe CRC object for the given @pipe and @source. The library
 * assumes that the source is always available since recent kernels support at
 * least INTEL_PIPE_CRC_SOURCE_AUTO everywhere.
 */
igt_pipe_crc_t *
igt_pipe_crc_new(enum pipe pipe, enum intel_pipe_crc_source source)
{
	return pipe_crc_new(pipe, source, O_RDONLY);
}

/**
 * igt_pipe_crc_new_nonblock:
 * @pipe: display pipe to use as source
 * @source: CRC tap point to use as source
 *
 * This sets up a new pipe CRC capture object for the given @pipe and @source
 * in nonblocking mode.
 *
 * Returns: A pipe CRC object for the given @pipe and @source. The library
 * assumes that the source is always available since recent kernels support at
 * least INTEL_PIPE_CRC_SOURCE_AUTO everywhere.
 */
igt_pipe_crc_t *
igt_pipe_crc_new_nonblock(enum pipe pipe, enum intel_pipe_crc_source source)
{
	return pipe_crc_new(pipe, source, O_RDONLY | O_NONBLOCK);
}

/**
 * igt_pipe_crc_free:
 * @pipe_crc: pipe CRC object
 *
 * Frees all resources associated with @pipe_crc.
 */
void igt_pipe_crc_free(igt_pipe_crc_t *pipe_crc)
{
	if (!pipe_crc)
		return;

	close(pipe_crc->ctl_fd);
	close(pipe_crc->crc_fd);
	free(pipe_crc);
}

static bool pipe_crc_init_from_string(igt_pipe_crc_t *pipe_crc, igt_crc_t *crc,
				      const char *line)
{
	int n, i;
	const char *buf;

	if (pipe_crc->is_legacy) {
		crc->has_valid_frame = true;
		crc->n_words = 5;
		n = sscanf(line, "%8u %8x %8x %8x %8x %8x", &crc->frame,
			   &crc->crc[0], &crc->crc[1], &crc->crc[2],
			   &crc->crc[3], &crc->crc[4]);
		return n == 6;
	}

	if (strncmp(line, "XXXXXXXXXX", 10) == 0)
		crc->has_valid_frame = false;
	else {
		crc->has_valid_frame = true;
		crc->frame = strtoul(line, NULL, 16);
	}

	buf = line + 10;
	for (i = 0; *buf != '\n'; i++, buf += 11)
		crc->crc[i] = strtoul(buf, NULL, 16);

	crc->n_words = i;

	return true;
}

static int read_crc(igt_pipe_crc_t *pipe_crc, igt_crc_t *out)
{
	ssize_t bytes_read;
	char buf[MAX_LINE_LEN + 1];
	size_t read_len;

	if (pipe_crc->is_legacy)
		read_len = LEGACY_LINE_LEN;
	else
		read_len = MAX_LINE_LEN;

	igt_set_timeout(5, "CRC reading");
	bytes_read = read(pipe_crc->crc_fd, &buf, read_len);
	igt_reset_timeout();

	if (bytes_read < 0 && errno == EAGAIN) {
		igt_assert(pipe_crc->flags & O_NONBLOCK);
		bytes_read = 0;
	}
	buf[bytes_read] = '\0';

	if (bytes_read && !pipe_crc_init_from_string(pipe_crc, out, buf))
		return -EINVAL;

	return bytes_read;
}

static void read_one_crc(igt_pipe_crc_t *pipe_crc, igt_crc_t *out)
{
	while (read_crc(pipe_crc, out) == 0)
		usleep(1000);
}

/**
 * igt_pipe_crc_start:
 * @pipe_crc: pipe CRC object
 *
 * Starts the CRC capture process on @pipe_crc.
 */
void igt_pipe_crc_start(igt_pipe_crc_t *pipe_crc)
{
	igt_crc_t crc;

	igt_assert(igt_pipe_crc_do_start(pipe_crc));

	if (pipe_crc->is_legacy) {
		/*
		 * For some no yet identified reason, the first CRC is
		 * bonkers. So let's just wait for the next vblank and read
		 * out the buggy result.
		 *
		 * On CHV sometimes the second CRC is bonkers as well, so
		 * don't trust that one either.
		 */
		read_one_crc(pipe_crc, &crc);
		read_one_crc(pipe_crc, &crc);
	}
}

/**
 * igt_pipe_crc_stop:
 * @pipe_crc: pipe CRC object
 *
 * Stops the CRC capture process on @pipe_crc.
 */
void igt_pipe_crc_stop(igt_pipe_crc_t *pipe_crc)
{
	char buf[32];

	if (pipe_crc->is_legacy) {
		sprintf(buf, "pipe %s none",
			kmstest_pipe_name(pipe_crc->pipe));
		igt_assert_eq(write(pipe_crc->ctl_fd, buf, strlen(buf)),
			      strlen(buf));
	} else {
		close(pipe_crc->crc_fd);
		pipe_crc->crc_fd = -1;
	}
}

/**
 * igt_pipe_crc_get_crcs:
 * @pipe_crc: pipe CRC object
 * @n_crcs: number of CRCs to capture
 * @out_crcs: buffer pointer for the captured CRC values
 *
 * Read up to @n_crcs from @pipe_crc. This function does not block, and will
 * return early if not enough CRCs can be captured, if @pipe_crc has been
 * opened using igt_pipe_crc_new_nonblock(). It will block until @n_crcs are
 * retrieved if @pipe_crc has been opened using igt_pipe_crc_new(). @out_crcs is
 * alloced by this function and must be released with free() by the caller.
 *
 * Callers must start and stop the capturing themselves by calling
 * igt_pipe_crc_start() and igt_pipe_crc_stop(). For one-shot CRC collecting
 * look at igt_pipe_crc_collect_crc().
 *
 * Returns:
 * The number of CRCs captured. Should be equal to @n_crcs in blocking mode, but
 * can be less (even zero) in non-blocking mode.
 */
int
igt_pipe_crc_get_crcs(igt_pipe_crc_t *pipe_crc, int n_crcs,
		      igt_crc_t **out_crcs)
{
	igt_crc_t *crcs;
	int n = 0;

	crcs = calloc(n_crcs, sizeof(igt_crc_t));

	do {
		igt_crc_t *crc = &crcs[n];
		int ret;

		ret = read_crc(pipe_crc, crc);
		if (ret < 0)
			continue;
		if (ret == 0)
			break;

		n++;
	} while (n < n_crcs);

	*out_crcs = crcs;
	return n;
}

static void crc_sanity_checks(igt_crc_t *crc)
{
	int i;
	bool all_zero = true;

	for (i = 0; i < crc->n_words; i++) {
		igt_warn_on_f(crc->crc[i] == 0xffffffff,
			      "Suspicious CRC: it looks like the CRC "
			      "read back was from a register in a powered "
			      "down well\n");
		if (crc->crc[i])
			all_zero = false;
	}

	igt_warn_on_f(all_zero, "Suspicious CRC: All values are 0.\n");
}

/**
 * igt_pipe_crc_collect_crc:
 * @pipe_crc: pipe CRC object
 * @out_crc: buffer for the captured CRC values
 *
 * Read a single CRC from @pipe_crc. This function blocks until the CRC is
 * retrieved, irrespective of whether @pipe_crc has been opened with
 * igt_pipe_crc_new() or igt_pipe_crc_new_nonblock().  @out_crc must be
 * allocated by the caller.
 *
 * This function takes care of the pipe_crc book-keeping, it will start/stop
 * the collection of the CRC.
 *
 * This function also calls the interactive debug with the "crc" domain, so you
 * can make use of this feature to actually see the screen that is being CRC'd.
 *
 * For continuous CRC collection look at igt_pipe_crc_start(),
 * igt_pipe_crc_get_crcs() and igt_pipe_crc_stop().
 */
void igt_pipe_crc_collect_crc(igt_pipe_crc_t *pipe_crc, igt_crc_t *out_crc)
{
	igt_debug_wait_for_keypress("crc");

	igt_pipe_crc_start(pipe_crc);
	read_one_crc(pipe_crc, out_crc);
	igt_pipe_crc_stop(pipe_crc);

	crc_sanity_checks(out_crc);
}

/*
 * Drop caches
 */

/**
 * igt_drop_caches_set:
 * @val: bitmask for DROP_* values
 *
 * This calls the debugfs interface the drm/i915 GEM driver exposes to drop or
 * evict certain classes of gem buffer objects.
 */
void igt_drop_caches_set(uint64_t val)
{
	int fd;
	char data[19];
	size_t nbytes;

	sprintf(data, "0x%" PRIx64, val);

	fd = igt_debugfs_open("i915_gem_drop_caches", O_WRONLY);

	igt_assert(fd >= 0);
	do {
		nbytes = write(fd, data, strlen(data) + 1);
	} while (nbytes == -1 && (errno == EINTR || errno == EAGAIN));
	igt_assert(nbytes == strlen(data) + 1);
	close(fd);
}

/*
 * Prefault control
 */

#define PREFAULT_DEBUGFS "/sys/module/i915/parameters/prefault_disable"
static void igt_prefault_control(bool enable)
{
	const char *name = PREFAULT_DEBUGFS;
	int fd;
	char buf[2] = {'Y', 'N'};
	int index;

	fd = open(name, O_RDWR);
	igt_require(fd >= 0);

	if (enable)
		index = 1;
	else
		index = 0;

	igt_require(write(fd, &buf[index], 1) == 1);

	close(fd);
}

static void enable_prefault_at_exit(int sig)
{
	igt_enable_prefault();
}

/**
 * igt_disable_prefault:
 *
 * Disable prefaulting in certain gem ioctls through the debugfs interface. As
 * usual this installs an exit handler to clean up and re-enable prefaulting
 * even when the test exited abnormally.
 *
 * igt_enable_prefault() will enable normale operation again.
 */
void igt_disable_prefault(void)
{
	igt_prefault_control(false);

	igt_install_exit_handler(enable_prefault_at_exit);
}

/**
 * igt_enable_prefault:
 *
 * Enable prefault (again) through the debugfs interface.
 */
void igt_enable_prefault(void)
{
	igt_prefault_control(true);
}

static int get_object_count(void)
{
	FILE *file;
	int ret, scanned;

	igt_drop_caches_set(DROP_RETIRE | DROP_ACTIVE | DROP_FREED);

	file = igt_debugfs_fopen("i915_gem_objects", "r");

	scanned = fscanf(file, "%i objects", &ret);
	igt_assert_eq(scanned, 1);

	return ret;
}

/**
 * igt_get_stable_obj_count:
 * @driver: fd to drm/i915 GEM driver
 *
 * This puts the driver into a stable (quiescent) state and then returns the
 * current number of gem buffer objects as reported in the i915_gem_objects
 * debugFS interface.
 */
int igt_get_stable_obj_count(int driver)
{
	int obj_count;
	gem_quiescent_gpu(driver);
	obj_count = get_object_count();
	/* The test relies on the system being in the same state before and
	 * after the test so any difference in the object count is a result of
	 * leaks during the test. gem_quiescent_gpu() mostly achieves this but
	 * on android occasionally obj_count can still change briefly.
	 * The loop ensures obj_count has remained stable over several checks
	 */
#ifdef ANDROID
	{
		int loop_count = 0;
		int prev_obj_count = obj_count;
		while (loop_count < 4) {
			usleep(200000);
			gem_quiescent_gpu(driver);
			obj_count = get_object_count();
			if (obj_count == prev_obj_count) {
				loop_count++;
			} else {
				igt_debug("loop_count=%d, obj_count=%d, prev_obj_count=%d\n",
					loop_count, obj_count, prev_obj_count);
				loop_count = 0;
				prev_obj_count = obj_count;
			}

		}
	}
#endif
	return obj_count;
}


/* Non-i915 specific debugfs API */

/**
 * igt_debugfs_dir:
 * @device: fd of the device (or -1 to default to Intel)
 *
 * This opens the debugfs directory corresponding to device for use
 * with igt_sysfs_get() and related functions.
 *
 * Returns:
 * The directory fd, or -1 on failure.
 */
int igt_debugfs_dir(int device)
{
	struct stat st;
	char path[256];

	if (fstat(device, &st) || !S_ISCHR(st.st_mode))
		return -1;

	sprintf(path, "%s/dri/%d",
		igt_debugfs_mount(), (int)(st.st_rdev & 0xff));
	igt_debug("Opening debugfs dir %s\n", path);
	return open(path, O_RDONLY);
}

void igt_debugfs_dump(int device, const char *filename)
{
	char *contents;
	int dir;

	dir = igt_debugfs_dir(device);
	contents = igt_sysfs_get(dir, filename);
	close(dir);

	igt_debug("%s:\n%s\n", filename, contents);
	free(contents);
}
