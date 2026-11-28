#define _GNU_SOURCE
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>

#include "atop.h"
#include "photoproc.h"
#include "sample.h"

static void label(FILE *out, const char *value, size_t maxlen)
{
    fputc('"', out);
    for (size_t i = 0; i < maxlen && value[i]; i++) {
        unsigned char c = value[i];
        if (c == '"' || c == '\\') {
            fputc('\\', out);
            fputc(c, out);
        } else if (c == '\n' || c == '\r') {
            fputs("\\n", out);
        } else if (c < 0x20) {
            fputc('?', out);
        } else {
            fputc(c, out);
        }
    }
    fputc('"', out);
}

static void process_metric(FILE *out, const char *metric, const struct raw_sample *sample,
                           const struct tstat *task, const char *protocol, count_t bytes)
{
    if (bytes < 0)
        return;
    fprintf(out, "atophttpd_process_network_%s_bytes_per_second{host=", metric);
    label(out, sample->utsname.nodename, sizeof(sample->utsname.nodename));
    fprintf(out, ",pid=\"%d\",name=", task->gen.pid);
    label(out, task->gen.name, sizeof(task->gen.name));
    fprintf(out, ",start_time=\"%lld\",protocol=\"%s\"} %.6f\n", (long long)task->gen.btime,
            protocol, (double)bytes / sample->interval);
}

static int process_compare(const void *left, const void *right)
{
    const struct tstat *a = *(const struct tstat *const *)left;
    const struct tstat *b = *(const struct tstat *const *)right;
    if (a->gen.pid != b->gen.pid)
        return (a->gen.pid > b->gen.pid) - (a->gen.pid < b->gen.pid);
    if (a->gen.btime != b->gen.btime)
        return (a->gen.btime > b->gen.btime) - (a->gen.btime < b->gen.btime);
    return strncmp(a->gen.name, b->gen.name, sizeof(a->gen.name));
}

void metrics_sample_status(const struct raw_sample *sample, unsigned int stale_after, time_t now,
                           struct metrics_status *status)
{
    memset(status, 0, sizeof(*status));
    if (!sample)
        return;
    status->age = now > sample->timestamp ? (long long)(now - sample->timestamp) : 0;
    unsigned int cutoff = stale_after;
    if (!cutoff)
        cutoff = sample->interval > 15
                     ? (sample->interval > UINT_MAX / 2 ? UINT_MAX : sample->interval * 2)
                     : 30;
    status->available = !!(sample->flags & (NETATOP | NETATOPD | NETATOPBPF));
    status->fresh = sample->interval > 0 && status->age <= cutoff;
    status->process_visible =
        status->fresh && status->available && !(sample->record_flags & RRBOOT);
}

static struct {
    uint64_t generation;
    char *body;
    size_t length;
    int valid;
} process_cache;

void metrics_reset(void)
{
    free(process_cache.body);
    memset(&process_cache, 0, sizeof(process_cache));
}

static int metrics_process_block(const struct raw_sample *sample, uint64_t generation)
{
    if (process_cache.valid && process_cache.generation == generation)
        return 0;
    free(process_cache.body);
    memset(&process_cache, 0, sizeof(process_cache));
    FILE *out = open_memstream(&process_cache.body, &process_cache.length);
    if (!out)
        return -errno;
    const struct tstat **processes =
        calloc(sample->ntasks ? sample->ntasks : 1, sizeof(*processes));
    if (!processes) {
        fclose(out);
        free(process_cache.body);
        process_cache.body = NULL;
        return -ENOMEM;
    }
    unsigned int count = 0;
    for (unsigned int i = 0; i < sample->ntasks; i++) {
        const struct tstat *task = &sample->tasks[i];
        if (task->gen.isproc)
            processes[count++] = task;
    }
    qsort(processes, count, sizeof(*processes), process_compare);
    for (unsigned int i = 0; i < count; i++) {
        const struct tstat *task = processes[i];
        if (i && process_compare(&processes[i - 1], &processes[i]) == 0)
            continue;
        process_metric(out, "receive", sample, task, "tcp", task->net.tcprsz);
        process_metric(out, "transmit", sample, task, "tcp", task->net.tcpssz);
        process_metric(out, "receive", sample, task, "udp", task->net.udprsz);
        process_metric(out, "transmit", sample, task, "udp", task->net.udpssz);
    }
    free(processes);
    if (fclose(out) != 0) {
        free(process_cache.body);
        process_cache.body = NULL;
        return -EIO;
    }
    process_cache.generation = generation;
    process_cache.valid = 1;
    return 0;
}

int metrics_render(const struct raw_sample *sample, uint64_t generation, unsigned int stale_after,
                   char **body, size_t *length)
{
    FILE *out;
    struct metrics_status status;
    metrics_sample_status(sample, stale_after, time(NULL), &status);

    *body = NULL;
    *length = 0;
    out = open_memstream(body, length);
    if (!out)
        return -errno;

    fputs(
        "# HELP atophttpd_sample_timestamp_seconds Unix time of the latest complete atop sample.\n"
        "# TYPE atophttpd_sample_timestamp_seconds gauge\n",
        out);
    if (sample) {
        fprintf(out, "atophttpd_sample_timestamp_seconds %lld\n", (long long)sample->timestamp);
    }
    fputs("# HELP atophttpd_sample_interval_seconds Duration of the latest atop interval.\n"
          "# TYPE atophttpd_sample_interval_seconds gauge\n",
          out);
    fprintf(out, "atophttpd_sample_interval_seconds %u\n", sample ? sample->interval : 0);
    fputs("# HELP atophttpd_sample_age_seconds Age of the latest complete atop sample.\n"
          "# TYPE atophttpd_sample_age_seconds gauge\n",
          out);
    fprintf(out, "atophttpd_sample_age_seconds %lld\n", status.age);
    fputs("# HELP atophttpd_sample_fresh Whether the latest atop sample is within the freshness "
          "limit.\n"
          "# TYPE atophttpd_sample_fresh gauge\n",
          out);
    fprintf(out, "atophttpd_sample_fresh %d\n", status.fresh);
    fputs("# HELP atophttpd_process_network_available Whether the rawlog records process network "
          "statistics.\n"
          "# TYPE atophttpd_process_network_available gauge\n",
          out);
    fprintf(out, "atophttpd_process_network_available %d\n", status.available);
    fputs("# HELP atophttpd_process_network_receive_bytes_per_second Process receive bytes per "
          "atop interval second.\n"
          "# TYPE atophttpd_process_network_receive_bytes_per_second gauge\n"
          "# HELP atophttpd_process_network_transmit_bytes_per_second Process transmit bytes per "
          "atop interval second.\n"
          "# TYPE atophttpd_process_network_transmit_bytes_per_second gauge\n",
          out);

    if (sample && status.process_visible) {
        int ret = metrics_process_block(sample, generation);
        if (ret) {
            fclose(out);
            free(*body);
            *body = NULL;
            return ret;
        }
        if (process_cache.length &&
            fwrite(process_cache.body, 1, process_cache.length, out) != process_cache.length) {
            fclose(out);
            free(*body);
            *body = NULL;
            return -EIO;
        }
    }
    if (fclose(out) != 0) {
        free(*body);
        *body = NULL;
        return -EIO;
    }
    return 0;
}
