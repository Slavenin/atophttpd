/*
 * Copyright 2022 zhenwei pi
 *
 * Authors:
 *   zhenwei pi <pizhenwei@bytedance.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#ifndef _CACHE_H_
#define _CACHE_H_

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <time.h>

struct cache_elem_t {
    time_t time;
    off_t off;
};

struct cache_t {
    char *name;
    int flags;
    int max_elems;
    int nr_elems;
    struct cache_elem_t *elems;
    off_t st_size;
    off_t scan_off;
    dev_t st_dev;
    ino_t st_ino;
    struct timespec st_mtim;
    struct timespec st_ctim;
    unsigned char scan_head[128], scan_tail[64];
    size_t scan_head_len, scan_tail_len;
    time_t error_time;
    int error_code;
    unsigned int pagesize;
    unsigned short hertz;
    struct utsname utsname;
};

struct cache_t *cache_alloc(const char *name);
struct cache_t *cache_find(const char *name);
void cache_free(const char *name);
int cache_set(struct cache_t *cache, time_t time, off_t off);
void cache_clear(struct cache_t *cache);
void cache_prune_missing(void);
int cache_recent_error(time_t selected, const char **path);
struct cache_t *cache_get(time_t time, off_t *off);
void cache_done(struct cache_t *cache);
void cache_sort();
struct cache_t *cache_get_recent();

/*
 * Reader/writer lock protecting the whole cache module (the global cache
 * array and the contents of every struct cache_t). Readers take cache_rdlock,
 * the cache updater rewrites it under cache_wrlock. The server is currently a
 * single-threaded event loop, so the lock is uncontended; it is kept so the
 * cache stays safe if request handling becomes concurrent again. A reader must
 * hold the lock for as long as it dereferences a cache_t it obtained, not just
 * across the lookup, since the writer may free that cache_t.
 */
void cache_rdlock(void);
void cache_wrlock(void);
void cache_unlock(void);

#endif
