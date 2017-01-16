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
 * Authors:
 * 	Daniel Vetter <daniel.vetter@ffwll.ch>
 * 	Damien Lespiau <damien.lespiau@intel.com>
 */

#include "config.h"
#include <unistd.h>
#include <stdio.h>
#include <stdarg.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <string.h>
#include <stdlib.h>
#ifdef HAVE_LINUX_KD_H
#include <linux/kd.h>
#elif HAVE_SYS_KD_H
#include <sys/kd.h>
#endif
#include <errno.h>
#include <time.h>

#ifdef __FreeBSD__
#include <sys/consio.h>
#endif

#include <i915_drm.h>

#include "drmtest.h"
#include "igt_kms.h"
#include "igt_aux.h"
#include "intel_chipset.h"
#include "igt_debugfs.h"
#include "igt_sysfs.h"

/**
 * SECTION:igt_kms
 * @short_description: Kernel modesetting support library
 * @title: KMS
 * @include: igt.h
 *
 * This library provides support to enumerate and set modeset configurations.
 *
 * There are two parts in this library: First the low level helper function
 * which directly build on top of raw ioctls or the interfaces provided by
 * libdrm. Those functions all have a kmstest_ prefix.
 *
 * The second part is a high-level library to manage modeset configurations
 * which abstracts away some of the low-level details like the difference
 * between legacy and universal plane support for setting cursors or in the
 * future the difference between legacy and atomic commit. These high-level
 * functions have all igt_ prefixes. This part is still very much work in
 * progress and so also lacks a bit documentation for the individual functions.
 *
 * Note that this library's header pulls in the [i-g-t framebuffer](intel-gpu-tools-i-g-t-framebuffer.html)
 * library as a dependency.
 */

/* list of connectors that need resetting on exit */
#define MAX_CONNECTORS 32
static char *forced_connectors[MAX_CONNECTORS + 1];
static int forced_connectors_device[MAX_CONNECTORS + 1];

static void update_edid_csum(unsigned char *edid)
{
	int i, sum = 0;
	struct tm *tm;
	time_t t;

	/* year of manufacture */
	t = time(NULL);
	tm = localtime(&t);
	edid[17] = tm->tm_year - 90;

	/* calculate checksum */
	for (i = 0; i < 127; i++) {
		sum = sum + edid[i];
	}
	edid[127] = 256 - sum;
}

#define VFREQ 60
#define CLOCK 148500
#define HACTIVE 1920
#define HBLANK 280
#define VACTIVE 1080
#define VBLANK 45
#define HOFFSET 88
#define HPULSE 44
#define VOFFSET 4
#define VPULSE 5

#define HSIZE 52
#define VSIZE 30

#define EDID_NAME base_edid
#include "igt_edid_template.h"

/**
 * igt_kms_get_base_edid:
 *
 * Get the base edid block, which includes the following modes:
 *
 *  - 1920x1080 60Hz
 *  - 1280x720 60Hz
 *  - 1024x768 60Hz
 *  - 800x600 60Hz
 *  - 640x480 60Hz
 *
 * This can be extended with further features using functions such as
 * #kmstest_edid_add_3d.
 *
 * Returns: a basic edid block
 */
const unsigned char* igt_kms_get_base_edid(void)
{
	update_edid_csum(base_edid);

	return base_edid;
}

#define VFREQ 60
#define CLOCK 101000
#define HACTIVE 1400
#define HBLANK 160
#define VACTIVE 1050
#define VBLANK 30
#define HOFFSET 48
#define HPULSE 32
#define VOFFSET 3
#define VPULSE 4

#define HSIZE 52
#define VSIZE 30

#define EDID_NAME alt_edid
#include "igt_edid_template.h"

/**
 * igt_kms_get_alt_edid:
 *
 * Get an alternate edid block, which includes the following modes:
 *
 *  - 1400x1050 60Hz
 *  - 1920x1080 60Hz
 *  - 1280x720 60Hz
 *  - 1024x768 60Hz
 *  - 800x600 60Hz
 *  - 640x480 60Hz
 *
 * This can be extended with further features using functions such as
 * #kmstest_edid_add_3d.
 *
 * Returns: an alternate edid block
 */
static const char *igt_plane_prop_names[IGT_NUM_PLANE_PROPS] = {
	"SRC_X",
	"SRC_Y",
	"SRC_W",
	"SRC_H",
	"CRTC_X",
	"CRTC_Y",
	"CRTC_W",
	"CRTC_H",
	"FB_ID",
	"CRTC_ID",
	"type",
	"rotation"
};

static const char *igt_crtc_prop_names[IGT_NUM_CRTC_PROPS] = {
	"background_color",
	"CTM",
	"DEGAMMA_LUT",
	"GAMMA_LUT",
	"MODE_ID",
	"ACTIVE"
};

static const char *igt_connector_prop_names[IGT_NUM_CONNECTOR_PROPS] = {
	"scaling mode",
	"CRTC_ID"
};

/*
 * Retrieve all the properies specified in props_name and store them into
 * plane->atomic_props_plane.
 */
static void
igt_atomic_fill_plane_props(igt_display_t *display, igt_plane_t *plane,
			int num_props, const char **prop_names)
{
	drmModeObjectPropertiesPtr props;
	int i, j, fd;

	fd = display->drm_fd;

	props = drmModeObjectGetProperties(fd, plane->drm_plane->plane_id, DRM_MODE_OBJECT_PLANE);
	igt_assert(props);

	for (i = 0; i < props->count_props; i++) {
		drmModePropertyPtr prop =
			drmModeGetProperty(fd, props->props[i]);

		for (j = 0; j < num_props; j++) {
			if (strcmp(prop->name, prop_names[j]) != 0)
				continue;

			plane->atomic_props_plane[j] = props->props[i];
			break;
		}

		drmModeFreeProperty(prop);
	}

	drmModeFreeObjectProperties(props);
}

/*
 * Retrieve all the properies specified in props_name and store them into
 * config->atomic_props_crtc and config->atomic_props_connector.
 */
static void
igt_atomic_fill_connector_props(igt_display_t *display, igt_output_t *output,
			int num_connector_props, const char **conn_prop_names)
{
	drmModeObjectPropertiesPtr props;
	int i, j, fd;

	fd = display->drm_fd;

	props = drmModeObjectGetProperties(fd, output->config.connector->connector_id, DRM_MODE_OBJECT_CONNECTOR);
	igt_assert(props);

	for (i = 0; i < props->count_props; i++) {
		drmModePropertyPtr prop =
			drmModeGetProperty(fd, props->props[i]);

		for (j = 0; j < num_connector_props; j++) {
			if (strcmp(prop->name, conn_prop_names[j]) != 0)
				continue;

			output->config.atomic_props_connector[j] = props->props[i];
			break;
		}

		drmModeFreeProperty(prop);
	}

	drmModeFreeObjectProperties(props);
}

static void
igt_atomic_fill_pipe_props(igt_display_t *display, igt_pipe_t *pipe,
			int num_crtc_props, const char **crtc_prop_names)
{
	drmModeObjectPropertiesPtr props;
	int i, j, fd;

	fd = display->drm_fd;

	props = drmModeObjectGetProperties(fd, pipe->crtc_id, DRM_MODE_OBJECT_CRTC);
	igt_assert(props);

	for (i = 0; i < props->count_props; i++) {
		drmModePropertyPtr prop =
			drmModeGetProperty(fd, props->props[i]);

		for (j = 0; j < num_crtc_props; j++) {
			if (strcmp(prop->name, crtc_prop_names[j]) != 0)
				continue;

			pipe->atomic_props_crtc[j] = props->props[i];
			break;
		}

		drmModeFreeProperty(prop);
	}

	drmModeFreeObjectProperties(props);
}

const unsigned char* igt_kms_get_alt_edid(void)
{
	update_edid_csum(alt_edid);

	return alt_edid;
}

/**
 * kmstest_pipe_name:
 * @pipe: display pipe
 *
 * Returns: String represnting @pipe, e.g. "A".
 */
const char *kmstest_pipe_name(enum pipe pipe)
{
	const char *str[] = { "A", "B", "C" };

	if (pipe == PIPE_NONE)
		return "None";

	if (pipe > 2)
		return "invalid";

	return str[pipe];
}

/**
 * kmstest_pipe_to_index:
 *@pipe: display pipe in string format
 *
 * Returns: index to corresponding pipe
 */
int kmstest_pipe_to_index(char pipe)
{
	if (pipe == 'A')
		return 0;
	else if (pipe == 'B')
		return 1;
	else if (pipe == 'C')
		return 2;
	else
		return -EINVAL;
}

/**
 * kmstest_plane_name:
 * @plane: display plane
 *
 * Returns: String represnting @pipe, e.g. "plane1".
 */
const char *kmstest_plane_name(enum igt_plane plane)
{
	static const char *names[] = {
		[IGT_PLANE_1] = "plane1",
		[IGT_PLANE_2] = "plane2",
		[IGT_PLANE_3] = "plane3",
		[IGT_PLANE_4] = "plane4",
		[IGT_PLANE_5] = "plane5",
		[IGT_PLANE_6] = "plane6",
		[IGT_PLANE_7] = "plane7",
		[IGT_PLANE_8] = "plane8",
		[IGT_PLANE_9] = "plane9",
		[IGT_PLANE_CURSOR] = "cursor",
	};

	igt_assert(plane < ARRAY_SIZE(names) && names[plane]);

	return names[plane];
}

static const char *mode_stereo_name(const drmModeModeInfo *mode)
{
	switch (mode->flags & DRM_MODE_FLAG_3D_MASK) {
	case DRM_MODE_FLAG_3D_FRAME_PACKING:
		return "FP";
	case DRM_MODE_FLAG_3D_FIELD_ALTERNATIVE:
		return "FA";
	case DRM_MODE_FLAG_3D_LINE_ALTERNATIVE:
		return "LA";
	case DRM_MODE_FLAG_3D_SIDE_BY_SIDE_FULL:
		return "SBSF";
	case DRM_MODE_FLAG_3D_L_DEPTH:
		return "LD";
	case DRM_MODE_FLAG_3D_L_DEPTH_GFX_GFX_DEPTH:
		return "LDGFX";
	case DRM_MODE_FLAG_3D_TOP_AND_BOTTOM:
		return "TB";
	case DRM_MODE_FLAG_3D_SIDE_BY_SIDE_HALF:
		return "SBSH";
	default:
		return NULL;
	}
}

/**
 * kmstest_dump_mode:
 * @mode: libdrm mode structure
 *
 * Prints @mode to stdout in a human-readable form.
 */
void kmstest_dump_mode(drmModeModeInfo *mode)
{
	const char *stereo = mode_stereo_name(mode);

	igt_info("  %s %d %d %d %d %d %d %d %d %d 0x%x 0x%x %d%s%s%s\n",
		 mode->name, mode->vrefresh,
		 mode->hdisplay, mode->hsync_start,
		 mode->hsync_end, mode->htotal,
		 mode->vdisplay, mode->vsync_start,
		 mode->vsync_end, mode->vtotal,
		 mode->flags, mode->type, mode->clock,
		 stereo ? " (3D:" : "",
		 stereo ? stereo : "", stereo ? ")" : "");
}

/**
 * kmstest_get_pipe_from_crtc_id:
 * @fd: DRM fd
 * @crtc_id: DRM CRTC id
 *
 * Returns: The crtc index for the given DRM CRTC ID @crtc_id. The crtc index
 * is the equivalent of the pipe id.  This value maps directly to an enum pipe
 * value used in other helper functions.  Returns 0 if the index could not be
 * determined.
 */

int kmstest_get_pipe_from_crtc_id(int fd, int crtc_id)
{
	drmModeRes *res;
	drmModeCrtc *crtc;
	int i, cur_id;

	res = drmModeGetResources(fd);
	igt_assert(res);

	for (i = 0; i < res->count_crtcs; i++) {
		crtc = drmModeGetCrtc(fd, res->crtcs[i]);
		igt_assert(crtc);
		cur_id = crtc->crtc_id;
		drmModeFreeCrtc(crtc);
		if (cur_id == crtc_id)
			break;
	}

	igt_assert(i < res->count_crtcs);

	drmModeFreeResources(res);

	return i;
}

/**
 * kmstest_find_crtc_for_connector:
 * @fd: DRM fd
 * @res: libdrm resources pointer
 * @connector: libdrm connector pointer
 * @crtc_blacklist_idx_mask: a mask of CRTC indexes that we can't return
 *
 * Returns: the CRTC ID for a CRTC that fits the connector, otherwise it asserts
 * false and never returns. The blacklist mask can be used in case you have
 * CRTCs that are already in use by other connectors.
 */
uint32_t kmstest_find_crtc_for_connector(int fd, drmModeRes *res,
					 drmModeConnector *connector,
					 uint32_t crtc_blacklist_idx_mask)
{
	drmModeEncoder *e;
	uint32_t possible_crtcs;
	int i, j;

	for (i = 0; i < connector->count_encoders; i++) {
		e = drmModeGetEncoder(fd, connector->encoders[i]);
		possible_crtcs = e->possible_crtcs & ~crtc_blacklist_idx_mask;
		drmModeFreeEncoder(e);

		for (j = 0; possible_crtcs >> j; j++)
			if (possible_crtcs & (1 << j))
				return res->crtcs[j];
	}

	igt_assert(false);
}

/**
 * kmstest_dumb_create:
 * @fd: open drm file descriptor
 * @width: width of the buffer in pixels
 * @height: height of the buffer in pixels
 * @bpp: bytes per pixel of the buffer
 * @stride: Pointer which receives the dumb bo's stride, can be NULL.
 * @size: Pointer which receives the dumb bo's size, can be NULL.
 *
 * This wraps the CREATE_DUMB ioctl, which allocates a new dumb buffer object
 * for the specified dimensions.
 *
 * Returns: The file-private handle of the created buffer object
 */
uint32_t kmstest_dumb_create(int fd, int width, int height, int bpp,
			     unsigned *stride, unsigned *size)
{
	struct drm_mode_create_dumb create;

	memset(&create, 0, sizeof(create));
	create.width = width;
	create.height = height;
	create.bpp = bpp;

	create.handle = 0;
	do_ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &create);
	igt_assert(create.handle);
	igt_assert(create.size >= width * height * bpp / 8);

	if (stride)
		*stride = create.pitch;

	if (size)
		*size = create.size;

	return create.handle;
}

void *kmstest_dumb_map_buffer(int fd, uint32_t handle, uint64_t size,
			      unsigned prot)
{
	struct drm_mode_map_dumb arg = {};
	void *ptr;

	arg.handle = handle;

	do_ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &arg);

	ptr = mmap(NULL, size, prot, MAP_SHARED, fd, arg.offset);
	igt_assert(ptr != MAP_FAILED);

	return ptr;
}

/*
 * Returns: the previous mode, or KD_GRAPHICS if no /dev/tty0 was
 * found and nothing was done.
 */
static signed long set_vt_mode(unsigned long mode)
{
	int fd;
	unsigned long prev_mode;
	static const char TTY0[] = "/dev/tty0";

	if (access(TTY0, F_OK)) {
		/* errno message should be "No such file". Do not
		   hardcode but ask strerror() in the very unlikely
		   case something else happened. */
		igt_debug("VT: %s: %s, cannot change its mode\n",
			  TTY0, strerror(errno));
		return KD_GRAPHICS;
	}

	fd = open(TTY0, O_RDONLY);
	if (fd < 0)
		return -errno;

	prev_mode = 0;
	if (drmIoctl(fd, KDGETMODE, &prev_mode))
		goto err;
	if (drmIoctl(fd, KDSETMODE, (void *)mode))
		goto err;

	close(fd);

	return prev_mode;
err:
	close(fd);

	return -errno;
}

static unsigned long orig_vt_mode = -1UL;

/**
 * kmstest_restore_vt_mode:
 *
 * Restore the VT mode in use before #kmstest_set_vt_graphics_mode was called.
 */
void kmstest_restore_vt_mode(void)
{
	long ret;

	if (orig_vt_mode != -1UL) {
		ret = set_vt_mode(orig_vt_mode);

		igt_assert(ret >= 0);
		igt_debug("VT: original mode 0x%lx restored\n", orig_vt_mode);
		orig_vt_mode = -1UL;
	}
}

/**
 * kmstest_set_vt_graphics_mode:
 *
 * Sets the controlling VT (if available) into graphics/raw mode and installs
 * an igt exit handler to set the VT back to text mode on exit. Use
 * #kmstest_restore_vt_mode to restore the previous VT mode manually.
 *
 * All kms tests must call this function to make sure that the fbcon doesn't
 * interfere by e.g. blanking the screen.
 */
void kmstest_set_vt_graphics_mode(void)
{
	long ret;

	igt_install_exit_handler((igt_exit_handler_t) kmstest_restore_vt_mode);

	ret = set_vt_mode(KD_GRAPHICS);

	igt_assert(ret >= 0);
	orig_vt_mode = ret;

	igt_debug("VT: graphics mode set (mode was 0x%lx)\n", ret);
}


static void reset_connectors_at_exit(int sig)
{
	igt_reset_connectors();
}

/**
 * kmstest_force_connector:
 * @fd: drm file descriptor
 * @connector: connector
 * @state: state to force on @connector
 *
 * Force the specified state on the specified connector.
 *
 * Returns: true on success
 */
bool kmstest_force_connector(int drm_fd, drmModeConnector *connector,
			     enum kmstest_force_connector_state state)
{
	char *path, **tmp;
	const char *value;
	drmModeConnector *temp;
	uint32_t devid;
	int len, dir, idx;

	if (is_i915_device(drm_fd)) {
		devid = intel_get_drm_devid(drm_fd);

		/*
		 * forcing hdmi or dp connectors on HSW and BDW doesn't
		 * currently work, so fail early to allow the test to skip if
		 * required
		 */
		if ((connector->connector_type == DRM_MODE_CONNECTOR_HDMIA ||
		     connector->connector_type == DRM_MODE_CONNECTOR_HDMIB ||
		     connector->connector_type == DRM_MODE_CONNECTOR_DisplayPort)
		    && (IS_HASWELL(devid) || IS_BROADWELL(devid)))
			return false;
	}

	switch (state) {
	case FORCE_CONNECTOR_ON:
		value = "on";
		break;
	case FORCE_CONNECTOR_DIGITAL:
		value = "on-digital";
		break;
	case FORCE_CONNECTOR_OFF:
		value = "off";
		break;

	default:
	case FORCE_CONNECTOR_UNSPECIFIED:
		value = "detect";
		break;
	}

	dir = igt_sysfs_open(drm_fd, &idx);
	if (dir < 0)
		return false;

	if (asprintf(&path, "card%d-%s-%d/status",
		     idx,
		     kmstest_connector_type_str(connector->connector_type),
		     connector->connector_type_id) < 0) {
		close(dir);
		return false;
	}

	if (!igt_sysfs_set(dir, path, value)) {
		close(dir);
		return false;
	}

	for (len = 0, tmp = forced_connectors; *tmp; tmp++) {
		/* check the connector is not already present */
		if (strcmp(*tmp, path) == 0) {
			len = -1;
			break;
		}
		len++;
	}

	if (len != -1 && len < MAX_CONNECTORS) {
		forced_connectors[len] = path;
		forced_connectors_device[len] = dir;
	}

	if (len >= MAX_CONNECTORS)
		igt_warn("Connector limit reached, %s will not be reset\n",
			 path);

	igt_debug("Connector %s is now forced %s\n", path, value);
	igt_debug("Current forced connectors:\n");
	tmp = forced_connectors;
	while (*tmp) {
		igt_debug("\t%s\n", *tmp);
		tmp++;
	}

	igt_install_exit_handler(reset_connectors_at_exit);

	/* To allow callers to always use GetConnectorCurrent we need to force a
	 * redetection here. */
	temp = drmModeGetConnector(drm_fd, connector->connector_id);
	drmModeFreeConnector(temp);

	return true;
}

/**
 * kmstest_force_edid:
 * @drm_fd: drm file descriptor
 * @connector: connector to set @edid on
 * @edid: An EDID data block
 * @length: length of the EDID data. #EDID_LENGTH defines the standard EDID
 * length
 *
 * Set the EDID data on @connector to @edid. See also #igt_kms_get_base_edid.
 *
 * If @length is zero, the forced EDID will be removed.
 */
void kmstest_force_edid(int drm_fd, drmModeConnector *connector,
			const unsigned char *edid, size_t length)
{
	char *path;
	int debugfs_fd, ret;
	drmModeConnector *temp;

	igt_assert_neq(asprintf(&path, "%s-%d/edid_override", kmstest_connector_type_str(connector->connector_type), connector->connector_type_id),
		       -1);
	debugfs_fd = igt_debugfs_open(path, O_WRONLY | O_TRUNC);
	free(path);

	igt_assert(debugfs_fd != -1);

	if (length == 0)
		ret = write(debugfs_fd, "reset", 5);
	else
		ret = write(debugfs_fd, edid, length);
	close(debugfs_fd);

	/* To allow callers to always use GetConnectorCurrent we need to force a
	 * redetection here. */
	temp = drmModeGetConnector(drm_fd, connector->connector_id);
	drmModeFreeConnector(temp);

	igt_assert(ret != -1);
}

/**
 * kmstest_get_connector_default_mode:
 * @drm_fd: DRM fd
 * @connector: libdrm connector
 * @mode: libdrm mode
 *
 * Retrieves the default mode for @connector and stores it in @mode.
 *
 * Returns: true on success, false on failure
 */
bool kmstest_get_connector_default_mode(int drm_fd, drmModeConnector *connector,
					drmModeModeInfo *mode)
{
	int i;

	if (!connector->count_modes) {
		igt_warn("no modes for connector %d\n",
			 connector->connector_id);
		return false;
	}

	for (i = 0; i < connector->count_modes; i++) {
		if (i == 0 ||
		    connector->modes[i].type & DRM_MODE_TYPE_PREFERRED) {
			*mode = connector->modes[i];
			if (mode->type & DRM_MODE_TYPE_PREFERRED)
				break;
		}
	}

	return true;
}

static void
_kmstest_connector_config_crtc_mask(int drm_fd,
				    drmModeConnector *connector,
				    struct kmstest_connector_config *config)
{
	int i;

	config->valid_crtc_idx_mask = 0;

	/* Now get a compatible encoder */
	for (i = 0; i < connector->count_encoders; i++) {
		drmModeEncoder *encoder = drmModeGetEncoder(drm_fd,
							    connector->encoders[i]);

		if (!encoder) {
			igt_warn("could not get encoder %d: %s\n",
				 connector->encoders[i],
				 strerror(errno));

			continue;
		}

		config->valid_crtc_idx_mask |= encoder->possible_crtcs;
		drmModeFreeEncoder(encoder);
	}
}

static drmModeEncoder *
_kmstest_connector_config_find_encoder(int drm_fd, drmModeConnector *connector, enum pipe pipe)
{
	int i;

	for (i = 0; i < connector->count_encoders; i++) {
		drmModeEncoder *encoder = drmModeGetEncoder(drm_fd, connector->encoders[i]);

		if (!encoder) {
			igt_warn("could not get encoder %d: %s\n",
				 connector->encoders[i],
				 strerror(errno));

			continue;
		}

		if (encoder->possible_crtcs & (1 << pipe))
			return encoder;

		drmModeFreeEncoder(encoder);
	}

	igt_assert(false);
	return NULL;
}

/**
 * _kmstest_connector_config:
 * @drm_fd: DRM fd
 * @connector_id: DRM connector id
 * @crtc_idx_mask: mask of allowed DRM CRTC indices
 * @config: structure filled with the possible configuration
 * @probe: whether to fully re-probe mode list or not
 *
 * This tries to find a suitable configuration for the given connector and CRTC
 * constraint and fills it into @config.
 */
static bool _kmstest_connector_config(int drm_fd, uint32_t connector_id,
				      unsigned long crtc_idx_mask,
				      struct kmstest_connector_config *config,
				      bool probe)
{
	drmModeRes *resources;
	drmModeConnector *connector;

	config->pipe = PIPE_NONE;

	resources = drmModeGetResources(drm_fd);
	if (!resources) {
		igt_warn("drmModeGetResources failed");
		goto err1;
	}

	/* First, find the connector & mode */
	if (probe)
		connector = drmModeGetConnector(drm_fd, connector_id);
	else
		connector = drmModeGetConnectorCurrent(drm_fd, connector_id);

	if (!connector)
		goto err2;

	if (connector->connector_id != connector_id) {
		igt_warn("connector id doesn't match (%d != %d)\n",
			 connector->connector_id, connector_id);
		goto err3;
	}

	/*
	 * Find given CRTC if crtc_id != 0 or else the first CRTC not in use.
	 * In both cases find the first compatible encoder and skip the CRTC
	 * if there is non such.
	 */
	_kmstest_connector_config_crtc_mask(drm_fd, connector, config);

	if (!connector->count_modes)
		memset(&config->default_mode, 0, sizeof(config->default_mode));
	else if (!kmstest_get_connector_default_mode(drm_fd, connector,
						     &config->default_mode))
		goto err3;

	config->connector = connector;

	crtc_idx_mask &= config->valid_crtc_idx_mask;
	if (!crtc_idx_mask)
		/* Keep config->connector */
		goto err2;

	config->pipe = ffs(crtc_idx_mask) - 1;

	config->encoder = _kmstest_connector_config_find_encoder(drm_fd, connector, config->pipe);
	config->crtc = drmModeGetCrtc(drm_fd, resources->crtcs[config->pipe]);

	if (connector->connection != DRM_MODE_CONNECTED)
		goto err2;

	if (!connector->count_modes) {
		igt_warn("connector %d/%s-%d has no modes\n", connector_id,
			 kmstest_connector_type_str(connector->connector_type),
			 connector->connector_type_id);
		goto err2;
	}

	drmModeFreeResources(resources);
	return true;
err3:
	drmModeFreeConnector(connector);
err2:
	drmModeFreeResources(resources);
err1:
	return false;
}

/**
 * kmstest_get_connector_config:
 * @drm_fd: DRM fd
 * @connector_id: DRM connector id
 * @crtc_idx_mask: mask of allowed DRM CRTC indices
 * @config: structure filled with the possible configuration
 *
 * This tries to find a suitable configuration for the given connector and CRTC
 * constraint and fills it into @config.
 */
bool kmstest_get_connector_config(int drm_fd, uint32_t connector_id,
				  unsigned long crtc_idx_mask,
				  struct kmstest_connector_config *config)
{
	return _kmstest_connector_config(drm_fd, connector_id, crtc_idx_mask,
					 config, 0);
}

/**
 * kmstest_probe_connector_config:
 * @drm_fd: DRM fd
 * @connector_id: DRM connector id
 * @crtc_idx_mask: mask of allowed DRM CRTC indices
 * @config: structure filled with the possible configuration
 *
 * This tries to find a suitable configuration for the given connector and CRTC
 * constraint and fills it into @config, fully probing the connector in the
 * process.
 */
bool kmstest_probe_connector_config(int drm_fd, uint32_t connector_id,
				    unsigned long crtc_idx_mask,
				    struct kmstest_connector_config *config)
{
	return _kmstest_connector_config(drm_fd, connector_id, crtc_idx_mask,
					 config, 1);
}

/**
 * kmstest_free_connector_config:
 * @config: connector configuration structure
 *
 * Free any resources in @config allocated in kmstest_get_connector_config().
 */
void kmstest_free_connector_config(struct kmstest_connector_config *config)
{
	drmModeFreeCrtc(config->crtc);
	config->crtc = NULL;

	drmModeFreeEncoder(config->encoder);
	config->encoder = NULL;

	drmModeFreeConnector(config->connector);
	config->connector = NULL;
}

/**
 * kmstest_set_connector_dpms:
 * @fd: DRM fd
 * @connector: libdrm connector
 * @mode: DRM DPMS value
 *
 * This function sets the DPMS setting of @connector to @mode.
 */
void kmstest_set_connector_dpms(int fd, drmModeConnector *connector, int mode)
{
	int i, dpms = 0;
	bool found_it = false;

	for (i = 0; i < connector->count_props; i++) {
		struct drm_mode_get_property prop;

		prop.prop_id = connector->props[i];
		prop.count_values = 0;
		prop.count_enum_blobs = 0;
		if (drmIoctl(fd, DRM_IOCTL_MODE_GETPROPERTY, &prop))
			continue;

		if (strcmp(prop.name, "DPMS"))
			continue;

		dpms = prop.prop_id;
		found_it = true;
		break;
	}
	igt_assert_f(found_it, "DPMS property not found on %d\n",
		     connector->connector_id);

	igt_assert(drmModeConnectorSetProperty(fd, connector->connector_id,
					       dpms, mode) == 0);
}

/**
 * kmstest_get_property:
 * @drm_fd: drm file descriptor
 * @object_id: object whose properties we're going to get
 * @object_type: type of obj_id (DRM_MODE_OBJECT_*)
 * @name: name of the property we're going to get
 * @prop_id: if not NULL, returns the property id
 * @value: if not NULL, returns the property value
 * @prop: if not NULL, returns the property, and the caller will have to free
 *        it manually.
 *
 * Finds a property with the given name on the given object.
 *
 * Returns: true in case we found something.
 */
bool
kmstest_get_property(int drm_fd, uint32_t object_id, uint32_t object_type,
		     const char *name, uint32_t *prop_id /* out */,
		     uint64_t *value /* out */,
		     drmModePropertyPtr *prop /* out */)
{
	drmModeObjectPropertiesPtr proplist;
	drmModePropertyPtr _prop;
	bool found = false;
	int i;

	proplist = drmModeObjectGetProperties(drm_fd, object_id, object_type);
	for (i = 0; i < proplist->count_props; i++) {
		_prop = drmModeGetProperty(drm_fd, proplist->props[i]);
		if (!_prop)
			continue;

		if (strcmp(_prop->name, name) == 0) {
			found = true;
			if (prop_id)
				*prop_id = proplist->props[i];
			if (value)
				*value = proplist->prop_values[i];
			if (prop)
				*prop = _prop;
			else
				drmModeFreeProperty(_prop);

			break;
		}
		drmModeFreeProperty(_prop);
	}

	drmModeFreeObjectProperties(proplist);
	return found;
}

/**
 * kmstest_edid_add_3d:
 * @edid: an existing valid edid block
 * @length: length of @edid
 * @new_edid_ptr: pointer to where the new edid will be placed
 * @new_length: pointer to the size of the new edid
 *
 * Makes a copy of an existing edid block and adds an extension indicating
 * stereo 3D capabilities.
 */
void kmstest_edid_add_3d(const unsigned char *edid, size_t length,
			 unsigned char *new_edid_ptr[], size_t *new_length)
{
	unsigned char *new_edid;
	int n_extensions;
	char sum = 0;
	int pos;
	int i;
	char cea_header_len = 4, video_block_len = 6, vsdb_block_len = 11;

	igt_assert(new_edid_ptr != NULL && new_length != NULL);

	*new_length = length + 128;

	new_edid = calloc(*new_length, sizeof(char));
	memcpy(new_edid, edid, length);
	*new_edid_ptr = new_edid;

	n_extensions = new_edid[126];
	n_extensions++;
	new_edid[126] = n_extensions;

	/* recompute checksum */
	for (i = 0; i < 127; i++) {
		sum = sum + new_edid[i];
	}
	new_edid[127] = 256 - sum;

	/* add a cea-861 extension block */
	pos = length;
	new_edid[pos++] = 0x2;
	new_edid[pos++] = 0x3;
	new_edid[pos++] = cea_header_len + video_block_len + vsdb_block_len;
	new_edid[pos++] = 0x0;

	/* video block (id | length) */
	new_edid[pos++] = 2 << 5 | (video_block_len - 1);
	new_edid[pos++] = 32 | 0x80; /* 1080p @ 24Hz | (native)*/
	new_edid[pos++] = 5;         /* 1080i @ 60Hz */
	new_edid[pos++] = 20;        /* 1080i @ 50Hz */
	new_edid[pos++] = 4;         /* 720p @ 60Hz*/
	new_edid[pos++] = 19;        /* 720p @ 50Hz*/

	/* vsdb block ( id | length ) */
	new_edid[pos++] = 3 << 5 | (vsdb_block_len - 1);
	/* registration id */
	new_edid[pos++] = 0x3;
	new_edid[pos++] = 0xc;
	new_edid[pos++] = 0x0;
	/* source physical address */
	new_edid[pos++] = 0x10;
	new_edid[pos++] = 0x00;
	/* Supports_AI ... etc */
	new_edid[pos++] = 0x00;
	/* Max TMDS Clock */
	new_edid[pos++] = 0x00;
	/* Latency present, HDMI Video Present */
	new_edid[pos++] = 0x20;
	/* HDMI Video */
	new_edid[pos++] = 0x80;
	new_edid[pos++] = 0x00;

	/* checksum */
	sum = 0;
	for (i = 0; i < 127; i++) {
		sum = sum + new_edid[length + i];
	}
	new_edid[length + 127] = 256 - sum;
}

/**
 * kmstest_unset_all_crtcs:
 * @drm_fd: the DRM fd
 * @resources: libdrm resources pointer
 *
 * Disables all the screens.
 */
void kmstest_unset_all_crtcs(int drm_fd, drmModeResPtr resources)
{
	int i, rc;

	for (i = 0; i < resources->count_crtcs; i++) {
		rc = drmModeSetCrtc(drm_fd, resources->crtcs[i], 0, 0, 0, NULL,
				    0, NULL);
		igt_assert(rc == 0);
	}
}

/**
 * kmstest_get_crtc_idx:
 * @res: the libdrm resources
 * @crtc_id: the CRTC id
 *
 * Get the CRTC index based on its ID. This is useful since a few places of
 * libdrm deal with CRTC masks.
 */
int kmstest_get_crtc_idx(drmModeRes *res, uint32_t crtc_id)
{
	int i;

	for (i = 0; i < res->count_crtcs; i++)
		if (res->crtcs[i] == crtc_id)
			return i;

	igt_assert(false);
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

unsigned int kmstest_get_vblank(int fd, int pipe, unsigned int flags)
{
	union drm_wait_vblank vbl;

	memset(&vbl, 0, sizeof(vbl));
	vbl.request.type = DRM_VBLANK_RELATIVE | pipe_select(pipe) | flags;
	if (drmIoctl(fd, DRM_IOCTL_WAIT_VBLANK, &vbl))
		return 0;

	return vbl.reply.sequence;
}

static void get_plane(char *str, int type, struct kmstest_plane *plane)
{
	int ret;
	char buf[256];

	plane->plane = type;
	ret = sscanf(str + 12, "%d%*c %*s %[^n]s",
		     &plane->id,
		     buf);
	igt_assert_eq(ret, 2);

	ret = sscanf(buf + 9, "%4d%*c%4d%*c", &plane->pos_x, &plane->pos_y);
	igt_assert_eq(ret, 2);

	ret = sscanf(buf + 30, "%4d%*c%4d%*c", &plane->width, &plane->height);
	igt_assert_eq(ret, 2);
}

static int parse_planes(FILE *fid, struct kmstest_plane *plane)
{
	char tmp[256];
	int nplanes;
	int ovl;

	ovl = 0;
	nplanes = 0;
	while (fgets(tmp, 256, fid) != NULL) {
		igt_assert_neq(nplanes, IGT_MAX_PLANES);
		if (strstr(tmp, "type=PRI") != NULL) {
			get_plane(tmp, IGT_PLANE_PRIMARY, &plane[nplanes]);
			nplanes++;
		} else if (strstr(tmp, "type=OVL") != NULL) {
			get_plane(tmp, IGT_PLANE_2 + ovl, &plane[nplanes]);
			ovl++;
			nplanes++;
		} else if (strstr(tmp, "type=CUR") != NULL) {
			get_plane(tmp, IGT_PLANE_CURSOR, &plane[nplanes]);
			nplanes++;
			break;
		}
	}

	return nplanes;
}

static void parse_crtc(char *info, struct kmstest_crtc *crtc)
{
	char buf[256];
	int ret;
	char pipe;

	ret = sscanf(info + 4, "%d%*c %*s %c%*c %*s %s%*c",
		     &crtc->id, &pipe, buf);
	igt_assert_eq(ret, 3);

	crtc->pipe = kmstest_pipe_to_index(pipe);
	igt_assert(crtc->pipe >= 0);

	ret = sscanf(buf + 6, "%d%*c%d%*c",
		     &crtc->width, &crtc->height);
	igt_assert_eq(ret, 2);
}

void kmstest_get_crtc(enum pipe pipe, struct kmstest_crtc *crtc)
{
	char tmp[256];
	FILE *fid;
	const char *mode = "r";
	int ncrtc;
	int line;

	fid = igt_debugfs_fopen("i915_display_info", mode);

	igt_skip_on(fid == NULL);

	ncrtc = 0;
	line = 0;
	while (fgets(tmp, 256, fid) != NULL) {
		if ((strstr(tmp, "CRTC") != NULL) && (line > 0)) {
			if (strstr(tmp, "active=yes") != NULL) {
				crtc->active = true;
				parse_crtc(tmp, crtc);
				crtc->nplanes = parse_planes(fid, crtc->plane);

				if (crtc->pipe != pipe)
					crtc = NULL;
				else
					ncrtc++;
			}
		}

		line++;
	}

	fclose(fid);

	igt_skip_on(ncrtc == 0);
}

void igt_assert_plane_visible(enum pipe pipe, bool visibility)
{
	struct kmstest_crtc crtc;
	int i;
	bool visible;

	kmstest_get_crtc(pipe, &crtc);

	visible = true;
	for (i = IGT_PLANE_2; i < crtc.nplanes; i++) {
		if (crtc.plane[i].pos_x > crtc.width) {
			visible = false;
			break;
		} else if (crtc.plane[i].pos_y > crtc.height) {
			visible = false;
			break;
		}
	}

	igt_assert_eq(visible, visibility);
}

/*
 * A small modeset API
 */

#define LOG_SPACES		"    "
#define LOG_N_SPACES		(sizeof(LOG_SPACES) - 1)

#define LOG_INDENT(d, section)				\
	do {						\
		igt_display_log(d, "%s {\n", section);	\
		igt_display_log_shift(d, 1);		\
	} while (0)
#define LOG_UNINDENT(d)					\
	do {						\
		igt_display_log_shift(d, -1);		\
		igt_display_log(d, "}\n");		\
	} while (0)
#define LOG(d, fmt, ...)	igt_display_log(d, fmt, ## __VA_ARGS__)

static void  __attribute__((format(printf, 2, 3)))
igt_display_log(igt_display_t *display, const char *fmt, ...)
{
	va_list args;
	int i;

	va_start(args, fmt);
	igt_debug("display: ");
	for (i = 0; i < display->log_shift; i++)
		igt_debug("%s", LOG_SPACES);
	igt_vlog(IGT_LOG_DOMAIN, IGT_LOG_DEBUG, fmt, args);
	va_end(args);
}

static void igt_display_log_shift(igt_display_t *display, int shift)
{
	display->log_shift += shift;
	igt_assert(display->log_shift >= 0);
}

static void igt_output_refresh(igt_output_t *output, bool final)
{
	igt_display_t *display = output->display;
	unsigned long crtc_idx_mask;

	crtc_idx_mask = output->pending_crtc_idx_mask;

	/* we mask out the pipes already in use */
	if (final)
		crtc_idx_mask &= ~display->pipes_in_use;

	kmstest_free_connector_config(&output->config);

	_kmstest_connector_config(display->drm_fd, output->id, crtc_idx_mask,
				  &output->config, output->force_reprobe);
	output->force_reprobe = false;

	if (!output->name && output->config.connector) {
		drmModeConnector *c = output->config.connector;

		igt_assert_neq(asprintf(&output->name, "%s-%d", kmstest_connector_type_str(c->connector_type), c->connector_type_id),
			       -1);
	}

	if (output->config.connector)
		igt_atomic_fill_connector_props(display, output,
			IGT_NUM_CONNECTOR_PROPS, igt_connector_prop_names);

	if (output->use_override_mode)
		output->config.default_mode = output->override_mode;

	if (output->config.pipe == PIPE_NONE)
		return;

	LOG(display, "%s: Selecting pipe %s\n", output->name,
	    kmstest_pipe_name(output->config.pipe));

	if (final)
		display->pipes_in_use |= 1 << output->config.pipe;
}

static bool
get_plane_property(int drm_fd, uint32_t plane_id, const char *name,
		   uint32_t *prop_id /* out */, uint64_t *value /* out */,
		   drmModePropertyPtr *prop /* out */)
{
	return kmstest_get_property(drm_fd, plane_id, DRM_MODE_OBJECT_PLANE,
				    name, prop_id, value, prop);
}

static int
igt_plane_set_property(igt_plane_t *plane, uint32_t prop_id, uint64_t value)
{
	igt_pipe_t *pipe = plane->pipe;
	igt_display_t *display = pipe->display;

	return drmModeObjectSetProperty(display->drm_fd, plane->drm_plane->plane_id,
				 DRM_MODE_OBJECT_PLANE, prop_id, value);
}

static bool
get_crtc_property(int drm_fd, uint32_t crtc_id, const char *name,
		   uint32_t *prop_id /* out */, uint64_t *value /* out */,
		   drmModePropertyPtr *prop /* out */)
{
	return kmstest_get_property(drm_fd, crtc_id, DRM_MODE_OBJECT_CRTC,
				    name, prop_id, value, prop);
}

static void
igt_crtc_set_property(igt_pipe_t *pipe, uint32_t prop_id, uint64_t value)
{
	drmModeObjectSetProperty(pipe->display->drm_fd,
		pipe->crtc_id, DRM_MODE_OBJECT_CRTC, prop_id, value);
}

/*
 * Walk a plane's property list to determine its type.  If we don't
 * find a type property, then the kernel doesn't support universal
 * planes and we know the plane is an overlay/sprite.
 */
static int get_drm_plane_type(int drm_fd, uint32_t plane_id)
{
	uint64_t value;
	bool has_prop;

	has_prop = get_plane_property(drm_fd, plane_id, "type",
				      NULL /* prop_id */, &value, NULL);
	if (has_prop)
		return (int)value;

	return DRM_PLANE_TYPE_OVERLAY;
}

/**
 * igt_display_init:
 * @display: a pointer to an #igt_display_t structure
 * @drm_fd: a drm file descriptor
 *
 * Initialize @display and allocate the various resources required. Use
 * #igt_display_fini to release the resources when they are no longer required.
 *
 */
void igt_display_init(igt_display_t *display, int drm_fd)
{
	drmModeRes *resources;
	drmModePlaneRes *plane_resources;
	int i;
	int is_atomic = 0;

	memset(display, 0, sizeof(igt_display_t));

	LOG_INDENT(display, "init");

	display->drm_fd = drm_fd;

	resources = drmModeGetResources(display->drm_fd);
	igt_assert(resources);

	/*
	 * We cache the number of pipes, that number is a physical limit of the
	 * hardware and cannot change of time (for now, at least).
	 */
	display->n_pipes = resources->count_crtcs;
	display->pipes = calloc(sizeof(igt_pipe_t), display->n_pipes);

	drmSetClientCap(drm_fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
	is_atomic = drmSetClientCap(drm_fd, DRM_CLIENT_CAP_ATOMIC, 1);
	plane_resources = drmModeGetPlaneResources(display->drm_fd);
	igt_assert(plane_resources);

	for (i = 0; i < display->n_pipes; i++) {
		igt_pipe_t *pipe = &display->pipes[i];
		igt_plane_t *plane;
		int p = IGT_PLANE_2;
		int j, type;
		uint8_t n_planes = 0;
		uint64_t prop_value;

		pipe->crtc_id = resources->crtcs[i];
		pipe->display = display;
		pipe->pipe = i;

		get_crtc_property(display->drm_fd, pipe->crtc_id,
				    "background_color",
				    &pipe->background_property,
				    &prop_value,
				    NULL);
		pipe->background = (uint32_t)prop_value;
		get_crtc_property(display->drm_fd, pipe->crtc_id,
				  "DEGAMMA_LUT",
				  &pipe->degamma_property,
				  NULL,
				  NULL);
		get_crtc_property(display->drm_fd, pipe->crtc_id,
				  "CTM",
				  &pipe->ctm_property,
				  NULL,
				  NULL);
		get_crtc_property(display->drm_fd, pipe->crtc_id,
				  "GAMMA_LUT",
				  &pipe->gamma_property,
				  NULL,
				  NULL);

		igt_atomic_fill_pipe_props(display, pipe, IGT_NUM_CRTC_PROPS, igt_crtc_prop_names);

		/* add the planes that can be used with that pipe */
		for (j = 0; j < plane_resources->count_planes; j++) {
			drmModePlane *drm_plane;

			drm_plane = drmModeGetPlane(display->drm_fd,
						    plane_resources->planes[j]);
			igt_assert(drm_plane);

			if (!(drm_plane->possible_crtcs & (1 << i))) {
				drmModeFreePlane(drm_plane);
				continue;
			}

			type = get_drm_plane_type(display->drm_fd,
						  plane_resources->planes[j]);
			switch (type) {
			case DRM_PLANE_TYPE_PRIMARY:
				plane = &pipe->planes[IGT_PLANE_PRIMARY];
				plane->is_primary = 1;
				plane->index = IGT_PLANE_PRIMARY;
				break;
			case DRM_PLANE_TYPE_CURSOR:
				/*
				 * Cursor should be the highest index in our
				 * internal list, but we don't know what that
				 * is yet.  Just stick it in the last slot
				 * for now and we'll move it later, if
				 * necessary.
				 */
				plane = &pipe->planes[IGT_PLANE_CURSOR];
				plane->is_cursor = 1;
				plane->index = IGT_PLANE_CURSOR;
				display->has_cursor_plane = true;
				break;
			default:
				plane = &pipe->planes[p];
				plane->index = p++;
				break;
			}

			n_planes++;
			plane->pipe = pipe;
			plane->drm_plane = drm_plane;

			if (is_atomic == 0) {
				display->is_atomic = 1;
				igt_atomic_fill_plane_props(display, plane, IGT_NUM_PLANE_PROPS, igt_plane_prop_names);
			}

			get_plane_property(display->drm_fd, drm_plane->plane_id,
					   "rotation",
					   &plane->rotation_property,
					   &prop_value,
					   NULL);
			plane->rotation = (igt_rotation_t)prop_value;
		}

		/*
		 * At the bare minimum, we should expect to have a primary
		 * plane
		 */
		igt_assert(pipe->planes[IGT_PLANE_PRIMARY].drm_plane);

		if (display->has_cursor_plane) {
			/*
			 * Cursor was put in the last slot.  If we have 0 or
			 * only 1 sprite, that's the wrong slot and we need to
			 * move it down.
			 */
			if (p != IGT_PLANE_CURSOR) {
				pipe->planes[p] =
					pipe->planes[IGT_PLANE_CURSOR];
				pipe->planes[p].index = p;
				memset(&pipe->planes[IGT_PLANE_CURSOR], 0,
				       sizeof *plane);
			}
		} else {
			/* Add drm_plane-less cursor */
			plane = &pipe->planes[p];
			plane->pipe = pipe;
			plane->index = p;
			plane->is_cursor = true;
		}

		pipe->n_planes = n_planes;

		for_each_plane_on_pipe(display, i, plane)
			plane->fb_changed = true;

		/* make sure we don't overflow the plane array */
		igt_assert_lte(pipe->n_planes, IGT_MAX_PLANES);

		pipe->mode_changed = true;
	}

	/*
	 * The number of connectors is set, so we just initialize the outputs
	 * array in _init(). This may change when we need dynamic connectors
	 * (say DisplayPort MST).
	 */
	display->n_outputs = resources->count_connectors;
	display->outputs = calloc(display->n_outputs, sizeof(igt_output_t));
	igt_assert(display->outputs);

	for (i = 0; i < display->n_outputs; i++) {
		igt_output_t *output = &display->outputs[i];

		/*
		 * We don't assign each output a pipe unless
		 * a pipe is set with igt_output_set_pipe().
		 */
		output->force_reprobe = true;
		output->pending_crtc_idx_mask = 0;
		output->id = resources->connectors[i];
		output->display = display;

		igt_output_refresh(output, false);

		output->config.pipe_changed = true;
	}

	drmModeFreePlaneResources(plane_resources);
	drmModeFreeResources(resources);

	LOG_UNINDENT(display);
}

int igt_display_get_n_pipes(igt_display_t *display)
{
	return display->n_pipes;
}

static void igt_pipe_fini(igt_pipe_t *pipe)
{
	int i;

	for (i = 0; i < pipe->n_planes; i++) {
		igt_plane_t *plane = &pipe->planes[i];

		if (plane->drm_plane) {
			drmModeFreePlane(plane->drm_plane);
			plane->drm_plane = NULL;
		}
	}
}

static void igt_output_fini(igt_output_t *output)
{
	kmstest_free_connector_config(&output->config);
	free(output->name);
}

/**
 * igt_display_fini:
 * @display: a pointer to an #igt_display_t structure
 *
 * Release any resources associated with @display. This does not free @display
 * itself.
 */
void igt_display_fini(igt_display_t *display)
{
	int i;

	for (i = 0; i < display->n_pipes; i++)
		igt_pipe_fini(&display->pipes[i]);

	for (i = 0; i < display->n_outputs; i++)
		igt_output_fini(&display->outputs[i]);
	free(display->outputs);
	free(display->pipes);
	display->outputs = NULL;
}

static void igt_display_refresh(igt_display_t *display)
{
	int i, j;

	display->pipes_in_use = 0;

       /* Check that two outputs aren't trying to use the same pipe */
        for (i = 0; i < display->n_outputs; i++) {
                igt_output_t *a = &display->outputs[i];

                if (!a->pending_crtc_idx_mask)
                        continue;

                for (j = 0; j < display->n_outputs; j++) {
                        igt_output_t *b = &display->outputs[j];

                        if (i == j)
                                continue;

                        igt_assert_f(a->pending_crtc_idx_mask !=
                                     b->pending_crtc_idx_mask,
                                     "%s and %s are both trying to use pipe %s\n",
                                     igt_output_name(a), igt_output_name(b),
                                     kmstest_pipe_name(ffs(a->pending_crtc_idx_mask) - 1));
                }
        }

	for (i = 0; i < display->n_outputs; i++) {
		igt_output_t *output = &display->outputs[i];

		igt_output_refresh(output, true);
	}
}

static igt_pipe_t *igt_output_get_driving_pipe(igt_output_t *output)
{
	igt_display_t *display = output->display;
	enum pipe pipe;

	if (!output->pending_crtc_idx_mask) {
		/*
		 * The user hasn't specified a pipe to use, return none.
		 */
		return NULL;
	} else {
		/*
		 * Otherwise, return the pending pipe (ie the pipe that should
		 * drive this output after the commit()
		 */
		pipe = ffs(output->pending_crtc_idx_mask) - 1;
	}

	igt_assert(pipe >= 0 && pipe < display->n_pipes);

	return &display->pipes[pipe];
}

static igt_plane_t *igt_pipe_get_plane(igt_pipe_t *pipe, enum igt_plane plane)
{
	int idx;

	/* Cursor plane is always the highest index */
	if (plane == IGT_PLANE_CURSOR)
		idx = pipe->n_planes - 1;
	else {
		igt_assert_f(plane >= 0 && plane < (pipe->n_planes),
			     "plane=%d\n", plane);
		idx = plane;
	}

	return &pipe->planes[idx];
}

static igt_output_t *igt_pipe_get_output(igt_pipe_t *pipe)
{
	igt_display_t *display = pipe->display;
	int i;

	for (i = 0; i < display->n_outputs; i++) {
		igt_output_t *output = &display->outputs[i];

		if (output->pending_crtc_idx_mask == (1 << pipe->pipe))
			return output;
	}

	return NULL;
}

bool igt_pipe_get_property(igt_pipe_t *pipe, const char *name,
			   uint32_t *prop_id, uint64_t *value,
			   drmModePropertyPtr *prop)
{
	return get_crtc_property(pipe->display->drm_fd,
				 pipe->crtc_id,
				 name,
				 prop_id, value, prop);
}

static uint32_t igt_plane_get_fb_id(igt_plane_t *plane)
{
	if (plane->fb)
		return plane->fb->fb_id;
	else
		return 0;
}

static uint32_t igt_plane_get_fb_gem_handle(igt_plane_t *plane)
{
	if (plane->fb)
		return plane->fb->gem_handle;
	else
		return 0;
}

#define CHECK_RETURN(r, fail) {	\
	if (r && !fail)		\
		return r;	\
	igt_assert_eq(r, 0);	\
}




/*
 * Add position and fb changes of a plane to the atomic property set
 */
static void
igt_atomic_prepare_plane_commit(igt_plane_t *plane, igt_pipe_t *pipe,
	drmModeAtomicReq *req)
{
	igt_display_t *display = pipe->display;
	uint32_t fb_id, crtc_id;

	igt_assert(plane->drm_plane);

	/* it's an error to try an unsupported feature */
	igt_assert(igt_plane_supports_rotation(plane) ||
			!plane->rotation_changed);

	fb_id = igt_plane_get_fb_id(plane);
	crtc_id = pipe->crtc_id;

	LOG(display,
	    "populating plane data: %s.%d, fb %u\n",
	    kmstest_pipe_name(pipe->pipe),
	    plane->index,
	    fb_id);

	if (plane->fb_changed) {
		igt_atomic_populate_plane_req(req, plane, IGT_PLANE_CRTC_ID, fb_id ? crtc_id : 0);
		igt_atomic_populate_plane_req(req, plane, IGT_PLANE_FB_ID, fb_id);
	}

	if (plane->position_changed || plane->size_changed) {
		uint32_t src_x = IGT_FIXED(plane->src_x, 0); /* src_x */
		uint32_t src_y = IGT_FIXED(plane->src_y, 0); /* src_y */
		uint32_t src_w = IGT_FIXED(plane->src_w, 0); /* src_w */
		uint32_t src_h = IGT_FIXED(plane->src_h, 0); /* src_h */
		int32_t crtc_x = plane->crtc_x;
		int32_t crtc_y = plane->crtc_y;
		uint32_t crtc_w = plane->crtc_w;
		uint32_t crtc_h = plane->crtc_h;

		LOG(display,
		"src = (%d, %d) %u x %u "
		"dst = (%d, %d) %u x %u\n",
		src_x >> 16, src_y >> 16, src_w >> 16, src_h >> 16,
		crtc_x, crtc_y, crtc_w, crtc_h);

		igt_atomic_populate_plane_req(req, plane, IGT_PLANE_SRC_X, src_x);
		igt_atomic_populate_plane_req(req, plane, IGT_PLANE_SRC_Y, src_y);
		igt_atomic_populate_plane_req(req, plane, IGT_PLANE_SRC_W, src_w);
		igt_atomic_populate_plane_req(req, plane, IGT_PLANE_SRC_H, src_h);

		igt_atomic_populate_plane_req(req, plane, IGT_PLANE_CRTC_X, crtc_x);
		igt_atomic_populate_plane_req(req, plane, IGT_PLANE_CRTC_Y, crtc_y);
		igt_atomic_populate_plane_req(req, plane, IGT_PLANE_CRTC_W, crtc_w);
		igt_atomic_populate_plane_req(req, plane, IGT_PLANE_CRTC_H, crtc_h);
	}

	if (plane->rotation_changed)
		igt_atomic_populate_plane_req(req, plane,
			IGT_PLANE_ROTATION, plane->rotation);
}



/*
 * Commit position and fb changes to a DRM plane via the SetPlane ioctl; if the
 * DRM call to program the plane fails, we'll either fail immediately (for
 * tests that expect the commit to succeed) or return the failure code (for
 * tests that expect a specific error code).
 */
static int igt_drm_plane_commit(igt_plane_t *plane,
				igt_pipe_t *pipe,
				bool fail_on_error)
{
	igt_display_t *display = pipe->display;
	uint32_t fb_id, crtc_id;
	int ret;
	uint32_t src_x;
	uint32_t src_y;
	uint32_t src_w;
	uint32_t src_h;
	int32_t crtc_x;
	int32_t crtc_y;
	uint32_t crtc_w;
	uint32_t crtc_h;

	igt_assert(plane->drm_plane);

	/* it's an error to try an unsupported feature */
	igt_assert(igt_plane_supports_rotation(plane) ||
		   !plane->rotation_changed);

	fb_id = igt_plane_get_fb_id(plane);
	crtc_id = pipe->crtc_id;

	if ((plane->fb_changed || plane->size_changed) && fb_id == 0) {
		LOG(display,
		    "SetPlane pipe %s, plane %d, disabling\n",
		    kmstest_pipe_name(pipe->pipe),
		    plane->index);

		ret = drmModeSetPlane(display->drm_fd,
				      plane->drm_plane->plane_id,
				      crtc_id,
				      fb_id,
				      0,    /* flags */
				      0, 0, /* crtc_x, crtc_y */
				      0, 0, /* crtc_w, crtc_h */
				      IGT_FIXED(0,0), /* src_x */
				      IGT_FIXED(0,0), /* src_y */
				      IGT_FIXED(0,0), /* src_w */
				      IGT_FIXED(0,0) /* src_h */);

		CHECK_RETURN(ret, fail_on_error);
	} else if (plane->fb_changed || plane->position_changed ||
		plane->size_changed) {
		src_x = IGT_FIXED(plane->src_x,0); /* src_x */
		src_y = IGT_FIXED(plane->src_y,0); /* src_y */
		src_w = IGT_FIXED(plane->src_w,0); /* src_w */
		src_h = IGT_FIXED(plane->src_h,0); /* src_h */
		crtc_x = plane->crtc_x;
		crtc_y = plane->crtc_y;
		crtc_w = plane->crtc_w;
		crtc_h = plane->crtc_h;

		LOG(display,
		    "SetPlane %s.%d, fb %u, src = (%d, %d) "
			"%ux%u dst = (%u, %u) %ux%u\n",
		    kmstest_pipe_name(pipe->pipe),
		    plane->index,
		    fb_id,
		    src_x >> 16, src_y >> 16, src_w >> 16, src_h >> 16,
		    crtc_x, crtc_y, crtc_w, crtc_h);

		ret = drmModeSetPlane(display->drm_fd,
				      plane->drm_plane->plane_id,
				      crtc_id,
				      fb_id,
				      0,    /* flags */
				      crtc_x, crtc_y,
				      crtc_w, crtc_h,
				      src_x, src_y,
				      src_w, src_h);

		CHECK_RETURN(ret, fail_on_error);
	}

	if (plane->rotation_changed) {
		ret = igt_plane_set_property(plane, plane->rotation_property,
				       plane->rotation);

		CHECK_RETURN(ret, fail_on_error);
	}

	return 0;
}

/*
 * Commit position and fb changes to a cursor via legacy ioctl's.  If commit
 * fails, we'll either fail immediately (for tests that expect the commit to
 * succeed) or return the failure code (for tests that expect a specific error
 * code).
 */
static int igt_cursor_commit_legacy(igt_plane_t *cursor,
				    igt_pipe_t *pipe,
				    bool fail_on_error)
{
	igt_display_t *display = pipe->display;
	uint32_t crtc_id = pipe->crtc_id;
	int ret;

	if (cursor->fb_changed) {
		uint32_t gem_handle = igt_plane_get_fb_gem_handle(cursor);

		if (gem_handle) {
			LOG(display,
			    "SetCursor pipe %s, fb %u %dx%d\n",
			    kmstest_pipe_name(pipe->pipe),
			    gem_handle,
			    cursor->crtc_w, cursor->crtc_h);

			ret = drmModeSetCursor(display->drm_fd, crtc_id,
					       gem_handle,
					       cursor->crtc_w,
					       cursor->crtc_h);
		} else {
			LOG(display,
			    "SetCursor pipe %s, disabling\n",
			    kmstest_pipe_name(pipe->pipe));

			ret = drmModeSetCursor(display->drm_fd, crtc_id,
					       0, 0, 0);
		}

		CHECK_RETURN(ret, fail_on_error);
	}

	if (cursor->position_changed) {
		int x = cursor->crtc_x;
		int y = cursor->crtc_y;

		LOG(display,
		    "MoveCursor pipe %s, (%d, %d)\n",
		    kmstest_pipe_name(pipe->pipe),
		    x, y);

		ret = drmModeMoveCursor(display->drm_fd, crtc_id, x, y);
		CHECK_RETURN(ret, fail_on_error);
	}

	return 0;
}

/*
 * Commit position and fb changes to a primary plane via the legacy interface
 * (setmode).
 */
static int igt_primary_plane_commit_legacy(igt_plane_t *primary,
					   igt_pipe_t *pipe,
					   bool fail_on_error)
{
	struct igt_display *display = primary->pipe->display;
	igt_output_t *output = igt_pipe_get_output(pipe);
	drmModeModeInfo *mode;
	uint32_t fb_id, crtc_id;
	int ret;

	/* Primary planes can't be windowed when using a legacy commit */
	igt_assert((primary->crtc_x == 0 && primary->crtc_y == 0));

	/* nor rotated */
	igt_assert(!primary->rotation_changed);

	if (!primary->fb_changed && !primary->position_changed &&
	    !primary->size_changed)
		return 0;

	crtc_id = pipe->crtc_id;
	fb_id = igt_plane_get_fb_id(primary);
	if (fb_id)
		mode = igt_output_get_mode(output);
	else
		mode = NULL;

	if (fb_id) {
		LOG(display,
		    "%s: SetCrtc pipe %s, fb %u, src (%d, %d), "
		    "mode %dx%d\n",
		    igt_output_name(output),
		    kmstest_pipe_name(pipe->pipe),
		    fb_id,
		    primary->src_x, primary->src_y,
		    mode->hdisplay, mode->vdisplay);

		ret = drmModeSetCrtc(display->drm_fd,
				     crtc_id,
				     fb_id,
				     primary->src_x, primary->src_y,
				     &output->id,
				     1,
				     mode);
	} else {
		LOG(display,
		    "SetCrtc pipe %s, disabling\n",
		    kmstest_pipe_name(pipe->pipe));

		ret = drmModeSetCrtc(display->drm_fd,
				     crtc_id,
				     fb_id,
				     0, 0, /* x, y */
				     NULL, /* connectors */
				     0,    /* n_connectors */
				     NULL  /* mode */);
	}

	CHECK_RETURN(ret, fail_on_error);

	primary->pipe->enabled = (fb_id != 0);

	return 0;
}


/*
 * Commit position and fb changes to a plane.  The value of @s will determine
 * which API is used to do the programming.
 */
static int igt_plane_commit(igt_plane_t *plane,
			    igt_pipe_t *pipe,
			    enum igt_commit_style s,
			    bool fail_on_error)
{
	if (plane->is_cursor && s == COMMIT_LEGACY) {
		return igt_cursor_commit_legacy(plane, pipe, fail_on_error);
	} else if (plane->is_primary && s == COMMIT_LEGACY) {
		return igt_primary_plane_commit_legacy(plane, pipe,
						       fail_on_error);
	} else {
		return igt_drm_plane_commit(plane, pipe, fail_on_error);
	}
}

/*
 * Commit all plane changes to an output.  Note that if @s is COMMIT_LEGACY,
 * enabling/disabling the primary plane will also enable/disable the CRTC.
 *
 * If @fail_on_error is true, any failure to commit plane state will lead
 * to subtest failure in the specific function where the failure occurs.
 * Otherwise, the first error code encountered will be returned and no
 * further programming will take place, which may result in some changes
 * taking effect and others not taking effect.
 */
static int igt_pipe_commit(igt_pipe_t *pipe,
			   enum igt_commit_style s,
			   bool fail_on_error)
{
	igt_display_t *display = pipe->display;
	int i;
	int ret;
	bool need_wait_for_vblank = false;

	if (pipe->background_changed) {
		igt_crtc_set_property(pipe, pipe->background_property,
			pipe->background);
	}

	if (pipe->color_mgmt_changed) {
		igt_crtc_set_property(pipe, pipe->degamma_property,
				      pipe->degamma_blob);
		igt_crtc_set_property(pipe, pipe->ctm_property,
				      pipe->ctm_blob);
		igt_crtc_set_property(pipe, pipe->gamma_property,
				      pipe->gamma_blob);
	}

	for (i = 0; i < pipe->n_planes; i++) {
		igt_plane_t *plane = &pipe->planes[i];

		if (plane->fb_changed || plane->position_changed || plane->size_changed)
			need_wait_for_vblank = true;

		ret = igt_plane_commit(plane, pipe, s, fail_on_error);
		CHECK_RETURN(ret, fail_on_error);
	}

	/*
	 * If the crtc is enabled, wait until the next vblank before returning
	 * if we made changes to any of the planes.
	 */
	if (need_wait_for_vblank && pipe->enabled) {
		igt_wait_for_vblank(display->drm_fd, pipe->pipe);
	}

	return 0;
}

static void
igt_pipe_replace_blob(igt_pipe_t *pipe, uint64_t *blob, void *ptr, size_t length)
{
	igt_display_t *display = pipe->display;
	uint32_t blob_id = 0;

	if (*blob != 0)
		igt_assert(drmModeDestroyPropertyBlob(display->drm_fd,
						      *blob) == 0);

	if (length > 0)
		igt_assert(drmModeCreatePropertyBlob(display->drm_fd,
						     ptr, length, &blob_id) == 0);

	*blob = blob_id;
}

/*
 * Add crtc property changes to the atomic property set
 */
static void igt_atomic_prepare_crtc_commit(igt_pipe_t *pipe_obj, drmModeAtomicReq *req)
{
	if (pipe_obj->background_changed)
		igt_atomic_populate_crtc_req(req, pipe_obj, IGT_CRTC_BACKGROUND, pipe_obj->background);

	if (pipe_obj->color_mgmt_changed) {
		igt_atomic_populate_crtc_req(req, pipe_obj, IGT_CRTC_DEGAMMA_LUT, pipe_obj->degamma_blob);
		igt_atomic_populate_crtc_req(req, pipe_obj, IGT_CRTC_CTM, pipe_obj->ctm_blob);
		igt_atomic_populate_crtc_req(req, pipe_obj, IGT_CRTC_GAMMA_LUT, pipe_obj->gamma_blob);
	}

	if (pipe_obj->mode_changed) {
		igt_output_t *output = igt_pipe_get_output(pipe_obj);

		if (!output) {
			igt_pipe_replace_blob(pipe_obj, &pipe_obj->mode_blob, NULL, 0);

			LOG(pipe_obj->display, "%s: Setting NULL mode\n",
			    kmstest_pipe_name(pipe_obj->pipe));
		} else {
			drmModeModeInfo *mode = igt_output_get_mode(output);

			igt_pipe_replace_blob(pipe_obj, &pipe_obj->mode_blob, mode, sizeof(*mode));

			LOG(pipe_obj->display, "%s: Setting mode %s from %s\n",
			    kmstest_pipe_name(pipe_obj->pipe),
			    mode->name, igt_output_name(output));
		}

		igt_atomic_populate_crtc_req(req, pipe_obj, IGT_CRTC_MODE_ID, pipe_obj->mode_blob);
		igt_atomic_populate_crtc_req(req, pipe_obj, IGT_CRTC_ACTIVE, !!output);
	}

	/*
	 *	TODO: Add all crtc level properties here
	 */
}

/*
 * Add connector property changes to the atomic property set
 */
static void igt_atomic_prepare_connector_commit(igt_output_t *output, drmModeAtomicReq *req)
{

	struct kmstest_connector_config *config = &output->config;

	if (config->connector_scaling_mode_changed)
		igt_atomic_populate_connector_req(req, output, IGT_CONNECTOR_SCALING_MODE, config->connector_scaling_mode);

	if (config->pipe_changed) {
		uint32_t crtc_id = 0;

		if (output->config.pipe != PIPE_NONE)
			crtc_id = output->config.crtc->crtc_id;

		igt_atomic_populate_connector_req(req, output, IGT_CONNECTOR_CRTC_ID, crtc_id);
	}
	/*
	 *	TODO: Add all other connector level properties here
	 */

}

/*
 * Commit all the changes of all the planes,crtcs, connectors
 * atomically using drmModeAtomicCommit()
 */
static int igt_atomic_commit(igt_display_t *display, uint32_t flags, void *user_data)
{

	int ret = 0, i;
	enum pipe pipe;
	drmModeAtomicReq *req;
	igt_output_t *output;

	if (display->is_atomic != 1)
		return -1;
	req = drmModeAtomicAlloc();
	drmModeAtomicSetCursor(req, 0);

	for_each_pipe(display, pipe) {
		igt_pipe_t *pipe_obj = &display->pipes[pipe];
		igt_plane_t *plane;

		/*
		 * Add CRTC Properties to the property set
		 */
		igt_atomic_prepare_crtc_commit(pipe_obj, req);

		for_each_plane_on_pipe(display, pipe, plane) {
			igt_atomic_prepare_plane_commit(plane, pipe_obj, req);
		}

	}

	for (i = 0; i < display->n_outputs; i++) {
		output = &display->outputs[i];

		if (!output->config.connector)
			continue;

		LOG(display, "%s: preparing atomic, pipe: %s\n",
		    igt_output_name(output),
		    kmstest_pipe_name(output->config.pipe));

		igt_atomic_prepare_connector_commit(output, req);
	}

	ret = drmModeAtomicCommit(display->drm_fd, req, flags, user_data);
	drmModeAtomicFree(req);
	return ret;

}

static void
display_commit_changed(igt_display_t *display, enum igt_commit_style s)
{
	int i;
	enum pipe pipe;

	for_each_pipe(display, pipe) {
		igt_pipe_t *pipe_obj = &display->pipes[pipe];
		igt_plane_t *plane;

		pipe_obj->color_mgmt_changed = false;
		pipe_obj->background_changed = false;

		if (s != COMMIT_UNIVERSAL)
			pipe_obj->mode_changed = false;

		for_each_plane_on_pipe(display, pipe, plane) {
			plane->fb_changed = false;
			plane->position_changed = false;
			plane->size_changed = false;

			if (s != COMMIT_LEGACY || !(plane->is_primary || plane->is_cursor))
				plane->rotation_changed = false;
		}
	}

	for (i = 0; i < display->n_outputs; i++) {
		igt_output_t *output = &display->outputs[i];

		if (s != COMMIT_UNIVERSAL)
			output->config.pipe_changed = false;

		if (s == COMMIT_ATOMIC)
			output->config.connector_scaling_mode_changed = false;
	}
}

/*
 * Commit all plane changes across all outputs of the display.
 *
 * If @fail_on_error is true, any failure to commit plane state will lead
 * to subtest failure in the specific function where the failure occurs.
 * Otherwise, the first error code encountered will be returned and no
 * further programming will take place, which may result in some changes
 * taking effect and others not taking effect.
 */
static int do_display_commit(igt_display_t *display,
			     enum igt_commit_style s,
			     bool fail_on_error)
{
	int ret;
	enum pipe pipe;
	LOG_INDENT(display, "commit");

	igt_display_refresh(display);

	if (s == COMMIT_ATOMIC) {
		ret = igt_atomic_commit(display, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);

		CHECK_RETURN(ret, fail_on_error);
	} else {
		int valid_outs = 0;

		for_each_pipe(display, pipe) {
			igt_pipe_t *pipe_obj = &display->pipes[pipe];
			igt_output_t *output = igt_pipe_get_output(pipe_obj);

			if (output)
				valid_outs++;

			ret = igt_pipe_commit(pipe_obj, s, fail_on_error);
			CHECK_RETURN(ret, fail_on_error);
		}

		CHECK_RETURN(ret, fail_on_error);

		if (valid_outs == 0) {
			LOG_UNINDENT(display);

			return -1;
		}
	}

	LOG_UNINDENT(display);

	if (ret)
		return ret;

	display_commit_changed(display, s);

	igt_debug_wait_for_keypress("modeset");

	return 0;
}

/**
 * igt_display_try_commit_atomic:
 * @display: #igt_display_t to commit.
 * @flags: Flags passed to drmModeAtomicCommit.
 * @user_data: User defined pointer passed to drmModeAtomicCommit.
 *
 * This function is similar to #igt_display_try_commit2, but is
 * used when you want to pass different flags to the actual commit.
 *
 * Useful flags can be DRM_MODE_ATOMIC_ALLOW_MODESET,
 * DRM_MODE_ATOMIC_NONBLOCK, DRM_MODE_PAGE_FLIP_EVENT,
 * or DRM_MODE_ATOMIC_TEST_ONLY.
 *
 * @user_data is returned in the event if you pass
 * DRM_MODE_PAGE_FLIP_EVENT to @flags.
 *
 * This function will return an error if commit fails, instead of
 * aborting the test.
 */
int igt_display_try_commit_atomic(igt_display_t *display, uint32_t flags, void *user_data)
{
	int ret;

	LOG_INDENT(display, "commit");

	igt_display_refresh(display);

	ret = igt_atomic_commit(display, flags, user_data);

	LOG_UNINDENT(display);

	if (ret || (flags & DRM_MODE_ATOMIC_TEST_ONLY))
		return ret;

	display_commit_changed(display, COMMIT_ATOMIC);

	igt_debug_wait_for_keypress("modeset");

	return 0;
}

/**
 * igt_display_commit_atomic:
 * @display: #igt_display_t to commit.
 * @flags: Flags passed to drmModeAtomicCommit.
 * @user_data: User defined pointer passed to drmModeAtomicCommit.
 *
 * This function is similar to #igt_display_commit2, but is
 * used when you want to pass different flags to the actual commit.
 *
 * Useful flags can be DRM_MODE_ATOMIC_ALLOW_MODESET,
 * DRM_MODE_ATOMIC_NONBLOCK, DRM_MODE_PAGE_FLIP_EVENT,
 * or DRM_MODE_ATOMIC_TEST_ONLY.
 *
 * @user_data is returned in the event if you pass
 * DRM_MODE_PAGE_FLIP_EVENT to @flags.
 *
 * This function will abort the test if commit fails.
 */
void igt_display_commit_atomic(igt_display_t *display, uint32_t flags, void *user_data)
{
	int ret = igt_display_try_commit_atomic(display, flags, user_data);

	igt_assert_eq(ret, 0);
}

/**
 * igt_display_commit2:
 * @display: DRM device handle
 * @s: Commit style
 *
 * Commits framebuffer and positioning changes to all planes of each display
 * pipe, using a specific API to perform the programming.  This function should
 * be used to exercise a specific driver programming API; igt_display_commit
 * should be used instead if the API used is unimportant to the test being run.
 *
 * This function should only be used to commit changes that are expected to
 * succeed, since any failure during the commit process will cause the IGT
 * subtest to fail.  To commit changes that are expected to fail, use
 * @igt_try_display_commit2 instead.
 *
 * Returns: 0 upon success.  This function will never return upon failure
 * since igt_fail() at lower levels will longjmp out of it.
 */
int igt_display_commit2(igt_display_t *display,
		       enum igt_commit_style s)
{
	do_display_commit(display, s, true);

	return 0;
}

/**
 * igt_display_try_commit2:
 * @display: DRM device handle
 * @s: Commit style
 *
 * Attempts to commit framebuffer and positioning changes to all planes of each
 * display pipe.  This function should be used to commit changes that are
 * expected to fail, so that the error code can be checked for correctness.
 * For changes that are expected to succeed, use @igt_display_commit instead.
 *
 * Note that in non-atomic commit styles, no display programming will be
 * performed after the first failure is encountered, so only some of the
 * operations requested by a test may have been completed.  Tests that catch
 * errors returned by this function should take care to restore the display to
 * a sane state after a failure is detected.
 *
 * Returns: 0 upon success, otherwise the error code of the first error
 * encountered.
 */
int igt_display_try_commit2(igt_display_t *display, enum igt_commit_style s)
{
	return do_display_commit(display, s, false);
}

/**
 * igt_display_commit:
 * @display: DRM device handle
 *
 * Commits framebuffer and positioning changes to all planes of each display
 * pipe.
 *
 * Returns: 0 upon success.  This function will never return upon failure
 * since igt_fail() at lower levels will longjmp out of it.
 */
int igt_display_commit(igt_display_t *display)
{
	return igt_display_commit2(display, COMMIT_LEGACY);
}

const char *igt_output_name(igt_output_t *output)
{
	return output->name;
}

drmModeModeInfo *igt_output_get_mode(igt_output_t *output)
{
	return &output->config.default_mode;
}

/**
 * igt_output_override_mode:
 * @output: Output of which the mode will be overridden
 * @mode: New mode, or NULL to disable override.
 *
 * Overrides the output's mode with @mode, so that it is used instead of the
 * mode obtained with get connectors. Note that the mode is used without
 * checking if the output supports it, so this might lead to unexpected results.
 */
void igt_output_override_mode(igt_output_t *output, drmModeModeInfo *mode)
{
	igt_pipe_t *pipe = igt_output_get_driving_pipe(output);

	if (mode)
		output->override_mode = *mode;
	else /* restore default_mode, may have been overwritten in igt_output_refresh */
		kmstest_get_connector_default_mode(output->display->drm_fd,
						   output->config.connector,
						   &output->config.default_mode);

	output->use_override_mode = !!mode;

	if (pipe)
		pipe->mode_changed = true;
}

void igt_output_set_pipe(igt_output_t *output, enum pipe pipe)
{
	igt_display_t *display = output->display;
	igt_pipe_t *old_pipe;

	igt_assert(output->name);

	if (output->pending_crtc_idx_mask) {
		old_pipe = igt_output_get_driving_pipe(output);

		old_pipe->mode_changed = true;
	}

	if (pipe == PIPE_NONE) {
		LOG(display, "%s: set_pipe(any)\n", igt_output_name(output));
		output->pending_crtc_idx_mask = 0;
	} else {
		LOG(display, "%s: set_pipe(%s)\n", igt_output_name(output),
		    kmstest_pipe_name(pipe));
		output->pending_crtc_idx_mask = 1 << pipe;

		display->pipes[pipe].mode_changed = true;
	}

	if (pipe != output->config.pipe)
		output->config.pipe_changed = true;

	igt_output_refresh(output, false);
}

void igt_output_set_scaling_mode(igt_output_t *output, uint64_t scaling_mode)
{
	output->config.connector_scaling_mode_changed = true;

	output->config.connector_scaling_mode = scaling_mode;

	igt_require(output->config.atomic_props_connector[IGT_CONNECTOR_SCALING_MODE]);
}

igt_plane_t *igt_output_get_plane(igt_output_t *output, enum igt_plane plane)
{
	igt_pipe_t *pipe;

	pipe = igt_output_get_driving_pipe(output);
	igt_assert(pipe);

	return igt_pipe_get_plane(pipe, plane);
}

void igt_plane_set_fb(igt_plane_t *plane, struct igt_fb *fb)
{
	igt_pipe_t *pipe = plane->pipe;
	igt_display_t *display = pipe->display;

	LOG(display, "%s.%d: plane_set_fb(%d)\n", kmstest_pipe_name(pipe->pipe),
	    plane->index, fb ? fb->fb_id : 0);

	plane->fb = fb;
	/* hack to keep tests working that don't call igt_plane_set_size() */
	if (fb) {
		/* set default plane size as fb size */
		plane->crtc_w = fb->width;
		plane->crtc_h = fb->height;

		/* set default src pos/size as fb size */
		plane->src_x = 0;
		plane->src_y = 0;
		plane->src_w = fb->width;
		plane->src_h = fb->height;
	} else {
		plane->src_x = 0;
		plane->src_y = 0;
		plane->src_w = 0;
		plane->src_h = 0;

		plane->crtc_w = 0;
		plane->crtc_h = 0;
	}

	plane->fb_changed = true;
	plane->size_changed = true;
}

void igt_plane_set_position(igt_plane_t *plane, int x, int y)
{
	igt_pipe_t *pipe = plane->pipe;
	igt_display_t *display = pipe->display;

	LOG(display, "%s.%d: plane_set_position(%d,%d)\n",
	    kmstest_pipe_name(pipe->pipe), plane->index, x, y);

	plane->crtc_x = x;
	plane->crtc_y = y;

	plane->position_changed = true;
}

/**
 * igt_plane_set_size:
 * @plane: plane pointer for which size to be set
 * @w: width
 * @h: height
 *
 * This function sets width and height for requested plane.
 * New size will be committed at plane commit time via
 * drmModeSetPlane().
 */
void igt_plane_set_size(igt_plane_t *plane, int w, int h)
{
	igt_pipe_t *pipe = plane->pipe;
	igt_display_t *display = pipe->display;

	LOG(display, "%s.%d: plane_set_size (%dx%d)\n",
	    kmstest_pipe_name(pipe->pipe), plane->index, w, h);

	plane->crtc_w = w;
	plane->crtc_h = h;

	plane->size_changed = true;
}

/**
 * igt_fb_set_position:
 * @fb: framebuffer pointer
 * @plane: plane
 * @x: X position
 * @y: Y position
 *
 * This function sets position for requested framebuffer as src to plane.
 * New position will be committed at plane commit time via drmModeSetPlane().
 */
void igt_fb_set_position(struct igt_fb *fb, igt_plane_t *plane,
	uint32_t x, uint32_t y)
{
	igt_pipe_t *pipe = plane->pipe;
	igt_display_t *display = pipe->display;

	LOG(display, "%s.%d: fb_set_position(%d,%d)\n",
	    kmstest_pipe_name(pipe->pipe), plane->index, x, y);

	plane->src_x = x;
	plane->src_y = y;

	plane->fb_changed = true;
}

/**
 * igt_fb_set_size:
 * @fb: framebuffer pointer
 * @plane: plane
 * @w: width
 * @h: height
 *
 * This function sets fetch rect size from requested framebuffer as src
 * to plane. New size will be committed at plane commit time via
 * drmModeSetPlane().
 */
void igt_fb_set_size(struct igt_fb *fb, igt_plane_t *plane,
	uint32_t w, uint32_t h)
{
	igt_pipe_t *pipe = plane->pipe;
	igt_display_t *display = pipe->display;

	LOG(display, "%s.%d: fb_set_size(%dx%d)\n",
	    kmstest_pipe_name(pipe->pipe), plane->index, w, h);

	plane->src_w = w;
	plane->src_h = h;

	plane->fb_changed = true;
}

static const char *rotation_name(igt_rotation_t rotation)
{
	switch (rotation) {
	case IGT_ROTATION_0:
		return "0°";
	case IGT_ROTATION_90:
		return "90°";
	case IGT_ROTATION_180:
		return "180°";
	case IGT_ROTATION_270:
		return "270°";
	default:
		igt_assert(0);
	}
}

void igt_plane_set_rotation(igt_plane_t *plane, igt_rotation_t rotation)
{
	igt_pipe_t *pipe = plane->pipe;
	igt_display_t *display = pipe->display;

	LOG(display, "%s.%d: plane_set_rotation(%s)\n",
	    kmstest_pipe_name(pipe->pipe),
	    plane->index, rotation_name(rotation));

	plane->rotation = rotation;

	plane->rotation_changed = true;
}

void
igt_pipe_set_degamma_lut(igt_pipe_t *pipe, void *ptr, size_t length)
{
	igt_pipe_replace_blob(pipe, &pipe->degamma_blob, ptr, length);
	pipe->color_mgmt_changed = 1;
}

void
igt_pipe_set_ctm_matrix(igt_pipe_t *pipe, void *ptr, size_t length)
{
	igt_pipe_replace_blob(pipe, &pipe->ctm_blob, ptr, length);
	pipe->color_mgmt_changed = 1;
}

void
igt_pipe_set_gamma_lut(igt_pipe_t *pipe, void *ptr, size_t length)
{
	igt_pipe_replace_blob(pipe, &pipe->gamma_blob, ptr, length);
	pipe->color_mgmt_changed = 1;
}

/**
 * igt_crtc_set_background:
 * @pipe: pipe pointer to which background color to be set
 * @background: background color value in BGR 16bpc
 *
 * Sets background color for requested pipe. Color value provided here
 * will be actually submitted at output commit time via "background_color"
 * property.
 * For example to get red as background, set background = 0x00000000FFFF.
 */
void igt_crtc_set_background(igt_pipe_t *pipe, uint64_t background)
{
	igt_display_t *display = pipe->display;

	LOG(display, "%s.%d: crtc_set_background(%"PRIx64")\n",
	    kmstest_pipe_name(pipe->pipe),
	    pipe->pipe, background);

	pipe->background = background;

	pipe->background_changed = true;
}


void igt_wait_for_vblank(int drm_fd, enum pipe pipe)
{
	drmVBlank wait_vbl;
	uint32_t pipe_id_flag;

	memset(&wait_vbl, 0, sizeof(wait_vbl));
	pipe_id_flag = kmstest_get_vbl_flag(pipe);

	wait_vbl.request.type = DRM_VBLANK_RELATIVE;
	wait_vbl.request.type |= pipe_id_flag;
	wait_vbl.request.sequence = 1;

	igt_assert(drmWaitVBlank(drm_fd, &wait_vbl) == 0);
}

/**
 * igt_enable_connectors:
 *
 * Force connectors to be enabled where this is known to work well. Use
 * #igt_reset_connectors to revert the changes.
 *
 * An exit handler is installed to ensure connectors are reset when the test
 * exits.
 */
void igt_enable_connectors(void)
{
	drmModeRes *res;
	int drm_fd;

	drm_fd = drm_open_driver(DRIVER_ANY);

	res = drmModeGetResources(drm_fd);
	igt_assert(res != NULL);

	for (int i = 0; i < res->count_connectors; i++) {
		drmModeConnector *c;

		/* Do a probe. This may be the first action after booting */
		c = drmModeGetConnector(drm_fd, res->connectors[i]);

		/* don't attempt to force connectors that are already connected
		 */
		if (c->connection == DRM_MODE_CONNECTED)
			continue;

		/* just enable VGA for now */
		if (c->connector_type == DRM_MODE_CONNECTOR_VGA) {
			if (!kmstest_force_connector(drm_fd, c, FORCE_CONNECTOR_ON))
				igt_info("Unable to force state on %s-%d\n",
					 kmstest_connector_type_str(c->connector_type),
					 c->connector_type_id);
		}

		drmModeFreeConnector(c);
	}

	close(drm_fd);
}

/**
 * igt_reset_connectors:
 *
 * Remove any forced state from the connectors.
 */
void igt_reset_connectors(void)
{
	/* reset the connectors stored in forced_connectors, avoiding any
	 * functions that are not safe to call in signal handlers */
	for (int i = 0; forced_connectors[i]; i++)
		igt_sysfs_set(forced_connectors_device[i],
			      forced_connectors[i],
			      "detect");
}

/**
 * kmstest_get_vbl_flag:
 * @pipe_id: Pipe to convert to flag representation.
 *
 * Convert a pipe id into the flag representation
 * expected in DRM while processing DRM_IOCTL_WAIT_VBLANK.
 */
uint32_t kmstest_get_vbl_flag(uint32_t pipe_id)
{
	if (pipe_id == 0)
		return 0;
	else if (pipe_id == 1)
		return _DRM_VBLANK_SECONDARY;
	else {
		uint32_t pipe_flag = pipe_id << 1;
		igt_assert(!(pipe_flag & ~DRM_VBLANK_HIGH_CRTC_MASK));
		return pipe_flag;
	}
}
