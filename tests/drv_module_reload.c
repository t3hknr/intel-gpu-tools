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
 */
#include "igt.h"
#include "igt_debugfs.h"
#include "igt_aux.h"
#include "igt_kmod.h"
#include "igt_sysfs.h"
#include "igt_core.h"

#include <dirent.h>
#include <sys/utsname.h>
#include <linux/limits.h>
#include <signal.h>
#include <libgen.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <fcntl.h>


#define LOCAL_I915_EXEC_BSD_SHIFT      (13)
#define LOCAL_I915_EXEC_BSD_MASK       (3 << LOCAL_I915_EXEC_BSD_SHIFT)

#define ENGINE_MASK  (I915_EXEC_RING_MASK | LOCAL_I915_EXEC_BSD_MASK)

static bool can_store_dword_imm(int fd)
{
	return intel_gen(intel_get_drm_devid(fd)) > 2;
}

static void store_dword(int fd, unsigned ring)
{
	const int gen = intel_gen(intel_get_drm_devid(fd));
	struct drm_i915_gem_exec_object2 obj[2];
	struct drm_i915_gem_relocation_entry reloc;
	struct drm_i915_gem_execbuffer2 execbuf;
	uint32_t batch[16];
	int i;

	if (!can_store_dword_imm(fd))
		return;

	if (!gem_has_ring(fd, ring))
		return;

	if (gen == 6 && (ring & ~(3<<13)) == I915_EXEC_BSD)
		return;

	intel_detect_and_clear_missed_interrupts(fd);
	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = (uintptr_t)obj;
	execbuf.buffer_count = 2;
	execbuf.flags = ring;
	if (gen < 6)
		execbuf.flags |= I915_EXEC_SECURE;

	memset(obj, 0, sizeof(obj));
	obj[0].handle = gem_create(fd, 4096);
	obj[1].handle = gem_create(fd, 4096);

	memset(&reloc, 0, sizeof(reloc));
	reloc.target_handle = obj[0].handle;
	reloc.presumed_offset = 0;
	reloc.offset = sizeof(uint32_t);
	reloc.delta = 0;
	reloc.read_domains = I915_GEM_DOMAIN_INSTRUCTION;
	reloc.write_domain = I915_GEM_DOMAIN_INSTRUCTION;
	obj[1].relocs_ptr = (uintptr_t)&reloc;
	obj[1].relocation_count = 1;

	i = 0;
	batch[i] = MI_STORE_DWORD_IMM | (gen < 6 ? 1 << 22 : 0);
	if (gen >= 8) {
		batch[++i] = 0;
		batch[++i] = 0;
	} else if (gen >= 4) {
		batch[++i] = 0;
		batch[++i] = 0;
		reloc.offset += sizeof(uint32_t);
	} else {
		batch[i]--;
		batch[++i] = 0;
	}
	batch[++i] = 0xc0ffee;
	batch[++i] = MI_BATCH_BUFFER_END;
	gem_write(fd, obj[1].handle, 0, batch, sizeof(batch));
	gem_execbuf(fd, &execbuf);
	gem_close(fd, obj[1].handle);

	gem_read(fd, obj[0].handle, 0, batch, sizeof(batch));
	gem_close(fd, obj[0].handle);
	igt_assert_eq(*batch, 0xc0ffee);
	igt_assert_eq(intel_detect_and_clear_missed_interrupts(fd), 0);
}

static void store_all(int fd)
{
	const int gen = intel_gen(intel_get_drm_devid(fd));
	struct drm_i915_gem_exec_object2 obj[2];
	struct drm_i915_gem_relocation_entry reloc[32];
	struct drm_i915_gem_execbuffer2 execbuf;
	unsigned engines[16], permuted[16];
	uint32_t batch[16];
	uint64_t offset;
	unsigned engine, nengine;
	int value;
	int i, j;

	if (!can_store_dword_imm(fd))
		return;

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = (uintptr_t)obj;
	execbuf.buffer_count = 2;
	if (gen < 6)
		execbuf.flags |= I915_EXEC_SECURE;

	memset(reloc, 0, sizeof(reloc));
	memset(obj, 0, sizeof(obj));
	obj[0].handle = gem_create(fd, 4096);
	obj[1].handle = gem_create(fd, 4096);
	obj[1].relocation_count = 1;

	offset = sizeof(uint32_t);
	i = 0;
	batch[i] = MI_STORE_DWORD_IMM | (gen < 6 ? 1 << 22 : 0);
	if (gen >= 8) {
		batch[++i] = 0;
		batch[++i] = 0;
	} else if (gen >= 4) {
		batch[++i] = 0;
		batch[++i] = 0;
		offset += sizeof(uint32_t);
	} else {
		batch[i]--;
		batch[++i] = 0;
	}
	batch[value = ++i] = 0xc0ffee;
	batch[++i] = MI_BATCH_BUFFER_END;

	nengine = 0;
	intel_detect_and_clear_missed_interrupts(fd);
	for_each_engine(fd, engine) {
		if (gen == 6 && (engine & ~(3<<13)) == I915_EXEC_BSD)
			continue;

		igt_assert(2*(nengine+1)*sizeof(batch) <= 4096);

		execbuf.flags &= ~ENGINE_MASK;
		execbuf.flags |= engine;

		j = 2*nengine;
		reloc[j].target_handle = obj[0].handle;
		reloc[j].presumed_offset = ~0;
		reloc[j].offset = j*sizeof(batch) + offset;
		reloc[j].delta = nengine*sizeof(uint32_t);
		reloc[j].read_domains = I915_GEM_DOMAIN_INSTRUCTION;
		reloc[j].write_domain = I915_GEM_DOMAIN_INSTRUCTION;
		obj[1].relocs_ptr = (uintptr_t)&reloc[j];

		batch[value] = 0xdeadbeef;
		gem_write(fd, obj[1].handle, j*sizeof(batch),
			  batch, sizeof(batch));
		execbuf.batch_start_offset = j*sizeof(batch);
		gem_execbuf(fd, &execbuf);

		j = 2*nengine + 1;
		reloc[j].target_handle = obj[0].handle;
		reloc[j].presumed_offset = ~0;
		reloc[j].offset = j*sizeof(batch) + offset;
		reloc[j].delta = nengine*sizeof(uint32_t);
		reloc[j].read_domains = I915_GEM_DOMAIN_INSTRUCTION;
		reloc[j].write_domain = I915_GEM_DOMAIN_INSTRUCTION;
		obj[1].relocs_ptr = (uintptr_t)&reloc[j];

		batch[value] = nengine;
		gem_write(fd, obj[1].handle, j*sizeof(batch),
			  batch, sizeof(batch));
		execbuf.batch_start_offset = j*sizeof(batch);
		gem_execbuf(fd, &execbuf);

		engines[nengine++] = engine;
	}
	gem_sync(fd, obj[1].handle);

	for (i = 0; i < nengine; i++) {
		obj[1].relocs_ptr = (uintptr_t)&reloc[2*i];
		execbuf.batch_start_offset = 2*i*sizeof(batch);
		memcpy(permuted, engines, nengine*sizeof(engines[0]));
		igt_permute_array(permuted, nengine, igt_exchange_int);
		for (j = 0; j < nengine; j++) {
			execbuf.flags &= ~ENGINE_MASK;
			execbuf.flags |= permuted[j];
			gem_execbuf(fd, &execbuf);
		}
		obj[1].relocs_ptr = (uintptr_t)&reloc[2*i+1];
		execbuf.batch_start_offset = (2*i+1)*sizeof(batch);
		execbuf.flags &= ~ENGINE_MASK;
		execbuf.flags |= engines[i];
		gem_execbuf(fd, &execbuf);
	}
	gem_close(fd, obj[1].handle);

	gem_read(fd, obj[0].handle, 0, engines, sizeof(engines));
	gem_close(fd, obj[0].handle);

	for (i = 0; i < nengine; i++)
		igt_assert_eq_u32(engines[i], i);
	igt_assert_eq(intel_detect_and_clear_missed_interrupts(fd), 0);
}

static int
reload(const char *opts_i915)
{
	int err = IGT_EXIT_SUCCESS;

	if ((err = igt_i915_driver_unload()))
		return err;

	if ((err = igt_i915_driver_load(opts_i915)))
		return err;

	return err;
}

static void
gem_sanitycheck(void)
{
	int err = 0;
	int fd;
	struct drm_i915_gem_caching args = {};


	fd = __drm_open_driver(DRIVER_INTEL);
	igt_set_timeout(1, "Module reload timeout!");

	if (ioctl(fd, DRM_IOCTL_I915_GEM_SET_CACHING, &args) < 0)
		err = -errno;

	igt_set_timeout(0, NULL);
	close(fd);

	igt_assert_eq(err, -ENOENT);
}

static void
gem_exec_store(void)
{
	int fd;
	const struct intel_execution_engine *e;

	fd = __drm_open_driver(DRIVER_INTEL);
	igt_fork_hang_detector(fd);

	for (e = intel_execution_engines; e->name; e++) {
		store_dword(fd, e->exec_id | e->flags);
	}

	store_all(fd);

	igt_stop_hang_detector();
	close(fd);
}

static void
hda_dynamic_debug(bool enable)
{
	FILE *fp;
	const char snd_hda_intel_on[] = "module snd_hda_intel +pf";
	const char snd_hda_core_on[] = "module snd_hda_core +pf";

	const char snd_hda_intel_off[] = "module snd_hda_core =_";
	const char snd_hda_core_off[] = "module snd_hda_intel =_";

	fp = fopen("/sys/kernel/debug/dynamic_debug/control", "w");
	igt_assert(fp != NULL);

	if (enable) {
		fwrite(snd_hda_intel_on, 1, sizeof(snd_hda_intel_on), fp);
		fwrite(snd_hda_core_on, 1, sizeof(snd_hda_core_on), fp);
	} else {
		fwrite(snd_hda_intel_off, 1, sizeof(snd_hda_intel_off), fp);
		fwrite(snd_hda_core_off, 1, sizeof(snd_hda_core_off), fp);
	}

	fclose(fp);
}

igt_main
{
	int i, err;
	char buf[64];

	igt_fixture
		hda_dynamic_debug(true);

	igt_subtest("basic-reload") {

		if ((err = reload(NULL)))
			igt_fail(err);

		gem_sanitycheck();
		gem_exec_store();
	}

	igt_subtest("basic-no-display")
		igt_assert_eq(reload("disable_display=1"), 0);

	igt_subtest("basic-reload-inject") {
		for (i = 0; i < 4; i++) {
			memset(buf, 0, sizeof(buf));
			snprintf(buf, sizeof(buf), "inject_load_failure=%d", i);
			reload(buf);
		}
	}

	igt_subtest("basic-reload-final") {
		if ((err = reload(NULL)))
			igt_fail(err);

		gem_sanitycheck();
		gem_exec_store();
	}

	igt_fixture
		hda_dynamic_debug(false);
}
