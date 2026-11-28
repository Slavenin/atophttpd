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
#include <limits.h>
#include <linux/types.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

#include "config.h"

#include "atop.h"
#include "cache.h"
#include "cgroups.h"
#include "httpd.h"
#include "json.h"
#include "log.h"
#include "rawlog.h"
#include "sample.h"
#include "version.h"

/* a little tricky: implemented in atop/version.c */
unsigned short getnumvers(void);

/* The index is protected by cache_rwlock; the decoded latest sample belongs to
 * the single HTTP event loop and is never handed to upstream's mutating JSON. */
static int rawlog_error;
static pthread_mutex_t json_render_lock = PTHREAD_MUTEX_INITIALIZER;
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

void rawlog_reset(void)
{
    /* The decoded sample belongs to the event loop, unlike the shared index. */
    rawlog_drop_decoded();
}

static void rawlog_mark_fault(const char *path, int error, time_t record_time)
{
    struct cache_t *cache = cache_find(path);
    if (!cache)
        cache = cache_alloc(path);
    if (!cache) {
        rawlog_error = 1;
        return;
    }
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
}

static int rawlog_verify_rawheader(struct rawheader *rh)
{
    if (rh->magic != MYMAGIC || rh->rawheadlen != sizeof(struct rawheader) ||
        rh->rawreclen != sizeof(struct rawrecord) || rh->sstatlen != sizeof(struct sstat) ||
        rh->tstatlen != sizeof(struct tstat) || rh->cstatlen != sizeof(struct cstat) ||
        !(rh->aversion & 0x8000) || (rh->aversion & 0x7fff) != getnumvers() || !rh->pagesize ||
        !rh->hertz)
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
        uint64_t length =
            (uint64_t)sizeof(rr) + rr.scomplen + rr.pcomplen + rr.ccomplen + rr.icomplen;
        if (!rr.scomplen || !rr.pcomplen || rr.scomplen > 1024U * 1024 * 1024 ||
            rr.pcomplen > 1024U * 1024 * 1024 || rr.ccomplen > 1024U * 1024 * 1024 ||
            rr.icomplen > 1024U * 1024 * 1024 || rr.ndeviat > 1000000 || length > INT64_MAX) {
            if (rr.curtime > cache->error_time)
                cache->error_time = rr.curtime;
            return -EINVAL;
        }
        if (length > (uint64_t)(file_size - off))
            break; /* the writer has not finished the four-block record */
        if (cache_set(cache, rr.curtime, off) < 0)
            return -ENOMEM;
        off += length;
    }
    cache->scan_off = off;
    cache->scan_tail_len = off < (off_t)sizeof(cache->scan_tail) ? (size_t)off
                                                                 : sizeof(cache->scan_tail);
    if (cache->scan_tail_len && pread(fd, cache->scan_tail, cache->scan_tail_len,
                                      off - cache->scan_tail_len) != (ssize_t)cache->scan_tail_len)
        return -EIO;
    cache->scan_head_len = off - (off_t)sizeof(struct rawheader) < (off_t)sizeof(cache->scan_head)
                               ? (size_t)(off - sizeof(struct rawheader))
                               : sizeof(cache->scan_head);
    if (cache->scan_head_len && pread(fd, cache->scan_head, cache->scan_head_len,
                                      sizeof(struct rawheader)) != (ssize_t)cache->scan_head_len)
        return -EIO;
    return 0;
}

static int rawlog_prefix_unchanged(int fd, const struct cache_t *cache)
{
    unsigned char tail[sizeof(cache->scan_tail)];
    unsigned char head[sizeof(cache->scan_head)];
    if (!cache->scan_tail_len ||
        pread(fd, tail, cache->scan_tail_len, cache->scan_off - cache->scan_tail_len) !=
            (ssize_t)cache->scan_tail_len ||
        memcmp(tail, cache->scan_tail, cache->scan_tail_len))
        return 0;
    if (cache->scan_head_len && (pread(fd, head, cache->scan_head_len, sizeof(struct rawheader)) !=
                                     (ssize_t)cache->scan_head_len ||
                                 memcmp(head, cache->scan_head, cache->scan_head_len)))
        return 0;
    return 1;
}

/* Caller holds the cache write lock. */
static int rawlog_parse_one(const char *path)
{
    struct rawheader rh;
    struct stat st;
    int fd = open(path, O_RDONLY);
    int ret;
    if (fd < 0) {
        ret = -errno;
        if (errno == ENOENT)
            cache_free(path);
        else
            rawlog_mark_fault(path, ret, 0);
        return ret;
    }
    if (fstat(fd, &st) < 0) {
        ret = -errno;
        rawlog_mark_fault(path, ret, 0);
        goto done;
    }
    if (st.st_size < (off_t)sizeof(rh)) {
        cache_free(path);
        ret = 0;
        goto done;
    }
    if (pread(fd, &rh, sizeof(rh), 0) != sizeof(rh)) {
        ret = -EIO;
        rawlog_mark_fault(path, ret, 0);
        goto done;
    }
    if (rh.magic != MYMAGIC) {
        if (cache_find(path)) {
            rawlog_mark_fault(path, -EINVAL, 0);
            ret = -EINVAL;
        } else {
            ret = 0; /* unrelated file */
        }
        goto done;
    }
    ret = rawlog_verify_rawheader(&rh);
    if (ret) {
        rawlog_error = 1;
        log_warn("incompatible atop rawlog %s (need atop %s layout; use atopconvert)\n", path,
                 ATOPVERS);
        rawlog_mark_fault(path, ret, 0);
        goto done;
    }
    struct cache_t *cache = cache_find(path);
    if (!cache)
        cache = cache_alloc(path);
    if (!cache) {
        ret = -ENOMEM;
        goto done;
    }
    if (cache->st_ino != st.st_ino || cache->st_dev != st.st_dev || st.st_size < cache->st_size ||
        (st.st_size == cache->st_size && (st.st_mtim.tv_sec != cache->st_mtim.tv_sec ||
                                          st.st_mtim.tv_nsec != cache->st_mtim.tv_nsec ||
                                          st.st_ctim.tv_sec != cache->st_ctim.tv_sec ||
                                          st.st_ctim.tv_nsec != cache->st_ctim.tv_nsec)) ||
        (st.st_size > cache->st_size && !rawlog_prefix_unchanged(fd, cache))) {
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
    ret = rawlog_scan(fd, cache, st.st_size);
    if (ret) {
        rawlog_mark_fault(path, ret, cache->error_time);
        goto done;
    }
    cache->error_code = 0;
    cache->error_time = 0;
    cache->st_size = st.st_size;
    cache->st_mtim = st.st_mtim;
    cache->st_ctim = st.st_ctim;
    cache->st_dev = st.st_dev;
    cache->st_ino = st.st_ino;
done:
    close(fd);
    return ret;
}

int rawlog_parse_all(const char *path)
{
    DIR *dir = opendir(path);
    if (!dir)
        return -errno;
    char name[PATH_MAX];
    struct dirent *entry;
    cache_wrlock();
    rawlog_error = 0;
    while ((entry = readdir(dir))) {
        if (entry->d_type != DT_REG && entry->d_type != DT_UNKNOWN)
            continue;
        if (snprintf(name, sizeof(name), "%s/%s", path, entry->d_name) >= (int)sizeof(name))
            continue;
        struct stat st;
        if (stat(name, &st) < 0 || !S_ISREG(st.st_mode))
            continue;
        rawlog_parse_one(name);
    }
    cache_prune_missing();
    cache_sort();
    cache_unlock();
    closedir(dir);
    return 0;
}

static int rawlog_uncompress_record(int fd, void *outbuf, unsigned long *outlen,
                                    unsigned long inlen)
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

    /* Reject implausible counts before they size any allocation: a sample
     * holds at most ndeviat tasks, so it cannot contain more processes
     * (totproc) or active processes (nactproc) than that, and zlib cannot
     * expand the compressed task blob by more than ~1032x, so it cannot
     * decompress to more tstats than that ratio allows. With pcomplen already
     * bounded to the file size by the caller, this bounds every allocation
     * below (and the process pointer arrays are sized by ndeviat so the fill
     * loop can never overrun them). */
    if (rr->ndeviat > 1000000 || rr->totproc > rr->ndeviat || rr->nactproc > rr->totproc ||
        outlen > (unsigned long)rr->pcomplen * 1032 + sizeof(struct tstat))
        return -EINVAL;

    /* 1, allocate memory (taskall is calloc'd so a short decompression
     * cannot leave the fill loop reading indeterminate task flags) */
    memset(devtstat, 0x00, sizeof(struct devtstat));
    devtstat->taskall = calloc(rr->ndeviat ? rr->ndeviat : 1, sizeof(struct tstat));
    if (!devtstat->taskall) {
        ret = -ENOMEM;
        goto free_devtstat;
    }

    devtstat->procall = malloc(sizeof(struct tstat *) * (rr->totproc ? rr->totproc : 1));
    if (!devtstat->procall) {
        ret = -ENOMEM;
        goto free_devtstat;
    }

    devtstat->procactive = malloc(sizeof(struct tstat *) * (rr->nactproc ? rr->nactproc : 1));
    if (!devtstat->procactive) {
        ret = -ENOMEM;
        goto free_devtstat;
    }

    /* 2, read record and uncompress */
    ret = rawlog_uncompress_record(fd, devtstat->taskall, &outlen, rr->pcomplen);
    if (ret)
        goto free_devtstat;

    /* 3, build devtstat */
    for (; ntaskall < rr->ndeviat; ntaskall++) {
        struct tstat *tstat = devtstat->taskall + ntaskall;
        if (tstat->gen.isproc) {
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
    int flags =
        hflags & ~(ACCTACTIVE | IOSTAT | NETATOP | NETATOPD | CONTAINERSTAT | GPUSTAT | CGROUPV2);
    if ((hflags & RAWLOGNG) == RAWLOGNG) {
        if (rflags & RRACCTACTIVE)
            flags |= ACCTACTIVE;
        if (rflags & RRIOSTAT)
            flags |= IOSTAT;
    }
    if (rflags & RRNETATOP)
        flags |= NETATOP;
    if (rflags & RRNETATOPD)
        flags |= NETATOPD;
    if (rflags & RRCGRSTAT)
        flags |= CGROUPV2;
    if (rflags & RRCONTAINERSTAT)
        flags |= CONTAINERSTAT;
    if (rflags & RRGPUSTAT)
        flags |= GPUSTAT;
    if (hflags & NETATOPBPF)
        flags |= NETATOPBPF;
    return flags;
}

/* A copied index entry is safe to use after releasing cache_rwlock. */
struct rawlog_source {
    char *path;
    off_t off, size, scan_off;
    dev_t dev;
    ino_t ino;
    struct timespec mtime, ctime;
    int flags;
    unsigned int pagesize;
    unsigned short hertz;
    struct utsname utsname;
};

static int rawlog_stat_matches(const struct rawlog_source *src, const struct stat *st)
{
    return src->dev == st->st_dev && src->ino == st->st_ino && src->size == st->st_size &&
           src->mtime.tv_sec == st->st_mtim.tv_sec && src->mtime.tv_nsec == st->st_mtim.tv_nsec &&
           src->ctime.tv_sec == st->st_ctim.tv_sec && src->ctime.tv_nsec == st->st_ctim.tv_nsec;
}

static int rawlog_select(const time_t *timestamp, struct rawlog_source *src)
{
    struct cache_t *cache;
    off_t off = 0;
    memset(src, 0, sizeof(*src));
    cache_rdlock();
    cache = timestamp ? cache_get(*timestamp, &off) : NULL;
    if (!cache) {
        cache = cache_get_recent();
        if (!cache) {
            int ret = rawlog_error ? -EINVAL : -ENOENT;
            cache_unlock();
            return ret;
        }
        if (timestamp && *timestamp < cache->elems[cache->nr_elems - 1].time) {
            cache_unlock();
            return -ENOENT;
        }
        off = cache->elems[cache->nr_elems - 1].off;
    }
    src->path = strdup(cache->name);
    src->off = off;
    src->size = cache->st_size;
    src->scan_off = cache->scan_off;
    src->dev = cache->st_dev;
    src->ino = cache->st_ino;
    src->mtime = cache->st_mtim;
    src->ctime = cache->st_ctim;
    src->flags = cache->flags;
    src->pagesize = cache->pagesize;
    src->hertz = cache->hertz;
    src->utsname = cache->utsname;
    cache_unlock();
    return src->path ? 0 : -ENOMEM;
}

void rawlog_sample_free(struct raw_sample *sample)
{
    free(sample->system);
    free(sample->tasks);
    free(sample->procall);
    free(sample->procactive);
    memset(sample, 0, sizeof(*sample));
}

static int rawlog_load(const struct rawlog_source *src, struct raw_sample *sample)
{
    struct rawrecord rr;
    struct devtstat dev;
    struct stat st;
    int fd = open(src->path, O_RDONLY);
    int ret;
    memset(sample, 0, sizeof(*sample));
    if (fd < 0)
        return -errno;
    if (fstat(fd, &st) < 0 || !rawlog_stat_matches(src, &st) ||
        src->off < (off_t)sizeof(struct rawheader) || src->off > st.st_size ||
        src->scan_off > st.st_size) {
        ret = -EAGAIN;
        goto done;
    }
    if (pread(fd, &rr, sizeof(rr), src->off) != sizeof(rr)) {
        ret = -EIO;
        goto done;
    }
    uint64_t length = (uint64_t)sizeof(rr) + rr.scomplen + rr.pcomplen + rr.ccomplen + rr.icomplen;
    if (!rr.scomplen || !rr.pcomplen || rr.scomplen > 1024U * 1024 * 1024 ||
        rr.pcomplen > 1024U * 1024 * 1024 || rr.ndeviat > 1000000 ||
        length > (uint64_t)(st.st_size - src->off)) {
        ret = -EINVAL;
        goto done;
    }
    if (lseek(fd, src->off + sizeof(rr), SEEK_SET) < 0) {
        ret = -errno;
        goto done;
    }
    sample->system = malloc(sizeof(*sample->system));
    if (!sample->system) {
        ret = -ENOMEM;
        goto done;
    }
    ret = rawlog_get_sstat(fd, sample->system, rr.scomplen);
    if (ret)
        goto done;
    ret = rawlog_get_devtstat(fd, &dev, &rr);
    if (ret)
        goto done;
    sample->tasks = dev.taskall;
    sample->procall = dev.procall;
    sample->procactive = dev.procactive;
    sample->ntasks = dev.ntaskall;
    sample->nprocall = dev.nprocall;
    sample->nprocactive = dev.nprocactive;
    sample->ntaskactive = dev.ntaskactive;
    sample->totrun = dev.totrun;
    sample->totslpi = dev.totslpi;
    sample->totslpu = dev.totslpu;
    sample->totzombie = dev.totzombie;
    sample->nexit = rr.nexit;
    sample->noverflow = rr.noverflow;
    sample->timestamp = rr.curtime;
    sample->interval = rr.interval;
    sample->flags = rawlog_record_flags(src->flags, rr.flags);
    sample->record_flags = rr.flags;
    sample->pagesize = src->pagesize;
    sample->hertz = src->hertz;
    sample->utsname = src->utsname;
    if (fstat(fd, &st) < 0 || !rawlog_stat_matches(src, &st))
        ret = -EAGAIN;
done:
    close(fd);
    if (ret)
        rawlog_sample_free(sample);
    return ret;
}

static int rawlog_refresh_path(const char *path)
{
    cache_wrlock();
    int ret = rawlog_parse_one(path);
    cache_unlock();
    return ret;
}

/* Event-loop-owned result: valid until the next acquire or rawlog_reset. */
int rawlog_get_recent_sample(const struct raw_sample **sample, uint64_t *generation)
{
    *sample = NULL;
    for (int attempt = 0; attempt < 4; attempt++) {
        const char *fault_path;
        char *fault_copy = NULL;
        cache_rdlock();
        struct cache_t *recent = cache_get_recent();
        time_t latest = recent ? recent->elems[recent->nr_elems - 1].time : 0;
        int fault = cache_recent_error(latest, &fault_path);
        if (fault && fault_path)
            fault_copy = strdup(fault_path);
        cache_unlock();
        if (fault) {
            if (!fault_copy)
                return -ENOMEM;
            int ret = rawlog_refresh_path(fault_copy);
            free(fault_copy);
            if (ret == -ENOENT)
                continue;
            if (ret) {
                rawlog_drop_decoded();
                return ret;
            }
            continue;
        }
        struct rawlog_source src;
        int ret = rawlog_select(NULL, &src);
        if (ret) {
            rawlog_drop_decoded();
            return ret;
        }
        int fd = open(src.path, O_RDONLY);
        struct stat st;
        if (fd < 0 || fstat(fd, &st) < 0) {
            ret = -errno;
            if (fd >= 0)
                close(fd);
            if (ret != -ENOENT) {
                cache_wrlock();
                rawlog_mark_fault(src.path, ret, 0);
                cache_unlock();
            } else {
                cache_wrlock();
                cache_free(src.path);
                cache_unlock();
            }
            free(src.path);
            rawlog_drop_decoded();
            if (ret == -ENOENT)
                continue;
            return ret;
        }
        close(fd);
        if (!rawlog_stat_matches(&src, &st)) {
            ret = rawlog_refresh_path(src.path);
            free(src.path);
            if (ret && ret != -EAGAIN) {
                rawlog_drop_decoded();
                return ret;
            }
            continue;
        }
        if (decoded.valid && decoded.off == src.off && decoded.dev == src.dev &&
            decoded.ino == src.ino && decoded.size == src.size &&
            decoded.mtime.tv_sec == src.mtime.tv_sec &&
            decoded.mtime.tv_nsec == src.mtime.tv_nsec &&
            decoded.ctime.tv_sec == src.ctime.tv_sec &&
            decoded.ctime.tv_nsec == src.ctime.tv_nsec && !strcmp(decoded.path, src.path)) {
            free(src.path);
            *sample = &decoded.sample;
            *generation = decoded.generation;
            return 0;
        }
        struct raw_sample next;
        ret = rawlog_load(&src, &next);
        if (ret) {
            free(src.path);
            rawlog_drop_decoded();
            if (ret == -EAGAIN)
                continue;
            return ret;
        }
        rawlog_drop_decoded();
        decoded.sample = next;
        decoded.path = src.path;
        decoded.dev = src.dev;
        decoded.ino = src.ino;
        decoded.off = src.off;
        decoded.size = src.size;
        decoded.mtime = src.mtime;
        decoded.ctime = src.ctime;
        decoded.valid = 1;
        decoded.generation++;
        *sample = &decoded.sample;
        *generation = decoded.generation;
        return 0;
    }
    rawlog_drop_decoded();
    return -EAGAIN;
}

int rawlog_get_record(time_t ts, char *labels, connection *conn, struct output *op)
{
    for (int attempt = 0; attempt < 3; attempt++) {
        struct rawlog_source src;
        int ret = rawlog_select(&ts, &src);
        if (ret)
            return ret;
        struct raw_sample sample;
        ret = rawlog_load(&src, &sample);
        if (ret == -EAGAIN) {
            rawlog_refresh_path(src.path);
            free(src.path);
            continue;
        }
        free(src.path);
        if (ret)
            return ret;
        struct devtstat dev = {.taskall = sample.tasks,
                               .procall = sample.procall,
                               .procactive = sample.procactive,
                               .ntaskall = sample.ntasks,
                               .nprocall = sample.nprocall,
                               .nprocactive = sample.nprocactive,
                               .ntaskactive = sample.ntaskactive,
                               .totrun = sample.totrun,
                               .totslpi = sample.totslpi,
                               .totslpu = sample.totslpu,
                               .totzombie = sample.totzombie};
        /* Upstream JSON reads these globals; serialize its mutable render only. */
        pthread_mutex_lock(&json_render_lock);
        pagesize = sample.pagesize;
        hertz = sample.hertz;
        utsname = sample.utsname;
        jsonout(sample.flags, labels, sample.timestamp, sample.interval, &dev, sample.system,
                sample.nexit, sample.noverflow, 0, conn, op);
        pthread_mutex_unlock(&json_render_lock);
        rawlog_sample_free(&sample);
        return 0;
    }
    return -EAGAIN;
}
