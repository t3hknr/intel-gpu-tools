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

#include "config.h"

#include <sys/poll.h>
#include <sys/ioctl.h>
#include <sched.h>
#include <signal.h>

#include "igt.h"
#include "igt_vgem.h"
#include "igt_rand.h"
#include "igt_sysfs.h"
#include "i915/gem_scheduler.h"

#define LO 0
#define HI 1
#define NOISE 2

#define MAX_PRIO LOCAL_I915_CONTEXT_MAX_USER_PRIORITY
#define MIN_PRIO LOCAL_I915_CONTEXT_MIN_USER_PRIORITY

#define BUSY_QLEN 8

IGT_TEST_DESCRIPTION("Check that we can control the order of execution");

static void store_dword(int fd, uint32_t ctx, unsigned ring,
			uint32_t target, uint32_t offset, uint32_t value,
			uint32_t cork, unsigned write_domain)
{
	const int gen = intel_gen(intel_get_drm_devid(fd));
	struct drm_i915_gem_exec_object2 obj[3];
	struct drm_i915_gem_relocation_entry reloc;
	struct drm_i915_gem_execbuffer2 execbuf;
	uint32_t batch[16];
	int i;

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(obj + !cork);
	execbuf.buffer_count = 2 + !!cork;
	execbuf.flags = ring;
	if (gen < 6)
		execbuf.flags |= I915_EXEC_SECURE;
	execbuf.rsvd1 = ctx;

	memset(obj, 0, sizeof(obj));
	obj[0].handle = cork;
	obj[1].handle = target;
	obj[2].handle = gem_create(fd, 4096);

	memset(&reloc, 0, sizeof(reloc));
	reloc.target_handle = obj[1].handle;
	reloc.presumed_offset = 0;
	reloc.offset = sizeof(uint32_t);
	reloc.delta = offset;
	reloc.read_domains = I915_GEM_DOMAIN_INSTRUCTION;
	reloc.write_domain = write_domain;
	obj[2].relocs_ptr = to_user_pointer(&reloc);
	obj[2].relocation_count = 1;

	i = 0;
	batch[i] = MI_STORE_DWORD_IMM | (gen < 6 ? 1 << 22 : 0);
	if (gen >= 8) {
		batch[++i] = offset;
		batch[++i] = 0;
	} else if (gen >= 4) {
		batch[++i] = 0;
		batch[++i] = offset;
		reloc.offset += sizeof(uint32_t);
	} else {
		batch[i]--;
		batch[++i] = offset;
	}
	batch[++i] = value;
	batch[++i] = MI_BATCH_BUFFER_END;
	gem_write(fd, obj[2].handle, 0, batch, sizeof(batch));
	gem_execbuf(fd, &execbuf);
	gem_close(fd, obj[2].handle);
}

struct cork {
	int device;
	uint32_t handle;
	uint32_t fence;
};

static void plug(int fd, struct cork *c)
{
	struct vgem_bo bo;
	int dmabuf;

	c->device = drm_open_driver(DRIVER_VGEM);

	bo.width = bo.height = 1;
	bo.bpp = 4;
	vgem_create(c->device, &bo);
	c->fence = vgem_fence_attach(c->device, &bo, VGEM_FENCE_WRITE);

	dmabuf = prime_handle_to_fd(c->device, bo.handle);
	c->handle = prime_fd_to_handle(fd, dmabuf);
	close(dmabuf);
}

static void unplug(struct cork *c)
{
	vgem_fence_signal(c->device, c->fence);
	close(c->device);
}

static uint32_t create_highest_priority(int fd)
{
	uint32_t ctx = gem_context_create(fd);

	/*
	 * If there is no priority support, all contexts will have equal
	 * priority (and therefore the max user priority), so no context
	 * can overtake us, and we effectively can form a plug.
	 */
	__gem_context_set_priority(fd, ctx, MAX_PRIO);

	return ctx;
}

static void unplug_show_queue(int fd, struct cork *c, unsigned int engine)
{
	igt_spin_t *spin[BUSY_QLEN];

	for (int n = 0; n < ARRAY_SIZE(spin); n++) {
		uint32_t ctx = create_highest_priority(fd);
		spin[n] = __igt_spin_batch_new(fd, ctx, engine, 0);
		gem_context_destroy(fd, ctx);
	}

	unplug(c); /* batches will now be queued on the engine */
	igt_debugfs_dump(fd, "i915_engine_info");

	for (int n = 0; n < ARRAY_SIZE(spin); n++)
		igt_spin_batch_free(fd, spin[n]);

}

static void fifo(int fd, unsigned ring)
{
	struct cork cork;
	uint32_t scratch;
	uint32_t *ptr;

	scratch = gem_create(fd, 4096);

	plug(fd, &cork);

	/* Same priority, same timeline, final result will be the second eb */
	store_dword(fd, 0, ring, scratch, 0, 1, cork.handle, 0);
	store_dword(fd, 0, ring, scratch, 0, 2, cork.handle, 0);

	unplug_show_queue(fd, &cork, ring);

	ptr = gem_mmap__gtt(fd, scratch, 4096, PROT_READ);
	gem_set_domain(fd, scratch, /* no write hazard lies! */
			I915_GEM_DOMAIN_GTT, I915_GEM_DOMAIN_GTT);
	gem_close(fd, scratch);

	igt_assert_eq_u32(ptr[0], 2);
	munmap(ptr, 4096);
}

static bool ignore_engine(int fd, unsigned engine)
{
	if (engine == 0)
		return true;

	if (gem_has_bsd2(fd) && engine == I915_EXEC_BSD)
		return true;

	return false;
}

static void smoketest(int fd, unsigned ring, unsigned timeout)
{
	const int ncpus = sysconf(_SC_NPROCESSORS_ONLN);
	unsigned engines[16];
	unsigned nengine;
	unsigned engine;
	uint32_t scratch;
	uint32_t *ptr;

	nengine = 0;
	for_each_engine(fd, engine) {
		if (ignore_engine(fd, engine))
			continue;

		engines[nengine++] = engine;
	}
	igt_require(nengine);

	scratch = gem_create(fd, 4096);
	igt_fork(child, ncpus) {
		unsigned long count = 0;
		uint32_t ctx;

		hars_petruska_f54_1_random_perturb(child);

		ctx = gem_context_create(fd);
		igt_until_timeout(timeout) {
			int prio;

			prio = hars_petruska_f54_1_random_unsafe_max(MAX_PRIO - MIN_PRIO) + MIN_PRIO;
			gem_context_set_priority(fd, ctx, prio);

			engine = engines[hars_petruska_f54_1_random_unsafe_max(nengine)];
			store_dword(fd, ctx, engine, scratch,
				    8*child + 0, ~child,
				    0, 0);
			for (unsigned int step = 0; step < 8; step++)
				store_dword(fd, ctx, engine, scratch,
					    8*child + 4, count++,
					    0, 0);
		}
		gem_context_destroy(fd, ctx);
	}
	igt_waitchildren();

	ptr = gem_mmap__gtt(fd, scratch, 4096, PROT_READ);
	gem_set_domain(fd, scratch, /* no write hazard lies! */
			I915_GEM_DOMAIN_GTT, I915_GEM_DOMAIN_GTT);
	gem_close(fd, scratch);

	for (unsigned n = 0; n < ncpus; n++) {
		igt_assert_eq_u32(ptr[2*n], ~n);
		/*
		 * Note this count is approximate due to unconstrained
		 * ordering of the dword writes between engines.
		 *
		 * Take the result with a pinch of salt.
		 */
		igt_info("Child[%d] completed %u cycles\n",  n, ptr[2*n+1]);
	}
	munmap(ptr, 4096);
}

static void reorder(int fd, unsigned ring, unsigned flags)
#define EQUAL 1
{
	struct cork cork;
	uint32_t scratch;
	uint32_t *ptr;
	uint32_t ctx[2];

	ctx[LO] = gem_context_create(fd);
	gem_context_set_priority(fd, ctx[LO], MIN_PRIO);

	ctx[HI] = gem_context_create(fd);
	gem_context_set_priority(fd, ctx[HI], flags & EQUAL ? MIN_PRIO : 0);

	scratch = gem_create(fd, 4096);
	plug(fd, &cork);

	/* We expect the high priority context to be executed first, and
	 * so the final result will be value from the low priority context.
	 */
	store_dword(fd, ctx[LO], ring, scratch, 0, ctx[LO], cork.handle, 0);
	store_dword(fd, ctx[HI], ring, scratch, 0, ctx[HI], cork.handle, 0);

	unplug_show_queue(fd, &cork, ring);

	gem_context_destroy(fd, ctx[LO]);
	gem_context_destroy(fd, ctx[HI]);

	ptr = gem_mmap__gtt(fd, scratch, 4096, PROT_READ);
	gem_set_domain(fd, scratch, /* no write hazard lies! */
			I915_GEM_DOMAIN_GTT, I915_GEM_DOMAIN_GTT);
	gem_close(fd, scratch);

	if (flags & EQUAL) /* equal priority, result will be fifo */
		igt_assert_eq_u32(ptr[0], ctx[HI]);
	else
		igt_assert_eq_u32(ptr[0], ctx[LO]);
	munmap(ptr, 4096);
}

static void promotion(int fd, unsigned ring)
{
	struct cork cork;
	uint32_t result, dep;
	uint32_t *ptr;
	uint32_t ctx[3];

	ctx[LO] = gem_context_create(fd);
	gem_context_set_priority(fd, ctx[LO], MIN_PRIO);

	ctx[HI] = gem_context_create(fd);
	gem_context_set_priority(fd, ctx[HI], 0);

	ctx[NOISE] = gem_context_create(fd);
	gem_context_set_priority(fd, ctx[NOISE], MIN_PRIO/2);

	result = gem_create(fd, 4096);
	dep = gem_create(fd, 4096);

	plug(fd, &cork);

	/* Expect that HI promotes LO, so the order will be LO, HI, NOISE.
	 *
	 * fifo would be NOISE, LO, HI.
	 * strict priority would be  HI, NOISE, LO
	 */
	store_dword(fd, ctx[NOISE], ring, result, 0, ctx[NOISE], cork.handle, 0);
	store_dword(fd, ctx[LO], ring, result, 0, ctx[LO], cork.handle, 0);

	/* link LO <-> HI via a dependency on another buffer */
	store_dword(fd, ctx[LO], ring, dep, 0, ctx[LO], 0, I915_GEM_DOMAIN_INSTRUCTION);
	store_dword(fd, ctx[HI], ring, dep, 0, ctx[HI], 0, 0);

	store_dword(fd, ctx[HI], ring, result, 0, ctx[HI], 0, 0);

	unplug_show_queue(fd, &cork, ring);

	gem_context_destroy(fd, ctx[NOISE]);
	gem_context_destroy(fd, ctx[LO]);
	gem_context_destroy(fd, ctx[HI]);

	ptr = gem_mmap__gtt(fd, dep, 4096, PROT_READ);
	gem_set_domain(fd, dep, /* no write hazard lies! */
			I915_GEM_DOMAIN_GTT, I915_GEM_DOMAIN_GTT);
	gem_close(fd, dep);

	igt_assert_eq_u32(ptr[0], ctx[HI]);
	munmap(ptr, 4096);

	ptr = gem_mmap__gtt(fd, result, 4096, PROT_READ);
	gem_set_domain(fd, result, /* no write hazard lies! */
			I915_GEM_DOMAIN_GTT, I915_GEM_DOMAIN_GTT);
	gem_close(fd, result);

	igt_assert_eq_u32(ptr[0], ctx[NOISE]);
	munmap(ptr, 4096);
}

#define NEW_CTX 0x1
static void preempt(int fd, unsigned ring, unsigned flags)
{
	uint32_t result = gem_create(fd, 4096);
	uint32_t *ptr = gem_mmap__gtt(fd, result, 4096, PROT_READ);
	igt_spin_t *spin[16];
	uint32_t ctx[2];

	ctx[LO] = gem_context_create(fd);
	gem_context_set_priority(fd, ctx[LO], MIN_PRIO);

	ctx[HI] = gem_context_create(fd);
	gem_context_set_priority(fd, ctx[HI], MAX_PRIO);

	for (int n = 0; n < 16; n++) {
		if (flags & NEW_CTX) {
			gem_context_destroy(fd, ctx[LO]);
			ctx[LO] = gem_context_create(fd);
			gem_context_set_priority(fd, ctx[LO], MIN_PRIO);
		}
		spin[n] = __igt_spin_batch_new(fd, ctx[LO], ring, 0);
		igt_debug("spin[%d].handle=%d\n", n, spin[n]->handle);

		store_dword(fd, ctx[HI], ring, result, 0, n + 1, 0, I915_GEM_DOMAIN_RENDER);

		gem_set_domain(fd, result, I915_GEM_DOMAIN_GTT, 0);
		igt_assert_eq_u32(ptr[0], n + 1);
		igt_assert(gem_bo_busy(fd, spin[0]->handle));
	}

	for (int n = 0; n < 16; n++)
		igt_spin_batch_free(fd, spin[n]);

	gem_context_destroy(fd, ctx[LO]);
	gem_context_destroy(fd, ctx[HI]);

	munmap(ptr, 4096);
	gem_close(fd, result);
}

static void preempt_other(int fd, unsigned ring)
{
	uint32_t result = gem_create(fd, 4096);
	uint32_t *ptr = gem_mmap__gtt(fd, result, 4096, PROT_READ);
	igt_spin_t *spin[16];
	unsigned int other;
	unsigned int n, i;
	uint32_t ctx[3];

	/* On each engine, insert
	 * [NOISE] spinner,
	 * [LOW] write
	 *
	 * Then on our target engine do a [HIGH] write which should then
	 * prompt its dependent LOW writes in front of the spinner on
	 * each engine. The purpose of this test is to check that preemption
	 * can cross engines.
	 */

	ctx[LO] = gem_context_create(fd);
	gem_context_set_priority(fd, ctx[LO], MIN_PRIO);

	ctx[NOISE] = gem_context_create(fd);

	ctx[HI] = gem_context_create(fd);
	gem_context_set_priority(fd, ctx[HI], MAX_PRIO);

	n = 0;
	for_each_engine(fd, other) {
		spin[n] = __igt_spin_batch_new(fd, ctx[NOISE], other, 0);
		store_dword(fd, ctx[LO], other,
			    result, (n + 1)*sizeof(uint32_t), n + 1,
			    0, I915_GEM_DOMAIN_RENDER);
		n++;
	}
	store_dword(fd, ctx[HI], ring,
		    result, (n + 1)*sizeof(uint32_t), n + 1,
		    0, I915_GEM_DOMAIN_RENDER);

	gem_set_domain(fd, result, I915_GEM_DOMAIN_GTT, 0);

	for (i = 0; i < n; i++) {
		igt_assert(gem_bo_busy(fd, spin[i]->handle));
		igt_spin_batch_free(fd, spin[i]);
	}

	n++;
	for (i = 0; i <= n; i++)
		igt_assert_eq_u32(ptr[i], i);

	gem_context_destroy(fd, ctx[LO]);
	gem_context_destroy(fd, ctx[NOISE]);
	gem_context_destroy(fd, ctx[HI]);

	munmap(ptr, 4096);
	gem_close(fd, result);
}

static void preempt_self(int fd, unsigned ring)
{
	uint32_t result = gem_create(fd, 4096);
	uint32_t *ptr = gem_mmap__gtt(fd, result, 4096, PROT_READ);
	igt_spin_t *spin[16];
	unsigned int other;
	unsigned int n, i;
	uint32_t ctx[3];

	/* On each engine, insert
	 * [NOISE] spinner,
	 * [self/LOW] write
	 *
	 * Then on our target engine do a [self/HIGH] write which should then
	 * preempt its own lower priority task on any engine.
	 */

	ctx[NOISE] = gem_context_create(fd);

	ctx[HI] = gem_context_create(fd);

	n = 0;
	gem_context_set_priority(fd, ctx[HI], MIN_PRIO);
	for_each_engine(fd, other) {
		spin[n] = __igt_spin_batch_new(fd, ctx[NOISE], other, 0);
		store_dword(fd, ctx[HI], other,
			    result, (n + 1)*sizeof(uint32_t), n + 1,
			    0, I915_GEM_DOMAIN_RENDER);
		n++;
	}
	gem_context_set_priority(fd, ctx[HI], MAX_PRIO);
	store_dword(fd, ctx[HI], ring,
		    result, (n + 1)*sizeof(uint32_t), n + 1,
		    0, I915_GEM_DOMAIN_RENDER);

	gem_set_domain(fd, result, I915_GEM_DOMAIN_GTT, 0);

	for (i = 0; i < n; i++) {
		igt_assert(gem_bo_busy(fd, spin[i]->handle));
		igt_spin_batch_free(fd, spin[i]);
	}

	n++;
	for (i = 0; i <= n; i++)
		igt_assert_eq_u32(ptr[i], i);

	gem_context_destroy(fd, ctx[NOISE]);
	gem_context_destroy(fd, ctx[HI]);

	munmap(ptr, 4096);
	gem_close(fd, result);
}

static void deep(int fd, unsigned ring)
{
#define XS 8
	const unsigned int nctx = MAX_PRIO - MIN_PRIO;
	const unsigned size = ALIGN(4*nctx, 4096);
	struct timespec tv = {};
	struct cork cork;
	uint32_t result, dep[XS];
	uint32_t expected = 0;
	uint32_t *ptr;
	uint32_t *ctx;

	ctx = malloc(sizeof(*ctx) * nctx);
	for (int n = 0; n < nctx; n++) {
		ctx[n] = gem_context_create(fd);
		gem_context_set_priority(fd, ctx[n], MAX_PRIO - nctx + n);
	}

	result = gem_create(fd, size);
	for (int m = 0; m < XS; m ++)
		dep[m] = gem_create(fd, size);

	/* Bind all surfaces and contexts before starting the timeout. */
	{
		struct drm_i915_gem_exec_object2 obj[XS + 2];
		struct drm_i915_gem_execbuffer2 execbuf;
		const uint32_t bbe = MI_BATCH_BUFFER_END;

		memset(obj, 0, sizeof(obj));
		for (int n = 0; n < XS; n++)
			obj[n].handle = dep[n];
		obj[XS].handle = result;
		obj[XS+1].handle = gem_create(fd, 4096);
		gem_write(fd, obj[XS+1].handle, 0, &bbe, sizeof(bbe));

		memset(&execbuf, 0, sizeof(execbuf));
		execbuf.buffers_ptr = to_user_pointer(obj);
		execbuf.buffer_count = XS + 2;
		execbuf.flags = ring;
		for (int n = 0; n < nctx; n++) {
			execbuf.rsvd1 = ctx[n];
			gem_execbuf(fd, &execbuf);
		}
		gem_close(fd, obj[XS+1].handle);
		gem_sync(fd, result);
	}

	plug(fd, &cork);

	/* Create a deep dependency chain, with a few branches */
	for (int n = 0; n < nctx && igt_seconds_elapsed(&tv) < 8; n++)
		for (int m = 0; m < XS; m++)
			store_dword(fd, ctx[n], ring, dep[m], 4*n, ctx[n], cork.handle, I915_GEM_DOMAIN_INSTRUCTION);

	for (int n = 0; n < nctx && igt_seconds_elapsed(&tv) < 6; n++) {
		for (int m = 0; m < XS; m++) {
			store_dword(fd, ctx[n], ring, result, 4*n, ctx[n], dep[m], 0);
			store_dword(fd, ctx[n], ring, result, 4*m, ctx[n], 0, I915_GEM_DOMAIN_INSTRUCTION);
		}
		expected = ctx[n];
	}

	unplug_show_queue(fd, &cork, ring);
	igt_require(expected); /* too slow */

	for (int n = 0; n < nctx; n++)
		gem_context_destroy(fd, ctx[n]);

	for (int m = 0; m < XS; m++) {
		ptr = gem_mmap__gtt(fd, dep[m], size, PROT_READ);
		gem_set_domain(fd, dep[m], /* no write hazard lies! */
				I915_GEM_DOMAIN_GTT, I915_GEM_DOMAIN_GTT);
		gem_close(fd, dep[m]);

		for (int n = 0; n < nctx; n++)
			igt_assert_eq_u32(ptr[n], ctx[n]);
		munmap(ptr, size);
	}

	ptr = gem_mmap__gtt(fd, result, size, PROT_READ);
	gem_set_domain(fd, result, /* no write hazard lies! */
			I915_GEM_DOMAIN_GTT, I915_GEM_DOMAIN_GTT);
	gem_close(fd, result);

	/* No reordering due to PI on all contexts because of the common dep */
	for (int m = 0; m < XS; m++)
		igt_assert_eq_u32(ptr[m], expected);
	munmap(ptr, size);

	free(ctx);
#undef XS
}

static void alarm_handler(int sig)
{
}

static int __execbuf(int fd, struct drm_i915_gem_execbuffer2 *execbuf)
{
	int err = 0;
	if (ioctl(fd, DRM_IOCTL_I915_GEM_EXECBUFFER2, execbuf))
		err = -errno;
	return err;
}

static unsigned int measure_ring_size(int fd, unsigned int ring)
{
	struct sigaction sa = { .sa_handler = alarm_handler };
	struct drm_i915_gem_exec_object2 obj[2];
	struct drm_i915_gem_execbuffer2 execbuf;
	const uint32_t bbe = MI_BATCH_BUFFER_END;
	unsigned int count, last;
	struct itimerval itv;
	struct cork c;

	memset(obj, 0, sizeof(obj));
	obj[1].handle = gem_create(fd, 4096);
	gem_write(fd, obj[1].handle, 0, &bbe, sizeof(bbe));

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(obj + 1);
	execbuf.buffer_count = 1;
	execbuf.flags = ring;
	gem_execbuf(fd, &execbuf);
	gem_sync(fd, obj[1].handle);

	plug(fd, &c);
	obj[0].handle = c.handle;

	execbuf.buffers_ptr = to_user_pointer(obj);
	execbuf.buffer_count = 2;
	execbuf.rsvd1 = gem_context_create(fd);

	sigaction(SIGALRM, &sa, NULL);
	itv.it_interval.tv_sec = 0;
	itv.it_interval.tv_usec = 100;
	itv.it_value.tv_sec = 0;
	itv.it_value.tv_usec = 1000;
	setitimer(ITIMER_REAL, &itv, NULL);

	last = -1;
	count = 0;
	do {
		if (__execbuf(fd, &execbuf) == 0) {
			count++;
			continue;
		}

		if (last == count)
			break;

		last = count;
	} while (1);

	memset(&itv, 0, sizeof(itv));
	setitimer(ITIMER_REAL, &itv, NULL);

	unplug(&c);
	gem_close(fd, obj[1].handle);
	gem_context_destroy(fd, execbuf.rsvd1);

	return count;
}

static void wide(int fd, unsigned ring)
{
#define NCTX 4096
	struct timespec tv = {};
	unsigned int ring_size = measure_ring_size(fd, ring);

	struct cork cork;
	uint32_t result;
	uint32_t *ptr;
	uint32_t *ctx;
	unsigned int count;

	ctx = malloc(sizeof(*ctx)*NCTX);
	for (int n = 0; n < NCTX; n++)
		ctx[n] = gem_context_create(fd);

	result = gem_create(fd, 4*NCTX);

	plug(fd, &cork);

	/* Lots of in-order requests, plugged and submitted simultaneously */
	for (count = 0;
	     igt_seconds_elapsed(&tv) < 5 && count < ring_size;
	     count++) {
		for (int n = 0; n < NCTX; n++) {
			store_dword(fd, ctx[n], ring, result, 4*n, ctx[n], cork.handle, I915_GEM_DOMAIN_INSTRUCTION);
		}
	}
	igt_info("Submitted %d requests over %d contexts in %.1fms\n",
		 count, NCTX, igt_nsec_elapsed(&tv) * 1e-6);

	unplug_show_queue(fd, &cork, ring);

	for (int n = 0; n < NCTX; n++)
		gem_context_destroy(fd, ctx[n]);

	ptr = gem_mmap__gtt(fd, result, 4*NCTX, PROT_READ);
	gem_set_domain(fd, result, /* no write hazard lies! */
			I915_GEM_DOMAIN_GTT, I915_GEM_DOMAIN_GTT);
	for (int n = 0; n < NCTX; n++)
		igt_assert_eq_u32(ptr[n], ctx[n]);
	munmap(ptr, 4*NCTX);

	gem_close(fd, result);
	free(ctx);
#undef NCTX
}

static void reorder_wide(int fd, unsigned ring)
{
	const int gen = intel_gen(intel_get_drm_devid(fd));
	struct drm_i915_gem_relocation_entry reloc;
	struct drm_i915_gem_exec_object2 obj[3];
	struct drm_i915_gem_execbuffer2 execbuf;
	struct timespec tv = {};
	unsigned int ring_size = measure_ring_size(fd, ring);
	struct cork cork;
	uint32_t result, target;
	uint32_t *found, *expected;

	result = gem_create(fd, 4096);
	target = gem_create(fd, 4096);

	plug(fd, &cork);

	expected = gem_mmap__cpu(fd, target, 0, 4096, PROT_WRITE);
	gem_set_domain(fd, target, I915_GEM_DOMAIN_CPU, I915_GEM_DOMAIN_CPU);

	memset(obj, 0, sizeof(obj));
	obj[0].handle = cork.handle;
	obj[1].handle = result;
	obj[2].relocs_ptr = to_user_pointer(&reloc);
	obj[2].relocation_count = 1;

	memset(&reloc, 0, sizeof(reloc));
	reloc.target_handle = result;
	reloc.read_domains = I915_GEM_DOMAIN_INSTRUCTION;
	reloc.write_domain = 0; /* lies */

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(obj);
	execbuf.buffer_count = 3;
	execbuf.flags = ring;
	if (gen < 6)
		execbuf.flags |= I915_EXEC_SECURE;

	for (int n = MIN_PRIO, x = 1;
	     igt_seconds_elapsed(&tv) < 5 && n <= MAX_PRIO;
	     n++, x++) {
		unsigned int sz = ALIGN(ring_size * 64, 4096);
		uint32_t *batch;

		execbuf.rsvd1 = gem_context_create(fd);
		gem_context_set_priority(fd, execbuf.rsvd1, n);

		obj[2].handle = gem_create(fd, sz);
		batch = gem_mmap__gtt(fd, obj[2].handle, sz, PROT_WRITE);
		gem_set_domain(fd, obj[2].handle, I915_GEM_DOMAIN_GTT, I915_GEM_DOMAIN_GTT);

		for (int m = 0; m < ring_size; m++) {
			uint64_t addr;
			int idx = hars_petruska_f54_1_random_unsafe_max(1024);
			int i;

			execbuf.batch_start_offset = m * 64;
			reloc.offset = execbuf.batch_start_offset + sizeof(uint32_t);
			reloc.delta = idx * sizeof(uint32_t);
			addr = reloc.presumed_offset + reloc.delta;

			i = execbuf.batch_start_offset / sizeof(uint32_t);
			batch[i] = MI_STORE_DWORD_IMM | (gen < 6 ? 1 << 22 : 0);
			if (gen >= 8) {
				batch[++i] = addr;
				batch[++i] = addr >> 32;
			} else if (gen >= 4) {
				batch[++i] = 0;
				batch[++i] = addr;
				reloc.offset += sizeof(uint32_t);
			} else {
				batch[i]--;
				batch[++i] = addr;
			}
			batch[++i] = x;
			batch[++i] = MI_BATCH_BUFFER_END;

			if (!expected[idx])
				expected[idx] =  x;

			gem_execbuf(fd, &execbuf);
		}

		munmap(batch, sz);
		gem_close(fd, obj[2].handle);
		gem_context_destroy(fd, execbuf.rsvd1);
	}

	unplug_show_queue(fd, &cork, ring);

	found = gem_mmap__gtt(fd, result, 4096, PROT_READ);
	gem_set_domain(fd, result, /* no write hazard lies! */
			I915_GEM_DOMAIN_GTT, I915_GEM_DOMAIN_GTT);
	for (int n = 0; n < 1024; n++)
		igt_assert_eq_u32(found[n], expected[n]);
	munmap(found, 4096);
	munmap(expected, 4096);

	gem_close(fd, result);
	gem_close(fd, target);
}

static void bind_to_cpu(int cpu)
{
	const int ncpus = sysconf(_SC_NPROCESSORS_ONLN);
	struct sched_param rt = {.sched_priority = 99 };
	cpu_set_t allowed;

	igt_assert(sched_setscheduler(getpid(), SCHED_RR | SCHED_RESET_ON_FORK, &rt) == 0);

	CPU_ZERO(&allowed);
	CPU_SET(cpu % ncpus, &allowed);
	igt_assert(sched_setaffinity(getpid(), sizeof(cpu_set_t), &allowed) == 0);
}

static void test_pi_ringfull(int fd, unsigned int engine)
{
	const uint32_t bbe = MI_BATCH_BUFFER_END;
	struct sigaction sa = { .sa_handler = alarm_handler };
	struct drm_i915_gem_execbuffer2 execbuf;
	struct drm_i915_gem_exec_object2 obj[2];
	unsigned int last, count;
	struct itimerval itv;
	struct cork c;
	bool *result;

	result = mmap(NULL, 4096, PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
	igt_assert(result != MAP_FAILED);

	memset(&execbuf, 0, sizeof(execbuf));
	memset(&obj, 0, sizeof(obj));

	obj[1].handle = gem_create(fd, 4096);
	gem_write(fd, obj[1].handle, 0, &bbe, sizeof(bbe));

	execbuf.buffers_ptr = to_user_pointer(&obj[1]);
	execbuf.buffer_count = 1;
	execbuf.flags = engine;
	execbuf.rsvd1 = gem_context_create(fd);
	gem_context_set_priority(fd, execbuf.rsvd1, MIN_PRIO);

	gem_execbuf(fd, &execbuf);
	gem_sync(fd, obj[1].handle);

	/* Fill the low-priority ring */
	plug(fd, &c);
	obj[0].handle = c.handle;

	execbuf.buffers_ptr = to_user_pointer(obj);
	execbuf.buffer_count = 2;

	sigaction(SIGALRM, &sa, NULL);
	itv.it_interval.tv_sec = 0;
	itv.it_interval.tv_usec = 100;
	itv.it_value.tv_sec = 0;
	itv.it_value.tv_usec = 1000;
	setitimer(ITIMER_REAL, &itv, NULL);

	last = -1;
	count = 0;
	do {
		if (__execbuf(fd, &execbuf) == 0) {
			count++;
			continue;
		}

		if (last == count)
			break;

		last = count;
	} while (1);
	igt_debug("Filled low-priority ring with %d batches\n", count);

	memset(&itv, 0, sizeof(itv));
	setitimer(ITIMER_REAL, &itv, NULL);

	execbuf.buffers_ptr = to_user_pointer(&obj[1]);
	execbuf.buffer_count = 1;

	/* both parent + child on the same cpu, only parent is RT */
	bind_to_cpu(0);

	igt_fork(child, 1) {
		result[0] = true;

		igt_debug("Creating HP context\n");
		execbuf.rsvd1 = gem_context_create(fd);
		gem_context_set_priority(fd, execbuf.rsvd1, MAX_PRIO);

		kill(getppid(), SIGALRM);
		sched_yield();
		result[1] = true;

		itv.it_value.tv_sec = 0;
		itv.it_value.tv_usec = 10000;
		setitimer(ITIMER_REAL, &itv, NULL);

		/* Since we are the high priority task, we expect to be
		 * able to add ourselves to *our* ring without interruption.
		 */
		igt_debug("HP child executing\n");
		result[2] = __execbuf(fd, &execbuf) == 0;
		gem_context_destroy(fd, execbuf.rsvd1);
	}

	/* Relinquish CPU just to allow child to create a context */
	sleep(1);
	igt_assert_f(result[0], "HP context (child) not created");
	igt_assert_f(!result[1], "Child released too early!\n");

	/* Parent sleeps waiting for ringspace, releasing child */
	itv.it_value.tv_sec = 0;
	itv.it_value.tv_usec = 50000;
	setitimer(ITIMER_REAL, &itv, NULL);
	igt_debug("LP parent executing\n");
	igt_assert_eq(__execbuf(fd, &execbuf), -EINTR);
	igt_assert_f(result[1], "Child was not released!\n");
	igt_assert_f(result[2],
		     "High priority child unable to submit within 10ms\n");

	unplug(&c);
	igt_waitchildren();

	gem_context_destroy(fd, execbuf.rsvd1);
	gem_close(fd, obj[1].handle);
	gem_close(fd, obj[0].handle);
	munmap(result, 4096);
}

igt_main
{
	const struct intel_execution_engine *e;
	int fd = -1;

	igt_skip_on_simulation();

	igt_fixture {
		fd = drm_open_driver_master(DRIVER_INTEL);
		igt_show_submission_method(fd);
		igt_show_scheduler_capability(fd);
		igt_require_gem(fd);
		gem_require_mmap_wc(fd);
		igt_fork_hang_detector(fd);
	}

	igt_subtest_group {
		for (e = intel_execution_engines; e->name; e++) {
			/* default exec-id is purely symbolic */
			if (e->exec_id == 0)
				continue;

			igt_subtest_f("fifo-%s", e->name) {
				gem_require_ring(fd, e->exec_id | e->flags);
				igt_require(gem_can_store_dword(fd, e->exec_id) | e->flags);
				fifo(fd, e->exec_id | e->flags);
			}
		}
	}

	igt_subtest_group {
		igt_fixture {
			igt_require(gem_scheduler_enabled(fd));
			igt_require(gem_scheduler_has_ctx_priority(fd));
		}

		igt_subtest("smoketest-all")
			smoketest(fd, -1, 30);

		for (e = intel_execution_engines; e->name; e++) {
			/* default exec-id is purely symbolic */
			if (e->exec_id == 0)
				continue;

			igt_subtest_group {
				igt_fixture {
					gem_require_ring(fd, e->exec_id | e->flags);
					igt_require(gem_can_store_dword(fd, e->exec_id) | e->flags);
				}

				igt_subtest_f("in-order-%s", e->name)
					reorder(fd, e->exec_id | e->flags, EQUAL);

				igt_subtest_f("out-order-%s", e->name)
					reorder(fd, e->exec_id | e->flags, 0);

				igt_subtest_f("promotion-%s", e->name)
					promotion(fd, e->exec_id | e->flags);

				igt_subtest_group {
					igt_fixture {
						igt_require(gem_scheduler_has_preemption(fd));
					}

					igt_subtest_f("preempt-%s", e->name)
						preempt(fd, e->exec_id | e->flags, 0);

					igt_subtest_f("preempt-contexts-%s", e->name)
						preempt(fd, e->exec_id | e->flags, NEW_CTX);

					igt_subtest_f("preempt-other-%s", e->name)
						preempt_other(fd, e->exec_id | e->flags);

					igt_subtest_f("preempt-self-%s", e->name)
						preempt_self(fd, e->exec_id | e->flags);
				}

				igt_subtest_f("deep-%s", e->name)
					deep(fd, e->exec_id | e->flags);

				igt_subtest_f("wide-%s", e->name)
					wide(fd, e->exec_id | e->flags);

				igt_subtest_f("reorder-wide-%s", e->name)
					reorder_wide(fd, e->exec_id | e->flags);

				igt_subtest_f("smoketest-%s", e->name)
					smoketest(fd, e->exec_id | e->flags, 5);
			}
		}
	}

	igt_subtest_group {
		igt_fixture {
			igt_require(gem_scheduler_enabled(fd));
			igt_require(gem_scheduler_has_ctx_priority(fd));

			/* need separate rings */
			igt_require(gem_has_execlists(fd));
		}

		for (e = intel_execution_engines; e->name; e++) {
			igt_subtest_group {
				igt_fixture {
					gem_require_ring(fd, e->exec_id | e->flags);
					igt_require(gem_scheduler_has_preemption(fd));
				}

				igt_subtest_f("pi-ringfull-%s", e->name)
					test_pi_ringfull(fd, e->exec_id | e->flags);
			}
		}
	}

	igt_fixture {
		igt_stop_hang_detector();
		close(fd);
	}
}
