/*
 * Copyright © 2009 Intel Corporation
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

/** @file gem_exec_whisper.c
 *
 * Pass around a value to write into a scratch buffer between lots of batches
 */

#include "igt.h"
#include "igt_gt.h"
#include "igt_sysfs.h"

#define LOCAL_I915_EXEC_NO_RELOC (1<<11)
#define LOCAL_I915_EXEC_HANDLE_LUT (1<<12)

#define LOCAL_I915_EXEC_BSD_SHIFT      (13)
#define LOCAL_I915_EXEC_BSD_MASK       (3 << LOCAL_I915_EXEC_BSD_SHIFT)

#define ENGINE_MASK  (I915_EXEC_RING_MASK | LOCAL_I915_EXEC_BSD_MASK)

#define VERIFY 0

static void write_seqno(unsigned offset)
{
	uint32_t seqno = UINT32_MAX - offset;
	FILE *file;

	file = igt_debugfs_fopen("i915_next_seqno", "w");
	igt_assert(file);

	igt_assert(fprintf(file, "0x%x", seqno) > 0);
	fclose(file);

	igt_debug("next seqno set to: 0x%x\n", seqno);
}

static void check_bo(int fd, uint32_t handle)
{
	uint32_t *map;
	int i;

	igt_debug("Verifying result\n");
	map = gem_mmap__cpu(fd, handle, 0, 4096, PROT_READ);
	gem_set_domain(fd, handle, I915_GEM_DOMAIN_CPU, 0);
	for (i = 0; i < 1024; i++)
		igt_assert_eq(map[i], i);
	munmap(map, 4096);
}

static void verify_reloc(int fd, uint32_t handle,
			 const struct drm_i915_gem_relocation_entry *reloc)
{
	if (VERIFY) {
		uint64_t target = 0;
		if (intel_gen(intel_get_drm_devid(fd)) >= 8)
			gem_read(fd, handle, reloc->offset, &target, 8);
		else
			gem_read(fd, handle, reloc->offset, &target, 4);
		igt_assert_eq_u64(target,
				  reloc->presumed_offset + reloc->delta);
	}
}

static int __gem_context_create(int fd, uint32_t *ctx_id)
{
	struct drm_i915_gem_context_create arg;
	int ret = 0;

	memset(&arg, 0, sizeof(arg));
	if (drmIoctl(fd, DRM_IOCTL_I915_GEM_CONTEXT_CREATE, &arg))
		ret = -errno;

	*ctx_id = arg.ctx_id;
	return ret;
}

static bool can_mi_store_dword(int gen, unsigned engine)
{
	return !(gen == 6 && (engine & ~(3<<13)) == I915_EXEC_BSD);
}

static bool ignore_engine(int gen, unsigned engine)
{
	if (engine == 0)
		return true;

	if (!can_mi_store_dword(gen, engine))
		return true;

	return false;
}

#define CONTEXTS 0x1
#define FDS 0x2
#define INTERRUPTIBLE 0x4
#define CHAIN 0x8
#define FORKED 0x10
#define HANG 0x20

struct hang {
	struct drm_i915_gem_exec_object2 obj;
	struct drm_i915_gem_relocation_entry reloc;
	struct drm_i915_gem_execbuffer2 execbuf;
	int fd;
};

static void init_hang(struct hang *h)
{
	uint32_t *batch;
	int i, gen;

	h->fd = drm_open_driver(DRIVER_INTEL);
	igt_allow_hang(h->fd, 0, 0);

	gen = intel_gen(intel_get_drm_devid(h->fd));

	memset(&h->execbuf, 0, sizeof(h->execbuf));
	h->execbuf.buffers_ptr = to_user_pointer(&h->obj);
	h->execbuf.buffer_count = 1;

	memset(&h->obj, 0, sizeof(h->obj));
	h->obj.handle = gem_create(h->fd, 4096);

	h->obj.relocs_ptr = to_user_pointer(&h->reloc);
	h->obj.relocation_count = 1;
	memset(&h->reloc, 0, sizeof(h->reloc));

	batch = gem_mmap__cpu(h->fd, h->obj.handle, 0, 4096, PROT_WRITE);
	gem_set_domain(h->fd, h->obj.handle,
		       I915_GEM_DOMAIN_CPU, I915_GEM_DOMAIN_CPU);

	h->reloc.target_handle = h->obj.handle; /* recurse */
	h->reloc.presumed_offset = 0;
	h->reloc.offset = 5*sizeof(uint32_t);
	h->reloc.delta = 0;
	h->reloc.read_domains = I915_GEM_DOMAIN_COMMAND;
	h->reloc.write_domain = 0;

	i = 0;
	batch[i++] = 0xffffffff;
	batch[i++] = 0xdeadbeef;
	batch[i++] = 0xc00fee00;
	batch[i++] = 0x00c00fee;
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
			h->reloc.delta = 1;
		}
	}
	munmap(batch, 4096);
}

static void submit_hang(struct hang *h, unsigned *engines, int nengine)
{
	while (nengine--) {
		h->execbuf.flags &= ~ENGINE_MASK;
		h->execbuf.flags |= *engines++;
		gem_execbuf(h->fd, &h->execbuf);
	}
}

static void fini_hang(struct hang *h)
{
	close(h->fd);
}

static void whisper(int fd, unsigned engine, unsigned flags)
{
	const uint32_t bbe = MI_BATCH_BUFFER_END;
	const int gen = intel_gen(intel_get_drm_devid(fd));
	struct drm_i915_gem_exec_object2 batches[1024];
	struct drm_i915_gem_relocation_entry inter[1024];
	struct drm_i915_gem_relocation_entry reloc;
	struct drm_i915_gem_exec_object2 store, scratch;
	struct drm_i915_gem_exec_object2 tmp[2];
	struct drm_i915_gem_execbuffer2 execbuf;
	struct hang hang;
	int fds[64];
	uint32_t contexts[64];
	unsigned engines[16];
	unsigned nengine;
	uint32_t batch[16];
	int i, n, pass, loc;
	unsigned int relocations = 0;
	unsigned int reloc_migrations = 0;
	unsigned int reloc_interruptions = 0;
	unsigned int eb_migrations = 0;
	uint64_t old_offset;

	nengine = 0;
	if (engine == -1) {
		for_each_engine(fd, engine) {
			if (!ignore_engine(gen, engine))
				engines[nengine++] = engine;
		}
	} else {
		igt_require(gem_has_ring(fd, engine));
		igt_require(can_mi_store_dword(gen, engine));
		engines[nengine++] = engine;
	}
	igt_require(nengine);

	if (flags & HANG)
		init_hang(&hang);

	intel_detect_and_clear_missed_interrupts(fd);
	igt_fork(child, flags & FORKED ? sysconf(_SC_NPROCESSORS_ONLN) : 1)  {
		memset(&scratch, 0, sizeof(scratch));
		scratch.handle = gem_create(fd, 4096);
		scratch.flags = EXEC_OBJECT_WRITE;

		memset(&store, 0, sizeof(store));
		store.handle = gem_create(fd, 4096);
		store.relocs_ptr = to_user_pointer(&reloc);
		store.relocation_count = 1;

		memset(&reloc, 0, sizeof(reloc));
		reloc.offset = sizeof(uint32_t);
		if (gen < 8 && gen >= 4)
			reloc.offset += sizeof(uint32_t);
		loc = 8;
		if (gen >= 4)
			loc += 4;
		reloc.read_domains = I915_GEM_DOMAIN_INSTRUCTION;
		reloc.write_domain = I915_GEM_DOMAIN_INSTRUCTION;

		{
			tmp[0] = scratch;
			tmp[1] = store;
			gem_write(fd, store.handle, 0, &bbe, sizeof(bbe));

			memset(&execbuf, 0, sizeof(execbuf));
			execbuf.buffers_ptr = to_user_pointer(tmp);
			execbuf.buffer_count = 2;
			execbuf.flags = LOCAL_I915_EXEC_HANDLE_LUT;
			execbuf.flags |= LOCAL_I915_EXEC_NO_RELOC;
			if (gen < 6)
				execbuf.flags |= I915_EXEC_SECURE;
			igt_require(__gem_execbuf(fd, &execbuf) == 0);
			scratch = tmp[0];
			store = tmp[1];
		}

		i = 0;
		batch[i] = MI_STORE_DWORD_IMM | (gen < 6 ? 1 << 22 : 0);
		if (gen >= 8) {
			batch[++i] = store.offset + loc;
			batch[++i] = (store.offset + loc) >> 32;
		} else if (gen >= 4) {
			batch[++i] = 0;
			batch[++i] = store.offset + loc;
		} else {
			batch[i]--;
			batch[++i] = store.offset + loc;
		}
		batch[++i] = 0xc0ffee;
		igt_assert(loc == sizeof(uint32_t) * i);
		batch[++i] = MI_BATCH_BUFFER_END;

		if (flags & CONTEXTS) {
			igt_require(__gem_context_create(fd, &contexts[0]) == 0);
			for (n = 1; n < 64; n++)
				contexts[n] = gem_context_create(fd);
		}
		if (flags & FDS) {
			igt_require(gen >= 6);
			for (n = 0; n < 64; n++)
				fds[n] = drm_open_driver(DRIVER_INTEL);
		}

		memset(batches, 0, sizeof(batches));
		for (n = 0; n < 1024; n++) {
			batches[n].handle = gem_create(fd, 4096);
			gem_write(fd, batches[n].handle, 0, &bbe, sizeof(bbe));
		}
		execbuf.buffers_ptr = to_user_pointer(batches);
		execbuf.buffer_count = 1024;
		gem_execbuf(fd, &execbuf);

		execbuf.buffers_ptr = to_user_pointer(tmp);
		execbuf.buffer_count = 2;

		old_offset = store.offset;
		for (n = 0; n < 1024; n++) {
			if (gen >= 8) {
				batch[1] = old_offset + loc;
				batch[2] = (old_offset + loc) >> 32;
			} else if (gen >= 4) {
				batch[2] = old_offset + loc;
			} else {
				batch[1] = old_offset + loc;
			}

			inter[n] = reloc;
			inter[n].presumed_offset = old_offset;
			inter[n].delta = loc;
			batches[n].relocs_ptr = to_user_pointer(&inter[n]);
			batches[n].relocation_count = 1;
			gem_write(fd, batches[n].handle, 0, batch, sizeof(batch));

			old_offset = batches[n].offset;
		}

		igt_while_interruptible(flags & INTERRUPTIBLE) {
			for (pass = 0; pass < 1024; pass++) {
				uint64_t offset;

				if (!(flags & FORKED))
					write_seqno(pass);

				if (flags & HANG)
					submit_hang(&hang, engines, nengine);

				if (flags & CHAIN) {
					execbuf.flags &= ~ENGINE_MASK;
					execbuf.flags |= engines[rand() % nengine];
				}

				reloc.presumed_offset = scratch.offset;
				reloc.delta = 4*pass;
				offset = reloc.presumed_offset + reloc.delta;

				i = 0;
				if (gen >= 8) {
					batch[++i] = offset;
					batch[++i] = offset >> 32;
				} else if (gen >= 4) {
					batch[++i] = 0;
					batch[++i] = offset;
				} else {
					batch[++i] = offset;
				}
				batch[++i] = ~pass;
				gem_write(fd, store.handle, 0, batch, sizeof(batch));

				tmp[0] = scratch;
				igt_assert(tmp[0].flags & EXEC_OBJECT_WRITE);
				tmp[1] = store;
				verify_reloc(fd, store.handle, &reloc);
				execbuf.buffers_ptr = to_user_pointer(tmp);
				gem_execbuf(fd, &execbuf);
				igt_assert_eq_u64(reloc.presumed_offset, tmp[0].offset);
				scratch = tmp[0];

				gem_write(fd, batches[1023].handle, loc, &pass, sizeof(pass));
				for (n = 1024; --n >= 1; ) {
					int this_fd = fd;
					uint32_t handle[2];

					execbuf.buffers_ptr = to_user_pointer(&batches[n-1]);
					reloc_migrations += batches[n-1].offset != inter[n].presumed_offset;
					batches[n-1].offset = inter[n].presumed_offset;
					old_offset = inter[n].presumed_offset;
					batches[n-1].relocation_count = 0;
					batches[n-1].flags |= EXEC_OBJECT_WRITE;
					verify_reloc(fd, batches[n].handle, &inter[n]);

					if (flags & FDS) {
						this_fd = fds[rand() % 64];
						handle[0] = batches[n-1].handle;
						handle[1] = batches[n].handle;
						batches[n-1].handle =
							gem_open(this_fd,
									gem_flink(fd, handle[0]));
						batches[n].handle =
							gem_open(this_fd,
									gem_flink(fd, handle[1]));
					}

					if (!(flags & CHAIN)) {
						execbuf.flags &= ~ENGINE_MASK;
						execbuf.flags |= engines[rand() % nengine];
					}
					if (flags & CONTEXTS)
						execbuf.rsvd1 = contexts[rand() % 64];
					gem_execbuf(this_fd, &execbuf);
					if (inter[n].presumed_offset == -1) {
						reloc_interruptions++;
						inter[n].presumed_offset = batches[n-1].offset;
					}
					igt_assert_eq_u64(inter[n].presumed_offset, batches[n-1].offset);
					relocations += inter[n].presumed_offset != old_offset;

					batches[n-1].relocation_count = 1;
					batches[n-1].flags &= ~EXEC_OBJECT_WRITE;

					if (this_fd != fd) {
						gem_close(this_fd, batches[n-1].handle);
						batches[n-1].handle = handle[0];

						gem_close(this_fd, batches[n].handle);
						batches[n].handle = handle[1];
					}
				}
				execbuf.flags &= ~ENGINE_MASK;
				execbuf.rsvd1 = 0;
				execbuf.buffers_ptr = to_user_pointer(&tmp);

				tmp[0] = tmp[1];
				tmp[0].relocation_count = 0;
				tmp[0].flags = EXEC_OBJECT_WRITE;
				reloc_migrations += tmp[0].offset != inter[0].presumed_offset;
				tmp[0].offset = inter[0].presumed_offset;
				old_offset = tmp[0].offset;
				tmp[1] = batches[0];
				verify_reloc(fd, batches[0].handle, &inter[0]);
				gem_execbuf(fd, &execbuf);
				if (inter[0].presumed_offset == -1) {
					reloc_interruptions++;
					inter[0].presumed_offset = tmp[0].offset;
				}
				igt_assert_eq_u64(inter[0].presumed_offset, tmp[0].offset);
				relocations += inter[0].presumed_offset != old_offset;
				batches[0] = tmp[1];

				tmp[1] = tmp[0];
				tmp[0] = scratch;
				igt_assert(tmp[0].flags & EXEC_OBJECT_WRITE);
				igt_assert_eq_u64(reloc.presumed_offset, tmp[0].offset);
				igt_assert(tmp[1].relocs_ptr == to_user_pointer(&reloc));
				tmp[1].relocation_count = 1;
				tmp[1].flags &= ~EXEC_OBJECT_WRITE;
				verify_reloc(fd, store.handle, &reloc);
				gem_execbuf(fd, &execbuf);
				eb_migrations += tmp[0].offset != scratch.offset;
				eb_migrations += tmp[1].offset != store.offset;
				igt_assert_eq_u64(reloc.presumed_offset, tmp[0].offset);
				store = tmp[1];
				scratch = tmp[0];
			}
		}
		igt_info("Number of migrations for execbuf: %d\n", eb_migrations);
		igt_info("Number of migrations for reloc: %d, interrupted %d, patched %d\n", reloc_migrations, reloc_interruptions, relocations);

		check_bo(fd, scratch.handle);
		gem_close(fd, scratch.handle);
		gem_close(fd, store.handle);

		if (flags & FDS) {
			for (n = 0; n < 64; n++)
				close(fds[n]);
		}
		if (flags & CONTEXTS) {
			for (n = 0; n < 64; n++)
				gem_context_destroy(fd, contexts[n]);
		}
		for (n = 0; n < 1024; n++)
			gem_close(fd, batches[n].handle);
	}

	igt_waitchildren();

	if (flags & HANG)
		fini_hang(&hang);
	else
		igt_assert_eq(intel_detect_and_clear_missed_interrupts(fd), 0);
}

static void print_welcome(int fd)
{
	bool active;
	int dir;

	dir = igt_sysfs_open_parameters(fd);
	if (dir < 0)
		return;

	active = igt_sysfs_get_boolean(dir, "enable_guc_submission");
	if (active) {
		igt_info("Using GuC submission\n");
		goto out;
	}

	active = igt_sysfs_get_boolean(dir, "enable_execlists");
	if (active) {
		igt_info("Using Execlists submission\n");
		goto out;
	}

	active = igt_sysfs_get_boolean(dir, "semaphores");
	igt_info("Using Legacy submission%s\n",
		 active ? ", with semaphores" : "");

out:
	close(dir);
}

igt_main
{
	const struct mode {
		const char *name;
		unsigned flags;
	} modes[] = {
		{ "normal", 0 },
		{ "interruptible", INTERRUPTIBLE },
		{ "forked", FORKED },
		{ "chain", CHAIN },
		{ "chain-forked", CHAIN | FORKED },
		{ "chain-interruptible", CHAIN | INTERRUPTIBLE },
		{ "contexts", CONTEXTS },
		{ "contexts-interruptible", CONTEXTS | INTERRUPTIBLE},
		{ "contexts-forked", CONTEXTS | FORKED},
		{ "contexts-chain", CONTEXTS | CHAIN },
		{ "fds", FDS },
		{ "fds-interruptible", FDS | INTERRUPTIBLE},
		{ "fds-forked", FDS | FORKED},
		{ "fds-chain", FDS | CHAIN},
		{ NULL }
	};
	int fd;

	igt_fixture {
		fd = drm_open_driver_master(DRIVER_INTEL);
		print_welcome(fd);

		igt_fork_hang_detector(fd);
	}

	for (const struct mode *m = modes; m->name; m++)
		igt_subtest_f("%s", m->name)
			whisper(fd, -1, m->flags);

	for (const struct intel_execution_engine *e = intel_execution_engines;
	     e->name; e++) {
		for (const struct mode *m = modes; m->name; m++) {
			if (m->flags & CHAIN)
				continue;

			igt_subtest_f("%s-%s", e->name, m->name)
				whisper(fd, e->exec_id | e->flags, m->flags);
		}
	}

	igt_fixture {
		igt_stop_hang_detector();
	}

	igt_subtest_group {
		for (const struct mode *m = modes; m->name; m++) {
			if (m->flags & INTERRUPTIBLE)
				continue;
			igt_subtest_f("hang-%s", m->name)
				whisper(fd, -1, m->flags | HANG);
		}
	}

	igt_fixture {
		close(fd);
	}
}
