/*
 * Copyright 2022 zhenwei pi
 *
 * Authors:
 *   zhenwei pi <pizhenwei@bytedance.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cache.h"

#define RECORDS_TRUNK (60 * 60 / 10)

static int nr_caches;
static struct cache_t **caches;

static pthread_rwlock_t cache_rwlock = PTHREAD_RWLOCK_INITIALIZER;

void cache_rdlock(void)
{
    pthread_rwlock_rdlock(&cache_rwlock);
}

void cache_wrlock(void)
{
    pthread_rwlock_wrlock(&cache_rwlock);
}

void cache_unlock(void)
{
    pthread_rwlock_unlock(&cache_rwlock);
}

struct cache_t *cache_find(const char *name)
{
    for (int i = 0; i < nr_caches; i++) {
        struct cache_t *cache = caches[i];
        assert(cache);
        if (!strcmp(cache->name, name))
            return cache;
    }

    return NULL;
}

struct cache_t *cache_alloc(const char *name)
{
    struct cache_t *cache = cache_find(name);
    struct cache_t **grown;

    assert(!cache);

    cache = calloc(1, sizeof(*cache));
    if (!cache)
        return NULL;

    cache->name = strdup(name);
    if (!cache->name)
        goto err_cache;

    cache->nr_elems = 0;
    cache->max_elems = RECORDS_TRUNK;
    cache->elems = calloc(cache->max_elems, sizeof(struct cache_elem_t));
    if (!cache->elems)
        goto err_name;

    if (!caches)
        grown = calloc(1, sizeof(struct cache_t *));
    else
        grown = realloc(caches, sizeof(struct cache_t *) * (nr_caches + 1));
    if (!grown)
        goto err_elems;
    caches = grown;

    caches[nr_caches++] = cache;

    return cache;

err_elems:
    free(cache->elems);
err_name:
    free(cache->name);
err_cache:
    free(cache);
    return NULL;
}

void cache_free(const char *name)
{
    struct cache_t *cache = cache_find(name);

    if (!cache)
        return;

    for (int i = 0; i < nr_caches; i++) {
        if (caches[i] == cache) {
            memmove(&caches[i], &caches[i + 1], (nr_caches - i - 1) * sizeof(caches[0]));
            break;
        }
    }

    free(cache->elems);
    free(cache->name);
    free(cache);

    nr_caches--;
    if (!nr_caches) {
        free(caches);
        caches = NULL;
    } else {
        struct cache_t **shrunk = realloc(caches, sizeof(struct cache_t *) * nr_caches);
        if (shrunk)
            caches = shrunk;
    }
}

void cache_clear(struct cache_t *cache)
{
    cache->nr_elems = 0;
    cache->scan_off = 0;
    cache->scan_head_len = cache->scan_tail_len = 0;
}

void cache_prune_missing(void)
{
    for (int i = 0; i < nr_caches;) {
        struct stat st;
        if (stat(caches[i]->name, &st) < 0 || !S_ISREG(st.st_mode))
            cache_free(caches[i]->name);
        else
            i++;
    }
}

int cache_recent_error(time_t selected, const char **path)
{
    int error = 0;
    *path = NULL;
    for (int i = 0; i < nr_caches; i++) {
        if (caches[i]->error_code && caches[i]->error_time >= selected) {
            error = caches[i]->error_code;
            *path = caches[i]->name;
        }
    }
    return error;
}

static struct cache_t *__cache_get(time_t time)
{
    for (int i = 0; i < nr_caches; i++) {
        struct cache_t *cache = caches[i];
        struct cache_elem_t *first_elem, *last_elem;

        if (!cache->nr_elems)
            continue;
        first_elem = &cache->elems[0];
        last_elem = &cache->elems[cache->nr_elems - 1];
        if ((time >= first_elem->time) && (time <= last_elem->time))
            return cache;
    }

    return NULL;
}

struct cache_t *cache_get(time_t time, off_t *off)
{
    struct cache_t *cache = __cache_get(time);

    if (!cache)
        return NULL;

    int left = 0;
    int right = cache->nr_elems - 1;

    while (left < right) {
        int mid = (left + right) >> 1;
        struct cache_elem_t *elem = &cache->elems[mid];
        if (time <= elem->time) {
            right = mid;
        } else {
            left = mid + 1;
        }
    }

    struct cache_elem_t *elem = &cache->elems[left];
    *off = elem->off;
    return cache;
}

static int cache_elems_cmp(const void *p1, const void *p2)
{
    struct cache_elem_t *e1 = (struct cache_elem_t *)p1;
    struct cache_elem_t *e2 = (struct cache_elem_t *)p2;

    return (e1->time > e2->time) - (e1->time < e2->time);
}

int cache_set(struct cache_t *cache, time_t time, off_t off)
{
    if (cache->nr_elems == cache->max_elems) {
        int new_max = cache->max_elems + RECORDS_TRUNK;
        struct cache_elem_t *grown = realloc(cache->elems, sizeof(struct cache_elem_t) * new_max);
        if (!grown)
            return -1;
        cache->elems = grown;
        cache->max_elems = new_max;
    }

    struct cache_elem_t *new_elem = &cache->elems[cache->nr_elems++];
    new_elem->time = time;
    new_elem->off = off;

    if (cache->nr_elems == 1)
        return 0;

    struct cache_elem_t *last_elem = &cache->elems[cache->nr_elems - 2];
    if (last_elem->time > new_elem->time) {
        qsort(cache->elems, cache->nr_elems, sizeof(struct cache_elem_t), cache_elems_cmp);
    }
    return 0;
}

static int cache_cmp(const void *p1, const void *p2)
{
    struct cache_t **c1 = (struct cache_t **)p1;
    struct cache_t **c2 = (struct cache_t **)p2;

    return ((*c1)->elems[0].time > (*c2)->elems[0].time) -
           ((*c1)->elems[0].time < (*c2)->elems[0].time);
}

void cache_sort()
{
    /* qsort() is declared nonnull: skip it when there is nothing to sort
     * (caches is NULL while no cache has been allocated yet). */
    if (nr_caches < 2)
        return;

    qsort(caches, nr_caches, sizeof(struct cache_t *), cache_cmp);
}

struct cache_t *cache_get_recent()
{
    struct cache_t *recent = NULL;
    for (int i = 0; i < nr_caches; i++) {
        if (!caches[i]->nr_elems)
            continue;
        if (!recent || caches[i]->elems[caches[i]->nr_elems - 1].time >
                           recent->elems[recent->nr_elems - 1].time)
            recent = caches[i];
    }
    return recent;
}
