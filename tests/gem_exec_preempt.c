/*
 * Copyright © 2017 Intel Corporation
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
 *    Michał Winiarski <michal.winiarski@intel.com>
 *
 */

#include "igt.h"

#define MAX_PRIO 1023

#define LOCAL_PARAM_HAS_SCHEDULER 41
#define LOCAL_CONTEXT_PARAM_PRIORITY 6

IGT_TEST_DESCRIPTION("Sanity check for preemption.");

static void ctx_set_priority(int fd, uint32_t ctx, int prio)
{
	struct local_i915_gem_context_param param;

	memset(&param, 0, sizeof(param));
	param.context = ctx;
	param.size = 0;
	param.param = LOCAL_CONTEXT_PARAM_PRIORITY;
	param.value = prio;

	gem_context_set_param(fd, &param);
}

static void noop(int fd, unsigned ring)
{
	uint32_t ctx;
	igt_spin_t *load;
	struct drm_i915_gem_exec_object2 obj;
	struct drm_i915_gem_execbuffer2 execbuf;
	uint32_t *batch;
	int64_t timeout = 1000;

	load = igt_spin_batch_new(fd, ring, 0);

	ctx = gem_context_create(fd);
	ctx_set_priority(fd, ctx, MAX_PRIO);

	memset(&obj, 0, sizeof(obj));
	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(&obj);
	execbuf.buffer_count = 1;
	execbuf.rsvd1 = ctx;

	obj.handle = gem_create(fd, 4096);
	batch = gem_mmap__wc(fd, obj.handle, 0, 4096, PROT_WRITE);
	gem_set_domain(fd, obj.handle,
			I915_GEM_DOMAIN_GTT, I915_GEM_DOMAIN_GTT);
	*batch++ = MI_BATCH_BUFFER_END;

	execbuf.flags = ring;

	gem_execbuf(fd, &execbuf);
	gem_wait(fd, obj.handle, &timeout);

	gem_execbuf(fd, &execbuf);
	gem_wait(fd, obj.handle, 0);

	gem_close(fd, obj.handle);
	gem_context_destroy(fd, ctx);

	igt_spin_batch_end(load);

	igt_spin_batch_free(fd, load);
}

igt_main
{
	const struct intel_execution_engine *e;
	int fd = -1;

	igt_fixture {
		fd = drm_open_driver(DRIVER_INTEL);

		igt_fork_hang_detector(fd);
	}

	for (e = intel_execution_engines; e->name; e++) {
		igt_subtest_f("noop-%s", e->name)
			noop(fd, e->exec_id | e->flags);
	}

	igt_fixture {
		igt_stop_hang_detector();

		close(fd);
	}
}
