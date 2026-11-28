#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include "atop.h"
#include "photoproc.h"
#include "sample.h"

int main(void)
{
    struct raw_sample sample = {0};
    struct metrics_status status;
    sample.timestamp = 100;
    sample.interval = 10;
    sample.flags = NETATOPBPF;
    metrics_sample_status(&sample, 2, 99, &status);
    assert(status.age == 0 && status.fresh && status.available && status.process_visible);
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

    struct tstat task = {0};
    task.gen.isproc = 1;
    task.gen.pid = 42;
    task.gen.btime = 1000;
    strcpy(task.gen.name, "a\"\\b");
    task.net.tcprsz = 1000;
    sample.tasks = &task;
    sample.ntasks = 1;
    sample.timestamp = time(NULL);
    sample.interval = 10;
    sample.record_flags = 0;
    strcpy(sample.utsname.nodename, "test-host");
    char *body = NULL;
    size_t length = 0;
    assert(metrics_render(&sample, 1, 0, &body, &length) == 0);
    assert(strstr(body, "pid=\"42\",name=\"a\\\"\\\\b\"") != NULL);
    assert(strstr(body, "protocol=\"tcp\"} 100.000000") != NULL);
    free(body);
    strcpy(task.gen.name, "changed");
    assert(metrics_render(&sample, 1, 0, &body, &length) == 0);
    assert(strstr(body, "name=\"changed\"") == NULL);
    free(body);
    assert(metrics_render(&sample, 2, 0, &body, &length) == 0);
    assert(strstr(body, "name=\"changed\"") != NULL);
    free(body);
    metrics_reset();
    return 0;
}
