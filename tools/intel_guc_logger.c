/*
 * Copyright © 2014-2018 Intel Corporation
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
#include <stdio.h>
#include <fcntl.h>
#include <sched.h>
#include <sys/ioctl.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/poll.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <assert.h>
#include <pthread.h>

#include "igt.h"

#define MB(x) ((uint64_t)(x) * 1024 * 1024)
#ifndef PAGE_SIZE
  #define PAGE_SIZE 4096
#endif
/* Currently the size of GuC log buffer is 19 pages & so is the size of relay
 * subbuffer. If the size changes in future, then this define also needs to be
 * updated accordingly.
 */
#define SUBBUF_SIZE (19*PAGE_SIZE)
/* Need large buffering from logger side to hide the DISK IO latency, Driver
 * can only store 8 snapshots of GuC log buffer in relay.
 */
#define NUM_SUBBUFS 100

#define LOG_FILE_NAME  "guc_log0"
#define DEFAULT_OUTPUT_FILE_NAME  "guc_log_dump.dat"
#define CONTROL_FILE_NAME "i915_guc_log_relay"
#define LOG_LEVEL_FILE_NAME "i915_guc_log_level"
#define GUC_LOG_LEVEL_MIN 0
#define GUC_LOG_LEVEL_MAX 5

bool stop_logging;

struct guc_logger_options {
	int verbosity;
	unsigned test_duration;
	ssize_t max_filesize;
	char *out_filename;
};

struct guc_logger_thread_data {
	int out_fd;
	void *buffer;
	uint32_t produced;
	uint32_t consumed;
	pthread_mutex_t mutex;
	pthread_cond_t underflow_cond;
	pthread_cond_t overflow_cond;
};

static void guc_log_level(uint64_t verbosity)
{
	int log_level_fd;
	char data[3];
	int ret;

	log_level_fd = igt_debugfs_open(-1, LOG_LEVEL_FILE_NAME, O_WRONLY);
	assert(log_level_fd >= 0);

	ret = snprintf(data, sizeof(data), "%lu", verbosity);
	assert(ret > 0);

	ret = write(log_level_fd, data, ret);
	assert(ret > 0);

	close(log_level_fd);
}

static void int_sig_handler(int sig)
{
	igt_info("received signal %d\n", sig);

	stop_logging = true;
}

static ssize_t
pull_leftover_data(struct guc_logger_thread_data *data, int log_fd)
{
	unsigned int bytes_read = 0;
	int ret;

	do {
		/* Read the logs from relay buffer */
		ret = read(log_fd, data->buffer, SUBBUF_SIZE);
		if (!ret)
			break;

		igt_assert_f(ret > 0, "failed to read from the guc log file\n");
		igt_assert_f(ret == SUBBUF_SIZE, "invalid read from relay file\n");

		bytes_read += ret;

		if (data->out_fd >= 0) {
			ret = write(data->out_fd, data->buffer, SUBBUF_SIZE);
			igt_assert_f(ret == SUBBUF_SIZE, "couldn't dump the logs in a file\n");
		}
	} while(1);

	return bytes_read;
}

static int num_filled_bufs(struct guc_logger_thread_data *data)
{
	return (data->produced - data->consumed);
}

static ssize_t pull_data(struct guc_logger_thread_data *data, int log_fd)
{
	char *ptr;
	int ret;

	pthread_mutex_lock(&data->mutex);
	while (num_filled_bufs(data) >= NUM_SUBBUFS) {
		igt_debug("overflow, will wait, produced %u, consumed %u\n",
			  data->produced, data->consumed);
		/* Stall the main thread in case of overflow, as there are no
		 * buffers available to store the new logs, otherwise there
		 * could be corruption if both threads work on the same buffer.
		 */
		pthread_cond_wait(&data->overflow_cond, &data->mutex);
	};
	pthread_mutex_unlock(&data->mutex);

	ptr = (char*)data->buffer + (data->produced % NUM_SUBBUFS) * SUBBUF_SIZE;

	/* Read the logs from relay buffer */
	ret = read(log_fd, ptr, SUBBUF_SIZE);
	igt_assert_f(ret >= 0, "failed to read from the guc log file\n");
	igt_assert_f(!ret || ret == SUBBUF_SIZE, "invalid read from relay file\n");

	if (ret) {
		pthread_mutex_lock(&data->mutex);
		data->produced++;
		pthread_cond_signal(&data->underflow_cond);
		pthread_mutex_unlock(&data->mutex);
	} else {
		/* Occasionally (very rare) read from the relay file returns no
		 * data, albeit the polling done prior to read call indicated
		 * availability of data.
		 */
		igt_debug("no data read from the relay file\n");
	}

	return ret;
}

static void *flusher(void *arg)
{
	struct guc_logger_thread_data *data = arg;
	char *ptr;
	int ret;

	igt_debug("execution started of flusher thread\n");

	do {
		pthread_mutex_lock(&data->mutex);
		while (!num_filled_bufs(data)) {
			/* Exit only after completing the flush of all the filled
			 * buffers as User would expect that all logs captured up
			 * till the point of interruption/exit are written out to
			 * the disk file.
			 */
			if (stop_logging) {
				igt_debug("flusher to exit now\n");
				pthread_mutex_unlock(&data->mutex);
				return NULL;
			}
			pthread_cond_wait(&data->underflow_cond, &data->mutex);
		};
		pthread_mutex_unlock(&data->mutex);

		ptr = (char*)data->buffer + (data->consumed % NUM_SUBBUFS) * SUBBUF_SIZE;

		ret = write(data->out_fd, ptr, SUBBUF_SIZE);
		igt_assert_f(ret == SUBBUF_SIZE, "couldn't dump the logs in a file\n");

		pthread_mutex_lock(&data->mutex);
		data->consumed++;
		pthread_cond_signal(&data->overflow_cond);
		pthread_mutex_unlock(&data->mutex);
	} while(1);

	return NULL;
}

static void init_flusher_data(struct guc_logger_thread_data *data,
			      int out_fd, void *buffer)
{
	pthread_cond_init(&data->underflow_cond, NULL);
	pthread_cond_init(&data->overflow_cond, NULL);
	pthread_mutex_init(&data->mutex, NULL);
	data->out_fd = out_fd;
	data->buffer = buffer;
}

static void init_flusher_thread(pthread_t *flush_thread,
				struct guc_logger_thread_data *data)
{
	struct sched_param	thread_sched;
	pthread_attr_t		p_attr;
	int ret;

	ret = pthread_attr_init(&p_attr);
	igt_assert_f(ret == 0, "error obtaining default thread attributes\n");

	ret = pthread_attr_setinheritsched(&p_attr, PTHREAD_EXPLICIT_SCHED);
	igt_assert_f(ret == 0, "couldn't set inheritsched\n");

	ret = pthread_attr_setschedpolicy(&p_attr, SCHED_RR);
	igt_assert_f(ret == 0, "couldn't set thread scheduling policy\n");

	/* Keep the flusher task also at rt priority, so that it doesn't get
	 * too late in flushing the collected logs in local buffers to the disk,
	 * and so main thread always have spare buffers to collect the logs.
	 */
	thread_sched.sched_priority = 5;
	ret = pthread_attr_setschedparam(&p_attr, &thread_sched);
	igt_assert_f(ret == 0, "couldn't set thread priority\n");

	ret = pthread_create(flush_thread, &p_attr, flusher, data);
	igt_assert_f(ret == 0, "thread creation failed\n");

	ret = pthread_attr_destroy(&p_attr);
	igt_assert_f(ret == 0, "error destroying thread attributes\n");
}

static void init_main_thread(void)
{
	struct sched_param thread_sched;
	int ret;

	/* Run the main thread at highest priority to ensure that it always
	 * gets woken-up at earliest on arrival of new data and so is always
	 * ready to pull the logs, otherwise there could be loss logs if
	 * GuC firmware is generating logs at a very high rate.
	 */
	thread_sched.sched_priority = 1;
	ret = sched_setscheduler(getpid(), SCHED_FIFO, &thread_sched);
	igt_assert_f(ret == 0, "couldn't set the priority\n");
}

static void print_usage(FILE *out, char **argv)
{
	const char *help =
		"  -v --verbosity=level   verbosity level of GuC logging\n"
		"  -o --outputfile=name   name of the output file, including the location, where logs will be stored\n"
		"  -t --testduration=sec  max duration in seconds for which the logger should run\n"
		"  -s --size=MB           max size of output file in MBs after which logging will be stopped\n";

	fprintf(out, "Usage: %s [OPTIONS]\n", argv[0]);
	fprintf(out, "%s", help);
}

static void
process_command_line(int argc, char **argv, struct guc_logger_options *opts)
{
	static struct option long_options[] = {
		{"verbosity", required_argument, 0, 'v'},
		{"outputfile", required_argument, 0, 'o'},
		{"testduration", required_argument, 0, 't'},
		{"size", required_argument, 0, 's'},
		{"help", no_argument, 0, 'h'},
		{ 0, 0, 0, 0 }
	};

	int option_index = 0;
	int c;
	opts->verbosity = -1;

	while ((c = getopt_long(argc, argv, "v:o:t:s:h",
				long_options, &option_index)) != -1) {
		switch (c) {
		case 'v':
			opts->verbosity = atoi(optarg);
			if (opts->verbosity < GUC_LOG_LEVEL_MIN ||
			    opts->verbosity > GUC_LOG_LEVEL_MAX) {
				fprintf(stderr, "invalid input for -v option\n");
				goto out;
			}
			break;
		case 'o':
			opts->out_filename = strdup(optarg);
			if (!opts->out_filename) {
				fprintf(stderr, "Couldn't allocate the o/p filename\n");
				goto out;
			}
			break;
		case 't':
			opts->test_duration = atoi(optarg);
			if (opts->test_duration < 0) {
				fprintf(stderr, "invalid input for -t option\n");
				goto out;
			}
			break;
		case 's':
			opts->max_filesize = atoi(optarg);
			if (opts->max_filesize < 0) {
				fprintf(stderr, "invalid input for -s option\n");
				goto out;
			}
			break;
		case 'h':
			print_usage(stdout, argv);
			exit(EXIT_SUCCESS);
		case '?':
			fprintf(stderr, "Argument: %c expects a value\n\n", c);
			goto out;
		case ':':
			fprintf(stderr, "Invalid argument: %c\n\n", c);
			goto out;
		default:
			fprintf(stderr, "Error\n\n");
			goto out;
		}
	}

	return;

out:
	print_usage(stderr, argv);
	exit(EXIT_FAILURE);
}

int main(int argc, char **argv)
{
	struct guc_logger_options opts = { 0 };
	struct guc_logger_thread_data data = { 0 };
	int out_fd;
	void *buffer;
	ssize_t total_bytes = 0;
	struct pollfd log_poll_fd;
	pthread_t flush_thread;
	int nfds;
	int log_fd, control_fd;
	int ret;

	process_command_line(argc, argv, &opts);

	if (opts.verbosity != -1)
		guc_log_level(opts.verbosity);

	out_fd = open(opts.out_filename ? : DEFAULT_OUTPUT_FILE_NAME,
		      O_CREAT | O_WRONLY | O_TRUNC | O_DIRECT, 0440);
	assert(out_fd >= 0);
	free(opts.out_filename);

	if (signal(SIGINT, int_sig_handler) == SIG_ERR)
		igt_assert_f(0, "SIGINT handler registration failed\n");

	if (signal(SIGALRM, int_sig_handler) == SIG_ERR)
		igt_assert_f(0, "SIGALRM handler registration failed\n");

	/* Need an aligned pointer for direct IO */
	ret = posix_memalign((void **)&buffer, PAGE_SIZE,
			     NUM_SUBBUFS * SUBBUF_SIZE);
	igt_assert_f(ret == 0, "couldn't allocate the read buffer\n");

	/* Keep the pages locked in RAM, avoid page fault overhead */
	ret = mlock(buffer, NUM_SUBBUFS * SUBBUF_SIZE);
	igt_assert_f(ret == 0, "failed to lock memory\n");

	/* Use a separate thread for flushing the logs to a file on disk.
	 * Main thread will buffer the data from relay file in its pool of
	 * buffers and other thread will flush the data to disk in background.
	 * This is needed, albeit by default data is written out to disk in
	 * async mode, as when there are too many dirty pages in the RAM,
	 * (/proc/sys/vm/dirty_ratio), kernel starts blocking the processes
	 * doing the file writes.
	 */
	init_flusher_data(&data, out_fd, buffer);
	init_flusher_thread(&flush_thread, &data);
	init_main_thread();

	control_fd = igt_debugfs_open(-1, CONTROL_FILE_NAME, O_RDWR);
	assert(control_fd >= 0);

	log_fd = igt_debugfs_open(-1, LOG_FILE_NAME, O_RDONLY);
	assert(log_fd >= 0);

	memset(&log_poll_fd, 0, sizeof(log_poll_fd));
	log_poll_fd.fd = log_fd;
	log_poll_fd.events = POLLIN;
	nfds = 1; /* only one fd to poll */

	alarm(opts.test_duration); /* Start the alarm */

	do {
		ret = poll(&log_poll_fd, nfds, -1);
		if (ret < 0) {
			if (errno == EINTR)
				break;
			igt_assert_f(0, "poll call failed\n");
		}

		total_bytes += pull_data(&data, log_fd);
		if (opts.max_filesize && (total_bytes > MB(opts.max_filesize))) {
			igt_debug("reached the target of %lu bytes\n", MB(opts.max_filesize));
			stop_logging = true;
		}
	} while (!stop_logging);

	pthread_cond_signal(&data.underflow_cond);
	pthread_join(flush_thread, NULL);

	write(control_fd, "", 1);

	total_bytes += pull_leftover_data(&data, log_fd);
	igt_info("total bytes written %lu\n", total_bytes);

	close(log_fd);
	close(control_fd);
	close(out_fd);
}
