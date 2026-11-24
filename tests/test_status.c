#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include "atop.h"
#include "sample.h"
#include "timeutil.h"

int main(void)
{
	struct raw_sample sample = {0};
	struct metrics_status status;
	sample.timestamp = 100;
	sample.interval = 10;
	sample.flags = NETATOPBPF;
	metrics_sample_status(&sample, 2, 99, &status);
	assert(status.age == 0 && status.fresh && status.available &&
		status.process_visible);
	metrics_sample_status(&sample, 2, 103, &status);
	assert(status.age == 3 && !status.fresh && !status.process_visible);
	sample.record_flags = RRBOOT;
	metrics_sample_status(&sample, 2, 101, &status);
	assert(status.fresh && status.available && !status.process_visible);
	sample.interval = 0;
	metrics_sample_status(&sample, 2, 101, &status);
	assert(!status.fresh && !status.process_visible);
	metrics_sample_status(NULL, 0, 100, &status);
	assert(!status.age && !status.fresh && !status.available);

	struct timespec last = {.tv_sec = 100, .tv_nsec = 500000000};
	assert(!cache_refresh_due((struct timespec){102, 999999999}, last, 3));
	assert(cache_refresh_due((struct timespec){103, 500000000}, last, 3));
	assert(cache_refresh_due((struct timespec){99, 0}, last, 3));
	assert(cache_refresh_due((struct timespec){100, 0},
		(struct timespec){0, 0}, 3));
	return 0;
}
