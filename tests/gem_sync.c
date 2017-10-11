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

#include <time.h>
#include <pthread.h>

#include "igt.h"
#include "igt_sysfs.h"

#define BIT(x) (1ul << (x))

#define LOCAL_I915_EXEC_NO_RELOC (1<<11)
#define LOCAL_I915_EXEC_HANDLE_LUT (1<<12)

#define LOCAL_I915_EXEC_BSD_SHIFT      (13)
#define LOCAL_I915_EXEC_BSD_MASK       (3 << LOCAL_I915_EXEC_BSD_SHIFT)

#define LOCAL_PARAM_HAS_SCHEDULER 41
#define   HAS_SCHEDULER		BIT(0)
#define   HAS_PRIORITY		BIT(1)
#define   HAS_PREEMPTION	BIT(2)
#define LOCAL_CONTEXT_PARAM_PRIORITY 6
#define   MAX_PRIO 1023
#define   MIN_PRIO -1023

#define ENGINE_MASK  (I915_EXEC_RING_MASK | LOCAL_I915_EXEC_BSD_MASK)

IGT_TEST_DESCRIPTION("Basic check of ring<->ring write synchronisation.");

/*
 * Testcase: Basic check of sync
 *
 * Extremely efficient at catching missed irqs
 */

static double gettime(void)
{
	static clockid_t clock = -1;
	struct timespec ts;

	/* Stay on the same clock for consistency. */
	if (clock != (clockid_t)-1) {
		if (clock_gettime(clock, &ts))
			goto error;
		goto out;
	}

#ifdef CLOCK_MONOTONIC_RAW
	if (!clock_gettime(clock = CLOCK_MONOTONIC_RAW, &ts))
		goto out;
#endif
#ifdef CLOCK_MONOTONIC_COARSE
	if (!clock_gettime(clock = CLOCK_MONOTONIC_COARSE, &ts))
		goto out;
#endif
	if (!clock_gettime(clock = CLOCK_MONOTONIC, &ts))
		goto out;
error:
	igt_warn("Could not read monotonic time: %s\n",
			strerror(errno));
	igt_assert(0);
	return 0;

out:
	return ts.tv_sec + 1e-9*ts.tv_nsec;
}

static void
sync_ring(int fd, unsigned ring, int num_children, int timeout)
{
	unsigned engines[16];
	const char *names[16];
	int num_engines = 0;

	if (ring == ~0u) {
		const struct intel_execution_engine *e;

		for (e = intel_execution_engines; e->name; e++) {
			if (e->exec_id == 0)
				continue;

			if (!gem_has_ring(fd, e->exec_id | e->flags))
				continue;

			if (e->exec_id == I915_EXEC_BSD) {
				int is_bsd2 = e->flags != 0;
				if (gem_has_bsd2(fd) != is_bsd2)
					continue;
			}

			names[num_engines] = e->name;
			engines[num_engines++] = e->exec_id | e->flags;
			if (num_engines == ARRAY_SIZE(engines))
				break;
		}

		num_children *= num_engines;
	} else {
		gem_require_ring(fd, ring);
		names[num_engines] = NULL;
		engines[num_engines++] = ring;
	}

	intel_detect_and_clear_missed_interrupts(fd);
	igt_fork(child, num_children) {
		const uint32_t bbe = MI_BATCH_BUFFER_END;
		struct drm_i915_gem_exec_object2 object;
		struct drm_i915_gem_execbuffer2 execbuf;
		double start, elapsed;
		unsigned long cycles;

		memset(&object, 0, sizeof(object));
		object.handle = gem_create(fd, 4096);
		gem_write(fd, object.handle, 0, &bbe, sizeof(bbe));

		memset(&execbuf, 0, sizeof(execbuf));
		execbuf.buffers_ptr = to_user_pointer(&object);
		execbuf.buffer_count = 1;
		execbuf.flags = engines[child % num_engines];
		gem_execbuf(fd, &execbuf);
		gem_sync(fd, object.handle);

		start = gettime();
		cycles = 0;
		do {
			do {
				gem_execbuf(fd, &execbuf);
				gem_sync(fd, object.handle);
			} while (++cycles & 1023);
		} while ((elapsed = gettime() - start) < timeout);
		igt_info("%s%sompleted %ld cycles: %.3f us\n",
			 names[child % num_engines] ?: "",
			 names[child % num_engines] ? " c" : "C",
			 cycles, elapsed*1e6/cycles);

		gem_close(fd, object.handle);
	}
	igt_waitchildren_timeout(timeout+10, NULL);
	igt_assert_eq(intel_detect_and_clear_missed_interrupts(fd), 0);
}

static void
store_ring(int fd, unsigned ring, int num_children, int timeout)
{
	const int gen = intel_gen(intel_get_drm_devid(fd));
	unsigned engines[16];
	const char *names[16];
	int num_engines = 0;

	if (ring == ~0u) {
		const struct intel_execution_engine *e;

		for (e = intel_execution_engines; e->name; e++) {
			if (e->exec_id == 0)
				continue;

			if (!gem_has_ring(fd, e->exec_id | e->flags))
				continue;

			if (!gem_can_store_dword(fd, e->exec_id | e->flags))
				continue;

			if (e->exec_id == I915_EXEC_BSD) {
				int is_bsd2 = e->flags != 0;
				if (gem_has_bsd2(fd) != is_bsd2)
					continue;
			}

			names[num_engines] = e->name;
			engines[num_engines++] = e->exec_id | e->flags;
			if (num_engines == ARRAY_SIZE(engines))
				break;
		}

		num_children *= num_engines;
	} else {
		gem_require_ring(fd, ring);
		igt_require(gem_can_store_dword(fd, ring));
		names[num_engines] = NULL;
		engines[num_engines++] = ring;
	}

	intel_detect_and_clear_missed_interrupts(fd);
	igt_fork(child, num_children) {
		const uint32_t bbe = MI_BATCH_BUFFER_END;
		struct drm_i915_gem_exec_object2 object[2];
		struct drm_i915_gem_relocation_entry reloc[1024];
		struct drm_i915_gem_execbuffer2 execbuf;
		double start, elapsed;
		unsigned long cycles;
		uint32_t *batch, *b;

		memset(&execbuf, 0, sizeof(execbuf));
		execbuf.buffers_ptr = to_user_pointer(object);
		execbuf.flags = engines[child % num_engines];
		execbuf.flags |= LOCAL_I915_EXEC_NO_RELOC;
		execbuf.flags |= LOCAL_I915_EXEC_HANDLE_LUT;
		if (gen < 6)
			execbuf.flags |= I915_EXEC_SECURE;

		memset(object, 0, sizeof(object));
		object[0].handle = gem_create(fd, 4096);
		gem_write(fd, object[0].handle, 0, &bbe, sizeof(bbe));
		execbuf.buffer_count = 1;
		gem_execbuf(fd, &execbuf);

		object[0].flags |= EXEC_OBJECT_WRITE;
		object[1].handle = gem_create(fd, 20*1024);

		object[1].relocs_ptr = to_user_pointer(reloc);
		object[1].relocation_count = 1024;

		batch = gem_mmap__cpu(fd, object[1].handle, 0, 20*1024,
				PROT_WRITE | PROT_READ);
		gem_set_domain(fd, object[1].handle,
				I915_GEM_DOMAIN_CPU, I915_GEM_DOMAIN_CPU);

		memset(reloc, 0, sizeof(reloc));
		b = batch;
		for (int i = 0; i < 1024; i++) {
			uint64_t offset;

			reloc[i].presumed_offset = object[0].offset;
			reloc[i].offset = (b - batch + 1) * sizeof(*batch);
			reloc[i].delta = i * sizeof(uint32_t);
			reloc[i].read_domains = I915_GEM_DOMAIN_INSTRUCTION;
			reloc[i].write_domain = I915_GEM_DOMAIN_INSTRUCTION;

			offset = object[0].offset + reloc[i].delta;
			*b++ = MI_STORE_DWORD_IMM | (gen < 6 ? 1 << 22 : 0);
			if (gen >= 8) {
				*b++ = offset;
				*b++ = offset >> 32;
			} else if (gen >= 4) {
				*b++ = 0;
				*b++ = offset;
				reloc[i].offset += sizeof(*batch);
			} else {
				b[-1] -= 1;
				*b++ = offset;
			}
			*b++ = i;
		}
		*b++ = MI_BATCH_BUFFER_END;
		igt_assert((b - batch)*sizeof(uint32_t) < 20*1024);
		munmap(batch, 20*1024);
		execbuf.buffer_count = 2;
		gem_execbuf(fd, &execbuf);
		gem_sync(fd, object[1].handle);

		start = gettime();
		cycles = 0;
		do {
			do {
				gem_execbuf(fd, &execbuf);
				gem_sync(fd, object[1].handle);
			} while (++cycles & 1023);
		} while ((elapsed = gettime() - start) < timeout);
		igt_info("%s%sompleted %ld cycles: %.3f us\n",
			 names[child % num_engines] ?: "",
			 names[child % num_engines] ? " c" : "C",
			 cycles, elapsed*1e6/cycles);

		gem_close(fd, object[1].handle);
		gem_close(fd, object[0].handle);
	}
	igt_waitchildren_timeout(timeout+10, NULL);
	igt_assert_eq(intel_detect_and_clear_missed_interrupts(fd), 0);
}

static void xchg(void *array, unsigned i, unsigned j)
{
	uint32_t *u32 = array;
	uint32_t tmp = u32[i];
	u32[i] = u32[j];
	u32[j] = tmp;
}

struct waiter {
	pthread_t thread;
	pthread_mutex_t mutex;
	pthread_cond_t cond;

	int ready;
	volatile int *done;

	int fd;
	struct drm_i915_gem_exec_object2 object;
	uint32_t handles[64];
};

static void *waiter(void *arg)
{
	struct waiter *w = arg;

	do {
		pthread_mutex_lock(&w->mutex);
		w->ready = 0;
		pthread_cond_signal(&w->cond);
		while (!w->ready)
			pthread_cond_wait(&w->cond, &w->mutex);
		pthread_mutex_unlock(&w->mutex);
		if (*w->done < 0)
			return NULL;

		gem_sync(w->fd, w->object.handle);
		for (int n = 0;  n < ARRAY_SIZE(w->handles); n++)
			gem_sync(w->fd, w->handles[n]);
	} while (1);
}

static void
__store_many(int fd, unsigned ring, int timeout, unsigned long *cycles)
{
	const int gen = intel_gen(intel_get_drm_devid(fd));
	const uint32_t bbe = MI_BATCH_BUFFER_END;
	struct drm_i915_gem_exec_object2 object[2];
	struct drm_i915_gem_execbuffer2 execbuf;
	struct drm_i915_gem_relocation_entry reloc[1024];
	struct waiter threads[64];
	int order[64];
	uint32_t *batch, *b;
	int done;

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(object);
	execbuf.flags = ring;
	execbuf.flags |= LOCAL_I915_EXEC_NO_RELOC;
	execbuf.flags |= LOCAL_I915_EXEC_HANDLE_LUT;
	if (gen < 6)
		execbuf.flags |= I915_EXEC_SECURE;

	memset(object, 0, sizeof(object));
	object[0].handle = gem_create(fd, 4096);
	gem_write(fd, object[0].handle, 0, &bbe, sizeof(bbe));
	execbuf.buffer_count = 1;
	gem_execbuf(fd, &execbuf);
	object[0].flags |= EXEC_OBJECT_WRITE;

	object[1].relocs_ptr = to_user_pointer(reloc);
	object[1].relocation_count = 1024;
	execbuf.buffer_count = 2;

	memset(reloc, 0, sizeof(reloc));
	b = batch = malloc(20*1024);
	for (int i = 0; i < 1024; i++) {
		uint64_t offset;

		reloc[i].presumed_offset = object[0].offset;
		reloc[i].offset = (b - batch + 1) * sizeof(*batch);
		reloc[i].delta = i * sizeof(uint32_t);
		reloc[i].read_domains = I915_GEM_DOMAIN_INSTRUCTION;
		reloc[i].write_domain = I915_GEM_DOMAIN_INSTRUCTION;

		offset = object[0].offset + reloc[i].delta;
		*b++ = MI_STORE_DWORD_IMM | (gen < 6 ? 1 << 22 : 0);
		if (gen >= 8) {
			*b++ = offset;
			*b++ = offset >> 32;
		} else if (gen >= 4) {
			*b++ = 0;
			*b++ = offset;
			reloc[i].offset += sizeof(*batch);
		} else {
			b[-1] -= 1;
			*b++ = offset;
		}
		*b++ = i;
	}
	*b++ = MI_BATCH_BUFFER_END;
	igt_assert((b - batch)*sizeof(uint32_t) < 20*1024);

	done = 0;
	for (int i = 0; i < ARRAY_SIZE(threads); i++) {
		threads[i].fd = fd;
		threads[i].object = object[1];
		threads[i].object.handle = gem_create(fd, 20*1024);
		gem_write(fd, threads[i].object.handle, 0, batch, 20*1024);

		pthread_cond_init(&threads[i].cond, NULL);
		pthread_mutex_init(&threads[i].mutex, NULL);
		threads[i].done = &done;
		threads[i].ready = 0;

		pthread_create(&threads[i].thread, NULL, waiter, &threads[i]);
		order[i] = i;
	}
	free(batch);

	for (int i = 0; i < ARRAY_SIZE(threads); i++) {
		for (int j = 0; j < ARRAY_SIZE(threads); j++)
			threads[i].handles[j] = threads[j].object.handle;
	}

	igt_until_timeout(timeout) {
		for (int i = 0; i < ARRAY_SIZE(threads); i++) {
			pthread_mutex_lock(&threads[i].mutex);
			while (threads[i].ready)
				pthread_cond_wait(&threads[i].cond,
						  &threads[i].mutex);
			pthread_mutex_unlock(&threads[i].mutex);
			igt_permute_array(threads[i].handles,
					  ARRAY_SIZE(threads[i].handles),
					  xchg);
		}

		igt_permute_array(order, ARRAY_SIZE(threads), xchg);
		for (int i = 0; i < ARRAY_SIZE(threads); i++) {
			object[1] = threads[i].object;
			gem_execbuf(fd, &execbuf);
			threads[i].object = object[1];
		}
		++*cycles;

		for (int i = 0; i < ARRAY_SIZE(threads); i++) {
			struct waiter *w = &threads[order[i]];

			w->ready = 1;
			pthread_cond_signal(&w->cond);
		}
	}

	for (int i = 0; i < ARRAY_SIZE(threads); i++) {
		pthread_mutex_lock(&threads[i].mutex);
		while (threads[i].ready)
			pthread_cond_wait(&threads[i].cond, &threads[i].mutex);
		pthread_mutex_unlock(&threads[i].mutex);
	}
	done = -1;
	for (int i = 0; i < ARRAY_SIZE(threads); i++) {
		threads[i].ready = 1;
		pthread_cond_signal(&threads[i].cond);
		pthread_join(threads[i].thread, NULL);
		gem_close(fd, threads[i].object.handle);
	}

	gem_close(fd, object[0].handle);
}

static void
store_many(int fd, unsigned ring, int timeout)
{
	unsigned long *shared;
	const char *names[16];
	int n = 0;

	shared = mmap(NULL, 4096, PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
	igt_assert(shared != MAP_FAILED);

	intel_detect_and_clear_missed_interrupts(fd);

	if (ring == ~0u) {
		const struct intel_execution_engine *e;

		for (e = intel_execution_engines; e->name; e++) {
			if (e->exec_id == 0)
				continue;

			if (!gem_has_ring(fd, e->exec_id | e->flags))
				continue;

			if (!gem_can_store_dword(fd, e->exec_id | e->flags))
				continue;

			if (e->exec_id == I915_EXEC_BSD) {
				int is_bsd2 = e->flags != 0;
				if (gem_has_bsd2(fd) != is_bsd2)
					continue;
			}

			igt_fork(child, 1)
				__store_many(fd,
					     e->exec_id | e->flags,
					     timeout,
					     &shared[n]);

			names[n++] = e->name;
		}
		igt_waitchildren();
	} else {
		gem_require_ring(fd, ring);
		igt_require(gem_can_store_dword(fd, ring));
		__store_many(fd, ring, timeout, &shared[n]);
		names[n++] = NULL;
	}

	for (int i = 0; i < n; i++) {
		igt_info("%s%sompleted %ld cycles\n",
			 names[i] ?: "", names[i] ? " c" : "C", shared[i]);
	}
	igt_assert_eq(intel_detect_and_clear_missed_interrupts(fd), 0);
	munmap(shared, 4096);
}

static void
sync_all(int fd, int num_children, int timeout)
{
	const struct intel_execution_engine *e;
	unsigned engines[16];
	int num_engines = 0;

	for (e = intel_execution_engines; e->name; e++) {
		if (e->exec_id == 0)
			continue;

		if (!gem_has_ring(fd, e->exec_id | e->flags))
			continue;

		if (e->exec_id == I915_EXEC_BSD) {
			int is_bsd2 = e->flags != 0;
			if (gem_has_bsd2(fd) != is_bsd2)
				continue;
		}

		engines[num_engines++] = e->exec_id | e->flags;
		if (num_engines == ARRAY_SIZE(engines))
			break;
	}
	igt_require(num_engines);

	intel_detect_and_clear_missed_interrupts(fd);
	igt_fork(child, num_children) {
		const uint32_t bbe = MI_BATCH_BUFFER_END;
		struct drm_i915_gem_exec_object2 object;
		struct drm_i915_gem_execbuffer2 execbuf;
		double start, elapsed;
		unsigned long cycles;

		memset(&object, 0, sizeof(object));
		object.handle = gem_create(fd, 4096);
		gem_write(fd, object.handle, 0, &bbe, sizeof(bbe));

		memset(&execbuf, 0, sizeof(execbuf));
		execbuf.buffers_ptr = to_user_pointer(&object);
		execbuf.buffer_count = 1;
		gem_execbuf(fd, &execbuf);
		gem_sync(fd, object.handle);

		start = gettime();
		cycles = 0;
		do {
			do {
				for (int n = 0; n < num_engines; n++) {
					execbuf.flags = engines[n];
					gem_execbuf(fd, &execbuf);
				}
				gem_sync(fd, object.handle);
			} while (++cycles & 1023);
		} while ((elapsed = gettime() - start) < timeout);
		igt_info("Completed %ld cycles: %.3f us\n",
			 cycles, elapsed*1e6/cycles);

		gem_close(fd, object.handle);
	}
	igt_waitchildren_timeout(timeout+10, NULL);
	igt_assert_eq(intel_detect_and_clear_missed_interrupts(fd), 0);
}

static void
store_all(int fd, int num_children, int timeout)
{
	const int gen = intel_gen(intel_get_drm_devid(fd));
	const struct intel_execution_engine *e;
	unsigned engines[16];
	int num_engines = 0;

	for (e = intel_execution_engines; e->name; e++) {
		if (e->exec_id == 0)
			continue;

		if (!gem_has_ring(fd, e->exec_id | e->flags))
			continue;

		if (!gem_can_store_dword(fd, e->exec_id))
			continue;

		if (e->exec_id == I915_EXEC_BSD) {
			int is_bsd2 = e->flags != 0;
			if (gem_has_bsd2(fd) != is_bsd2)
				continue;
		}

		engines[num_engines++] = e->exec_id | e->flags;
		if (num_engines == ARRAY_SIZE(engines))
			break;
	}
	igt_require(num_engines);

	intel_detect_and_clear_missed_interrupts(fd);
	igt_fork(child, num_children) {
		const uint32_t bbe = MI_BATCH_BUFFER_END;
		struct drm_i915_gem_exec_object2 object[2];
		struct drm_i915_gem_relocation_entry reloc[1024];
		struct drm_i915_gem_execbuffer2 execbuf;
		double start, elapsed;
		unsigned long cycles;
		uint32_t *batch, *b;

		memset(&execbuf, 0, sizeof(execbuf));
		execbuf.buffers_ptr = to_user_pointer(object);
		execbuf.flags |= LOCAL_I915_EXEC_NO_RELOC;
		execbuf.flags |= LOCAL_I915_EXEC_HANDLE_LUT;
		if (gen < 6)
			execbuf.flags |= I915_EXEC_SECURE;

		memset(object, 0, sizeof(object));
		object[0].handle = gem_create(fd, 4096);
		gem_write(fd, object[0].handle, 0, &bbe, sizeof(bbe));
		execbuf.buffer_count = 1;
		gem_execbuf(fd, &execbuf);

		object[0].flags |= EXEC_OBJECT_WRITE;
		object[1].handle = gem_create(fd, 1024*16 + 4096);

		object[1].relocs_ptr = to_user_pointer(reloc);
		object[1].relocation_count = 1024;

		batch = gem_mmap__cpu(fd, object[1].handle, 0, 16*1024 + 4096,
				PROT_WRITE | PROT_READ);
		gem_set_domain(fd, object[1].handle,
				I915_GEM_DOMAIN_CPU, I915_GEM_DOMAIN_CPU);

		memset(reloc, 0, sizeof(reloc));
		b = batch;
		for (int i = 0; i < 1024; i++) {
			uint64_t offset;

			reloc[i].presumed_offset = object[0].offset;
			reloc[i].offset = (b - batch + 1) * sizeof(*batch);
			reloc[i].delta = i * sizeof(uint32_t);
			reloc[i].read_domains = I915_GEM_DOMAIN_INSTRUCTION;
			reloc[i].write_domain = I915_GEM_DOMAIN_INSTRUCTION;

			offset = object[0].offset + reloc[i].delta;
			*b++ = MI_STORE_DWORD_IMM | (gen < 6 ? 1 << 22 : 0);
			if (gen >= 8) {
				*b++ = offset;
				*b++ = offset >> 32;
			} else if (gen >= 4) {
				*b++ = 0;
				*b++ = offset;
				reloc[i].offset += sizeof(*batch);
			} else {
				b[-1] -= 1;
				*b++ = offset;
			}
			*b++ = i;
		}
		*b++ = MI_BATCH_BUFFER_END;
		igt_assert((b - batch)*sizeof(uint32_t) < 20*1024);
		munmap(batch, 16*1024+4096);
		execbuf.buffer_count = 2;
		gem_execbuf(fd, &execbuf);
		gem_sync(fd, object[1].handle);

		start = gettime();
		cycles = 0;
		do {
			do {
				igt_permute_array(engines, num_engines, xchg);
				for (int n = 0; n < num_engines; n++) {
					execbuf.flags &= ~ENGINE_MASK;
					execbuf.flags |= engines[n];
					gem_execbuf(fd, &execbuf);
				}
				gem_sync(fd, object[1].handle);
			} while (++cycles & 1023);
		} while ((elapsed = gettime() - start) < timeout);
		igt_info("Completed %ld cycles: %.3f us\n",
			 cycles, elapsed*1e6/cycles);

		gem_close(fd, object[1].handle);
		gem_close(fd, object[0].handle);
	}
	igt_waitchildren_timeout(timeout+10, NULL);
	igt_assert_eq(intel_detect_and_clear_missed_interrupts(fd), 0);
}

static int __ctx_set_priority(int fd, uint32_t ctx, int prio)
{
	struct local_i915_gem_context_param param;

	memset(&param, 0, sizeof(param));
	param.context = ctx;
	param.size = 0;
	param.param = LOCAL_CONTEXT_PARAM_PRIORITY;
	param.value = prio;

	return __gem_context_set_param(fd, &param);
}

static void ctx_set_priority(int fd, uint32_t ctx, int prio)
{
	igt_assert_eq(__ctx_set_priority(fd, ctx, prio), 0);
}

static void
preempt(int fd, unsigned ring, int num_children, int timeout)
{
	unsigned engines[16];
	const char *names[16];
	int num_engines = 0;
	uint32_t ctx[2];

	if (ring == ~0u) {
		const struct intel_execution_engine *e;

		for (e = intel_execution_engines; e->name; e++) {
			if (e->exec_id == 0)
				continue;

			if (!gem_has_ring(fd, e->exec_id | e->flags))
				continue;

			if (e->exec_id == I915_EXEC_BSD) {
				int is_bsd2 = e->flags != 0;
				if (gem_has_bsd2(fd) != is_bsd2)
					continue;
			}

			names[num_engines] = e->name;
			engines[num_engines++] = e->exec_id | e->flags;
			if (num_engines == ARRAY_SIZE(engines))
				break;
		}

		num_children *= num_engines;
	} else {
		gem_require_ring(fd, ring);
		names[num_engines] = NULL;
		engines[num_engines++] = ring;
	}

	ctx[0] = gem_context_create(fd);
	ctx_set_priority(fd, ctx[0], MIN_PRIO);

	ctx[1] = gem_context_create(fd);
	ctx_set_priority(fd, ctx[1], MAX_PRIO);

	intel_detect_and_clear_missed_interrupts(fd);
	igt_fork(child, num_children) {
		const uint32_t bbe = MI_BATCH_BUFFER_END;
		struct drm_i915_gem_exec_object2 object;
		struct drm_i915_gem_execbuffer2 execbuf;
		double start, elapsed;
		unsigned long cycles;

		memset(&object, 0, sizeof(object));
		object.handle = gem_create(fd, 4096);
		gem_write(fd, object.handle, 0, &bbe, sizeof(bbe));

		memset(&execbuf, 0, sizeof(execbuf));
		execbuf.buffers_ptr = to_user_pointer(&object);
		execbuf.buffer_count = 1;
		execbuf.flags = engines[child % num_engines];
		execbuf.rsvd1 = ctx[1];
		gem_execbuf(fd, &execbuf);
		gem_sync(fd, object.handle);

		start = gettime();
		cycles = 0;
		do {
			igt_spin_t *spin =
				__igt_spin_batch_new(fd,
						     ctx[0],
						     execbuf.flags,
						     0);

			do {
				gem_execbuf(fd, &execbuf);
				gem_sync(fd, object.handle);
			} while (++cycles & 1023);

			igt_spin_batch_free(fd, spin);
		} while ((elapsed = gettime() - start) < timeout);
		igt_info("%s%sompleted %ld cycles: %.3f us\n",
			 names[child % num_engines] ?: "",
			 names[child % num_engines] ? " c" : "C",
			 cycles, elapsed*1e6/cycles);

		gem_close(fd, object.handle);
	}
	igt_waitchildren_timeout(timeout+10, NULL);
	igt_assert_eq(intel_detect_and_clear_missed_interrupts(fd), 0);

	gem_context_destroy(fd, ctx[1]);
	gem_context_destroy(fd, ctx[0]);
}

static unsigned int has_scheduler(int fd)
{
	drm_i915_getparam_t gp;
	unsigned int caps = 0;

	gp.param = LOCAL_PARAM_HAS_SCHEDULER;
	gp.value = (int *)&caps;
	drmIoctl(fd, DRM_IOCTL_I915_GETPARAM, &gp);

	if (!caps)
		return 0;

	igt_info("Has kernel scheduler\n");
	if (caps & HAS_PRIORITY)
		igt_info(" - With priority sorting\n");
	if (caps & HAS_PREEMPTION)
		igt_info(" - With preemption enabled\n");

	return caps;
}

igt_main
{
	const struct intel_execution_engine *e;
	const int ncpus = sysconf(_SC_NPROCESSORS_ONLN);
	unsigned int sched_caps = 0;
	int fd = -1;

	igt_skip_on_simulation();

	igt_fixture {
		fd = drm_open_driver(DRIVER_INTEL);
		igt_require_gem(fd);
		igt_show_submission_method(fd);
		sched_caps = has_scheduler(fd);

		igt_fork_hang_detector(fd);
	}

	for (e = intel_execution_engines; e->name; e++) {
		igt_subtest_f("%s", e->name)
			sync_ring(fd, e->exec_id | e->flags, 1, 150);
		igt_subtest_f("store-%s", e->name)
			store_ring(fd, e->exec_id | e->flags, 1, 150);
		igt_subtest_f("many-%s", e->name)
			store_many(fd, e->exec_id | e->flags, 150);
		igt_subtest_f("forked-%s", e->name)
			sync_ring(fd, e->exec_id | e->flags, ncpus, 150);
		igt_subtest_f("forked-store-%s", e->name)
			store_ring(fd, e->exec_id | e->flags, ncpus, 150);
	}

	igt_subtest("basic-each")
		sync_ring(fd, ~0u, 1, 5);
	igt_subtest("basic-store-each")
		store_ring(fd, ~0u, 1, 5);
	igt_subtest("basic-many-each")
		store_many(fd, ~0u, 5);
	igt_subtest("forked-each")
		sync_ring(fd, ~0u, ncpus, 150);
	igt_subtest("forked-store-each")
		store_ring(fd, ~0u, ncpus, 150);

	igt_subtest("basic-all")
		sync_all(fd, 1, 5);
	igt_subtest("basic-store-all")
		store_all(fd, 1, 5);

	igt_subtest("all")
		sync_all(fd, 1, 150);
	igt_subtest("store-all")
		store_all(fd, 1, 150);
	igt_subtest("forked-all")
		sync_all(fd, ncpus, 150);
	igt_subtest("forked-store-all")
		store_all(fd, ncpus, 150);

	igt_subtest_group {
		igt_fixture {
			igt_require(sched_caps & HAS_PRIORITY);
			igt_require(sched_caps & HAS_PREEMPTION);
		}

		igt_subtest("preempt-all")
			preempt(fd, -1, 1, 20);

		for (e = intel_execution_engines; e->name; e++) {
			igt_subtest_f("preempt-%s", e->name)
				preempt(fd, e->exec_id | e->flags, ncpus, 150);
		}
	}

	igt_fixture {
		igt_stop_hang_detector();
		close(fd);
	}
}
