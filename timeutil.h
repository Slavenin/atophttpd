#ifndef ATOPHTTPD_TIMEUTIL_H
#define ATOPHTTPD_TIMEUTIL_H

#include <time.h>

/* Compare monotonic timestamps without involving wall-clock corrections. */
static inline int cache_refresh_due(struct timespec now, struct timespec last,
		unsigned int seconds)
{
	if (!last.tv_sec && !last.tv_nsec)
		return 1;
	if (now.tv_sec < last.tv_sec ||
	    (now.tv_sec == last.tv_sec && now.tv_nsec < last.tv_nsec))
		return 1;
	if (now.tv_sec - last.tv_sec > seconds)
		return 1;
	return now.tv_sec - last.tv_sec == seconds && now.tv_nsec >= last.tv_nsec;
}

#endif
