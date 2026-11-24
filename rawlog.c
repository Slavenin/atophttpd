/*
 * Copyright 2022 zhenwei pi
 *
 * Authors:
 *   zhenwei pi <pizhenwei@bytedance.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/types.h>
#include <stdint.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

#include "config.h"

#include "atop.h"
#include "cgroups.h"
#include "cache.h"
#include "httpd.h"
#include "json.h"
#include "rawlog.h"
#include "sample.h"
#include "version.h"

/* a little tricky: implemented in atop/version.c */
unsigned short getnumvers(void);
static int rawlog_error;
static struct {
	struct raw_sample sample;
	char *path;
	dev_t dev;
	ino_t ino;
	off_t off, size;
	struct timespec mtime, ctime;
	uint64_t generation;
	int valid;
} decoded;

static void rawlog_drop_decoded(void)
{
	if (decoded.valid)
		rawlog_sample_free(&decoded.sample);
	free(decoded.path);
	decoded.path = NULL;
	decoded.valid = 0;
}

static void rawlog_mark_fault(const char *path, int error, time_t record_time)
{
	struct cache_t *cache = cache_find(path);
	if (!cache)
		cache = cache_alloc(path);
	if (cache->error_time > record_time)
		record_time = cache->error_time;
	if (cache->nr_elems) {
		time_t previous = cache->elems[cache->nr_elems - 1].time;
		if (previous > record_time)
			record_time = previous;
	}
	cache_clear(cache);
	cache->error_time = record_time;
	cache->error_code = error;
	rawlog_drop_decoded();
}

static int rawlog_verify_rawheader(struct rawheader *rh)
{
	if (rh->magic != MYMAGIC)
		return -EINVAL;

	if ((rh->rawheadlen != sizeof(struct rawheader)) ||
		(rh->rawreclen != sizeof(struct rawrecord)) ||
		(rh->sstatlen != sizeof(struct sstat)) ||
		(rh->tstatlen != sizeof(struct tstat)) ||
		(rh->cstatlen != sizeof(struct cstat))) {
		return -EINVAL;
	}

	if (!(rh->aversion & 0x8000) || (rh->aversion & 0x7fff) != getnumvers() ||
	    !rh->pagesize || !rh->hertz)
		return -EINVAL;

	return 0;
}

static int rawlog_scan(int fd, struct cache_t *cache, off_t file_size)
{
	off_t off = cache->scan_off;
	struct rawrecord rr;

	while (file_size - off >= (off_t)sizeof(rr)) {
		if (pread(fd, &rr, sizeof(rr), off) != sizeof(rr))
			return -EIO;

		uint64_t length = (uint64_t)sizeof(rr) + rr.scomplen + rr.pcomplen +
				rr.ccomplen + rr.icomplen;
		if (!rr.scomplen || !rr.pcomplen ||
		    rr.scomplen > 1024U * 1024 * 1024 ||
		    rr.pcomplen > 1024U * 1024 * 1024 ||
		    rr.ccomplen > 1024U * 1024 * 1024 ||
		    rr.icomplen > 1024U * 1024 * 1024 ||
		    rr.ndeviat > 1000000 || length > INT64_MAX) {
			if (rr.curtime > cache->error_time)
				cache->error_time = rr.curtime;
			return -EINVAL;
		}
		if (length > (uint64_t)(file_size - off))
			break; /* writer has not completed this record yet */
		cache_set(cache, rr.curtime, off);
		off += length;
	}
	cache->scan_off = off;
	cache->scan_tail_len = off < (off_t)sizeof(cache->scan_tail) ?
		(size_t)off : sizeof(cache->scan_tail);
	if (cache->scan_tail_len &&
	    pread(fd, cache->scan_tail, cache->scan_tail_len,
		 off - cache->scan_tail_len) != (ssize_t)cache->scan_tail_len)
		return -EIO;
	cache->scan_head_len = off - (off_t)sizeof(struct rawheader) <
		(off_t)sizeof(cache->scan_head) ?
		(size_t)(off - sizeof(struct rawheader)) : sizeof(cache->scan_head);
	if (cache->scan_head_len &&
	    pread(fd, cache->scan_head, cache->scan_head_len,
		 sizeof(struct rawheader)) != (ssize_t)cache->scan_head_len)
		return -EIO;
	return 0;
}

static int rawlog_prefix_unchanged(int fd, const struct cache_t *cache)
{
	unsigned char tail[sizeof(cache->scan_tail)];
	unsigned char head[sizeof(cache->scan_head)];
	if (!cache->scan_tail_len)
		return 0;
	if (pread(fd, tail, cache->scan_tail_len,
		 cache->scan_off - cache->scan_tail_len) !=
	    (ssize_t)cache->scan_tail_len)
		return 0;
	if (memcmp(tail, cache->scan_tail, cache->scan_tail_len))
		return 0;
	if (cache->scan_head_len &&
	    (pread(fd, head, cache->scan_head_len, sizeof(struct rawheader)) !=
	     (ssize_t)cache->scan_head_len ||
	     memcmp(head, cache->scan_head, cache->scan_head_len)))
		return 0;
	return 1;
}

static int rawlog_parse_one(const char *path)
{
	struct rawheader rh;
	struct stat statbuf;
	struct cache_t *cache;
	int fd;
	int ret;

	fd = open(path, O_RDONLY);
	if (fd < 0) {
		int error = -errno;
		if (errno == ENOENT)
			cache_free(path);
		else
			rawlog_mark_fault(path, error, 0);
		return error;
	}
	if (fstat(fd, &statbuf) < 0) {
		ret = -errno;
		rawlog_mark_fault(path, ret, 0);
		goto close_fd;
	}
	if (statbuf.st_size < (off_t)sizeof(rh)) {
		if (decoded.path && !strcmp(decoded.path, path))
			rawlog_drop_decoded();
		cache_free(path);
		ret = 0; /* log header is still being written */
		goto close_fd;
	}
	if (pread(fd, &rh, sizeof(rh), 0) != sizeof(rh)) {
		ret = -EIO;
		rawlog_mark_fault(path, ret, 0);
		goto close_fd;
	}
	if (rh.magic != MYMAGIC) {
		if (cache_find(path)) {
			rawlog_mark_fault(path, -EINVAL, 0);
			ret = -EINVAL;
		} else {
			ret = 0; /* other file in the log directory */
		}
		goto close_fd;
	}
	ret = rawlog_verify_rawheader(&rh);
	if (ret) {
		rawlog_error = 1;
		fprintf(stderr, "%s: incompatible atop rawlog %s (need atop %s layout; use atopconvert)\n",
			__func__, path, ATOPVERS);
		rawlog_mark_fault(path, ret, 0);
		goto close_fd;
	}
	cache = cache_find(path);
	if (!cache)
		cache = cache_alloc(path);
	if (cache->st_ino != statbuf.st_ino || cache->st_dev != statbuf.st_dev ||
	    statbuf.st_size < cache->st_size ||
	    (statbuf.st_size == cache->st_size &&
	     (statbuf.st_mtim.tv_sec != cache->st_mtim.tv_sec ||
	      statbuf.st_mtim.tv_nsec != cache->st_mtim.tv_nsec ||
	      statbuf.st_ctim.tv_sec != cache->st_ctim.tv_sec ||
	      statbuf.st_ctim.tv_nsec != cache->st_ctim.tv_nsec)) ||
	    (statbuf.st_size > cache->st_size &&
	     !rawlog_prefix_unchanged(fd, cache))) {
		if (cache->nr_elems) {
			time_t previous = cache->elems[cache->nr_elems - 1].time;
			if (previous > cache->error_time)
				cache->error_time = previous;
		}
		cache_clear(cache);
	}
	if (!cache->scan_off)
		cache->scan_off = sizeof(rh);
	cache->flags = rh.supportflags;
	cache->pagesize = rh.pagesize;
	cache->hertz = rh.hertz;
	cache->utsname = rh.utsname;
	ret = rawlog_scan(fd, cache, statbuf.st_size);
	if (ret) {
		rawlog_error = 1;
		fprintf(stderr, "%s: invalid record in %s\n", __func__, path);
		rawlog_mark_fault(path, ret, cache->error_time);
		goto close_fd;
	}
	cache->error_code = 0;
	cache->error_time = 0;
	cache->st_size = statbuf.st_size;
	cache->st_mtim = statbuf.st_mtim;
	cache->st_ctim = statbuf.st_ctim;
	cache->st_dev = statbuf.st_dev;
	cache->st_ino = statbuf.st_ino;

close_fd:
	close(fd);

	return ret;
}

int rawlog_parse_all(const char *path)
{
	struct dirent *dirent;
	DIR *dir = opendir(path);
	char name[PATH_MAX] = {0};

	if (!dir) {
		printf("%s: open \"%s\" failed: %m\n", __func__, path);
		return -errno;
	}
	rawlog_error = 0;

	while ((dirent = readdir(dir))) {
		if (dirent->d_type != DT_REG && dirent->d_type != DT_UNKNOWN)
			continue;

		if (snprintf(name, sizeof(name), "%s/%s", path, dirent->d_name) >= sizeof(name))
			continue;
		struct stat st;
		if (stat(name, &st) < 0 || !S_ISREG(st.st_mode))
			continue;
		rawlog_parse_one(name);
	}

	cache_prune_missing();
	cache_sort();

	closedir(dir);

	return 0;
}

static int rawlog_uncompress_record(int fd, void *outbuf, unsigned long *outlen, unsigned long inlen)
{
	Byte *inbuf;
	int ret = 0;

	if (!inlen || inlen > 1024UL * 1024 * 1024)
		return -EINVAL;
	inbuf = malloc(inlen);
	if (!inbuf)
		return -ENOMEM;

	if (read(fd, inbuf, inlen) != inlen) {
		ret = -EIO;
		goto free_buf;
	}

	unsigned long expected = *outlen;
	if (!expected)
		*outlen = 1;
	if (uncompress(outbuf, outlen, inbuf, inlen) != Z_OK || *outlen != expected)
		ret = -ENODATA;

free_buf:
	free(inbuf);

	return ret;
}

static int rawlog_get_sstat(int fd, struct sstat *sstat, unsigned long len)
{
	unsigned long outlen = sizeof(struct sstat);

	return rawlog_uncompress_record(fd, sstat, &outlen, len);
}

static void rawlog_free_devtstat(struct devtstat *devtstat)
{
	free(devtstat->taskall);
	free(devtstat->procall);
	free(devtstat->procactive);
}

static int rawlog_get_devtstat(int fd, struct devtstat *devtstat, struct rawrecord *rr)
{
	unsigned long outlen = sizeof(struct tstat) * rr->ndeviat;
	int ret;
	unsigned long ntaskall = 0, nprocall = 0, nprocactive = 0, ntaskactive = 0;

	/* 1, allocate memory */
	memset(devtstat, 0x00, sizeof(struct devtstat));
	if (rr->totproc > rr->ndeviat || rr->nactproc > rr->totproc ||
	    rr->ndeviat > 1000000)
		return -EINVAL;
	devtstat->taskall = malloc(outlen ? outlen : 1);
	if (!devtstat->taskall) {
		ret = -ENOMEM;
		goto free_devtstat;
	}

	devtstat->procall = malloc(sizeof(struct tstat*) * (rr->totproc ? rr->totproc : 1));
	if (!devtstat->procall) {
		ret = -ENOMEM;
		goto free_devtstat;
	}

	devtstat->procactive = malloc(sizeof(struct tstat*) * (rr->nactproc ? rr->nactproc : 1));
	if (!devtstat->procactive) {
		ret = -ENOMEM;
		goto free_devtstat;
	}

	/* 2, read record and uncompress */
	ret = rawlog_uncompress_record(fd, devtstat->taskall, &outlen, rr->pcomplen);
	if (ret)
		goto free_devtstat;

	/* 3, build devtstat */
	for ( ; ntaskall < rr->ndeviat; ntaskall++)
	{
		struct tstat *tstat = devtstat->taskall + ntaskall;
		if (tstat->gen.isproc)
		{
			if (nprocall >= rr->totproc ||
			    (!tstat->gen.wasinactive && nprocactive >= rr->nactproc)) {
				ret = -EINVAL;
				goto free_devtstat;
			}
			devtstat->procall[nprocall++] = tstat;
			if (!tstat->gen.wasinactive)
				devtstat->procactive[nprocactive++] = tstat;
		}

		if (!tstat->gen.wasinactive)
			ntaskactive++;
	}

	if (nprocall != rr->totproc || nprocactive != rr->nactproc) {
		ret = -EINVAL;
		goto free_devtstat;
	}
	devtstat->ntaskall = ntaskall;
	devtstat->nprocall = nprocall;
	devtstat->nprocactive = nprocactive;
	devtstat->ntaskactive = ntaskactive;
	devtstat->totrun = rr->totrun;
	devtstat->totslpi = rr->totslpi;
	devtstat->totslpu = rr->totslpu;
	devtstat->totzombie = rr->totzomb;


	return 0;

free_devtstat:
	rawlog_free_devtstat(devtstat);

	return ret;
}

static int rawlog_record_flags(int hflags, int rflags)
{
	int ret = hflags & ~(ACCTACTIVE | IOSTAT | NETATOP | NETATOPD |
		CONTAINERSTAT | GPUSTAT | CGROUPV2);

	if ((hflags & RAWLOGNG) == RAWLOGNG) {
		if (rflags & RRACCTACTIVE)
			ret |= ACCTACTIVE;

		if (rflags & RRIOSTAT)
			ret |= IOSTAT;
	}

	if (rflags & RRNETATOP)
		ret |= NETATOP;

	if (rflags & RRNETATOPD)
		ret |= NETATOPD;

	if (rflags & RRCGRSTAT)
		ret |= CGROUPV2;

	if (rflags & RRCONTAINERSTAT)
		ret |= CONTAINERSTAT;

	if (rflags & RRGPUSTAT)
		ret |= GPUSTAT;

	if (hflags & NETATOPBPF)
		ret |= NETATOPBPF;

	return ret;
}

static int rawlog_stat_matches(const struct cache_t *cache, const struct stat *st)
{
	return st->st_dev == cache->st_dev && st->st_ino == cache->st_ino &&
		st->st_size == cache->st_size &&
		st->st_mtim.tv_sec == cache->st_mtim.tv_sec &&
		st->st_mtim.tv_nsec == cache->st_mtim.tv_nsec &&
		st->st_ctim.tv_sec == cache->st_ctim.tv_sec &&
		st->st_ctim.tv_nsec == cache->st_ctim.tv_nsec;
}

static int rawlog_load(struct cache_t *cache, off_t off, struct raw_sample *sample)
{
	struct rawrecord rr;
	struct devtstat devtstat;
	struct stat st;
	int fd = open(cache->name, O_RDONLY);
	int ret;

	memset(sample, 0, sizeof(*sample));
	if (fd < 0)
		return -errno;
	if (fstat(fd, &st) < 0) {
		ret = -errno;
		goto close_fd;
	}
	if (!rawlog_stat_matches(cache, &st) || st.st_size < cache->scan_off) {
		ret = -EAGAIN;
		goto close_fd;
	}
	if (pread(fd, &rr, sizeof(rr), off) != sizeof(rr)) {
		ret = -EIO;
		goto close_fd;
	}
	uint64_t length = (uint64_t)sizeof(rr) + rr.scomplen + rr.pcomplen +
			rr.ccomplen + rr.icomplen;
	if (!rr.scomplen || !rr.pcomplen || rr.ndeviat > 1000000 ||
	    length > (uint64_t)(st.st_size - off)) {
		ret = -EINVAL;
		goto close_fd;
	}
	if (lseek(fd, off + sizeof(rr), SEEK_SET) < 0) {
		ret = -errno;
		goto close_fd;
	}
	sample->system = malloc(sizeof(*sample->system));
	if (!sample->system) {
		ret = -ENOMEM;
		goto close_fd;
	}
	ret = rawlog_get_sstat(fd, sample->system, rr.scomplen);
	if (ret)
		goto close_fd;
	ret = rawlog_get_devtstat(fd, &devtstat, &rr);
	if (ret)
		goto close_fd;
	sample->tasks = devtstat.taskall;
	sample->ntasks = devtstat.ntaskall;
	sample->nprocall = devtstat.nprocall;
	sample->nprocactive = devtstat.nprocactive;
	sample->ntaskactive = devtstat.ntaskactive;
	sample->totrun = devtstat.totrun;
	sample->totslpi = devtstat.totslpi;
	sample->totslpu = devtstat.totslpu;
	sample->totzombie = devtstat.totzombie;
	sample->nexit = rr.nexit;
	sample->noverflow = rr.noverflow;
	free(devtstat.procall);
	free(devtstat.procactive);
	sample->timestamp = rr.curtime;
	sample->interval = rr.interval;
	sample->flags = rawlog_record_flags(cache->flags, rr.flags);
	sample->record_flags = rr.flags;
	sample->pagesize = cache->pagesize;
	sample->hertz = cache->hertz;
	sample->utsname = cache->utsname;
	if (fstat(fd, &st) < 0 || !rawlog_stat_matches(cache, &st))
		ret = -EAGAIN;
close_fd:
	close(fd);
	if (ret)
		rawlog_sample_free(sample);
	return ret;
}

void rawlog_sample_free(struct raw_sample *sample)
{
	free(sample->system);
	free(sample->tasks);
	memset(sample, 0, sizeof(*sample));
}

static int rawlog_select(time_t *timestamp, struct cache_t **cache, off_t *off)
{
	if (timestamp) {
		*cache = cache_get(*timestamp, off);
		if (*cache)
			return 0;
	}
	*cache = cache_get_recent();
	if (!*cache)
		return rawlog_error ? -EINVAL : -ENOENT;
	if (timestamp && *timestamp < (*cache)->elems[(*cache)->nr_elems - 1].time)
		return -ENOENT;
	*off = (*cache)->elems[(*cache)->nr_elems - 1].off;
	return 0;
}

static int rawlog_acquire(time_t *timestamp, const struct raw_sample **sample,
		uint64_t *generation)
{
	off_t off;
	struct cache_t *cache;
	*sample = NULL;
	for (int attempt = 0; attempt < 3; attempt++) {
		if (!timestamp) {
			struct cache_t *recent = cache_get_recent();
			time_t latest = recent ?
				recent->elems[recent->nr_elems - 1].time : 0;
			const char *fault_path;
			int fault = cache_recent_error(latest, &fault_path);
			if (fault) {
				char *path = strdup(fault_path);
				if (!path)
					return -ENOMEM;
				int refresh = rawlog_parse_one(path);
				free(path);
				if (refresh == -ENOENT)
					continue;
				if (refresh) {
					rawlog_drop_decoded();
					return refresh;
				}
				continue;
			}
		}
		int ret = rawlog_select(timestamp, &cache, &off);
		if (ret) {
			rawlog_drop_decoded();
			return ret;
		}
		/* Reopen on every request: a replaced or rewritten log must not reuse bytes. */
		int fd = open(cache->name, O_RDONLY);
		struct stat st;
		if (fd < 0 || fstat(fd, &st) < 0) {
			int error = -errno;
			if (fd >= 0)
				close(fd);
			rawlog_drop_decoded();
			if (error == -ENOENT)
				cache_free(cache->name);
			else
				rawlog_mark_fault(cache->name, error, 0);
			return error;
		}
		close(fd);
		if (!rawlog_stat_matches(cache, &st)) {
			char *path = strdup(cache->name);
			if (!path)
				return -ENOMEM;
			ret = rawlog_parse_one(path);
			free(path);
			if (ret && ret != -EAGAIN) {
				rawlog_drop_decoded();
				return ret;
			}
			continue;
		}
		if (decoded.valid && decoded.off == off && decoded.dev == st.st_dev &&
		    decoded.ino == st.st_ino && decoded.size == st.st_size &&
		    decoded.mtime.tv_sec == st.st_mtim.tv_sec &&
		    decoded.mtime.tv_nsec == st.st_mtim.tv_nsec &&
		    decoded.ctime.tv_sec == st.st_ctim.tv_sec &&
		    decoded.ctime.tv_nsec == st.st_ctim.tv_nsec &&
		    !strcmp(decoded.path, cache->name)) {
			*sample = &decoded.sample;
			*generation = decoded.generation;
			return 0;
		}
		struct raw_sample next;
		ret = rawlog_load(cache, off, &next);
		if (ret) {
			rawlog_drop_decoded();
			if (ret == -EAGAIN)
				continue;
			return ret;
		}
		char *path = strdup(cache->name);
		if (!path) {
			rawlog_sample_free(&next);
			rawlog_drop_decoded();
			return -ENOMEM;
		}
		rawlog_drop_decoded();
		decoded.sample = next;
		decoded.path = path;
		decoded.dev = st.st_dev;
		decoded.ino = st.st_ino;
		decoded.off = off;
		decoded.size = st.st_size;
		decoded.mtime = st.st_mtim;
		decoded.ctime = st.st_ctim;
		decoded.valid = 1;
		decoded.generation++;
		*sample = &decoded.sample;
		*generation = decoded.generation;
		return 0;
	}
	rawlog_drop_decoded();
	return -EAGAIN;
}

int rawlog_get_recent_sample(const struct raw_sample **sample, uint64_t *generation)
{
	return rawlog_acquire(NULL, sample, generation);
}

int rawlog_get_record(time_t ts, char *labels, connection *conn)
{
	const struct raw_sample *sample;
	uint64_t generation;
	int ret = rawlog_acquire(&ts, &sample, &generation);
	if (ret)
		return ret;
	struct devtstat dev = {.taskall = sample->tasks, .ntaskall = sample->ntasks,
		.nprocall = sample->nprocall, .nprocactive = sample->nprocactive,
		.ntaskactive = sample->ntaskactive, .totrun = sample->totrun,
		.totslpi = sample->totslpi, .totslpu = sample->totslpu,
		.totzombie = sample->totzombie};
	pagesize = sample->pagesize;
	hertz = sample->hertz;
	utsname = sample->utsname;
	jsonout(sample->flags, labels, sample->timestamp, sample->interval,
		&dev, sample->system, sample->nexit, sample->noverflow, 0, conn);
	return 0; /* jsonout already sent the response, including label errors */
}
