/* Small rawlog producer using the current upstream struct layout. */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

#include "atop.h"
#include "cgroups.h"
#include "config.h"
#include "photoproc.h"
#include "photosyst.h"
#include "rawlog.h"
#include "version.h"

static void compressed(FILE *out, const void *src, size_t len, unsigned int *stored)
{
    uLongf size = compressBound(len);
    unsigned char *buf = malloc(size);
    if (!buf || compress(buf, &size, src, len) != Z_OK)
        exit(2);
    *stored = size;
    if (fwrite(buf, 1, size, out) != size)
        exit(2);
    free(buf);
}

int main(int argc, char **argv)
{
    if (argc != 4) {
        fprintf(stderr, "usage: mkfixture PATH MODE TIMESTAMP\n");
        return 2;
    }
    const char *mode = argv[2];
    FILE *out = fopen(argv[1], "wb");
    if (!out)
        return 2;
    struct rawheader rh = {0};
    rh.magic = MYMAGIC;
    if (!strcmp(mode, "bad-magic"))
        rh.magic = 0;
    rh.aversion = 0x8000 | getnumvers();
    rh.rawheadlen = sizeof(rh);
    rh.rawreclen = sizeof(struct rawrecord);
    rh.sstatlen = sizeof(struct sstat);
    rh.tstatlen = sizeof(struct tstat);
    rh.cstatlen = sizeof(struct cstat);
    rh.pagesize = 4096;
    rh.hertz = 100;
    rh.supportflags = (!strcmp(mode, "none") || !strcmp(mode, "rr-netatop")) ? 0 : NETATOPBPF;
    strcpy(rh.utsname.nodename, "fixture-host");
    if (!strcmp(mode, "bad-abi"))
        rh.tstatlen--;
    fwrite(&rh, sizeof(rh), 1, out);
    static struct sstat ss;
    static struct tstat tasks[3];
    tasks[0].gen.pid = 4242;
    tasks[0].gen.tgid = 4242;
    tasks[0].gen.btime = 1000;
    tasks[0].gen.isproc = 1;
    tasks[0].gen.state = 'S';
    strcpy(tasks[0].gen.name, "a\"\\b");
    strcpy(tasks[0].gen.cmdline, "a\"\\b");
    tasks[0].net.tcprsz = 1000;
    tasks[0].net.tcpssz = 2000;
    tasks[0].net.udprsz = 300;
    tasks[0].net.udpssz = 400;
    tasks[1] = tasks[0];
    tasks[1].gen.isproc = 0;
    tasks[2] = tasks[0];
    tasks[2].gen.btime = 2000; /* same PID, different process lifetime */
    tasks[2].gen.state = 'E';
    strcpy(tasks[2].gen.name, "second");
    tasks[2].net.tcprsz = 500;
    int with_cgroups = !strcmp(mode, "cgroups") || !strcmp(mode, "cgroup-single") ||
                       !strcmp(mode, "partial");
    int records = !strcmp(mode, "cgroups") ? 2 : 1;
    for (int i = 0; i < records; i++) {
        struct rawrecord rr = {0};
        rr.curtime = atoll(argv[3]) - (records - i - 1) * 10;
        rr.interval = !strcmp(mode, "zero-interval") ? 0 : 10;
        rr.flags = !strcmp(mode, "boot") ? RRBOOT : 0;
        if (!strcmp(mode, "rr-netatop"))
            rr.flags |= RRNETATOP;
        rr.ndeviat = 3;
        rr.totproc = 2;
        rr.nactproc = 2;
        if (with_cgroups)
            rr.flags |= RRCGRSTAT;
        /* Reserve the header, write all blocks, then fill their sizes. */
        long record_at = ftell(out);
        fwrite(&rr, sizeof(rr), 1, out);
        compressed(out, &ss, sizeof(ss), &rr.scomplen);
        compressed(out, tasks, sizeof(tasks), &rr.pcomplen);
        if (with_cgroups) {
            const char cg[] = "cgroup";
            const char ids[] = "pidlist";
            rr.coriglen = sizeof(cg);
            compressed(out, cg, sizeof(cg), &rr.ccomplen);
            compressed(out, ids, sizeof(ids), &rr.icomplen);
        }
        if (!strcmp(mode, "bad-zlib")) {
            fseek(out, record_at + sizeof(rr), SEEK_SET);
            fputc(0, out);
        }
        if (!strcmp(mode, "bad-length"))
            rr.pcomplen = 0;
        if (!strcmp(mode, "partial")) {
            fflush(out);
            if (ftruncate(fileno(out), record_at + sizeof(rr) + rr.scomplen / 2) < 0)
                return 2;
        }
        fseek(out, record_at, SEEK_SET);
        fwrite(&rr, sizeof(rr), 1, out);
        fseek(out, 0, SEEK_END);
    }
    fclose(out);
    return 0;
}
