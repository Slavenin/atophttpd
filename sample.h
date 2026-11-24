#ifndef ATOPHTTPD_SAMPLE_H
#define ATOPHTTPD_SAMPLE_H

#include <sys/utsname.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include "config.h"
#include "photosyst.h"
#include "photoproc.h"

struct raw_sample {
	struct sstat *system;
	struct tstat *tasks;
	unsigned int ntasks;
	unsigned int nprocall;
	unsigned int nprocactive;
	unsigned int ntaskactive;
	unsigned int totrun, totslpi, totslpu, totzombie;
	unsigned int nexit, noverflow;
	time_t timestamp;
	unsigned int interval;
	unsigned int flags;
	unsigned int record_flags;
	unsigned int pagesize;
	unsigned short hertz;
	struct utsname utsname;
};

int rawlog_get_recent_sample(const struct raw_sample **sample, uint64_t *generation);
void rawlog_sample_free(struct raw_sample *sample);
struct metrics_status {
	long long age;
	int fresh;
	int available;
	int process_visible;
};
void metrics_sample_status(const struct raw_sample *sample, unsigned int stale_after,
		time_t now, struct metrics_status *status);
int metrics_render(const struct raw_sample *sample, uint64_t generation, unsigned int stale_after,
		char **body, size_t *length);

#endif
