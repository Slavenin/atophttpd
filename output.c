/*
 * Copyright 2022 zhenwei pi
 *
 * Authors:
 *   zhenwei pi <pizhenwei@bytedance.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include "output.h"

#define OUTBUF_DEF_SIZE (1024 * 1024)

static void output_buf(struct output *op, char *buf, int size)
{
	if (!op->ob.buf)
	{
		op->ob.size = OUTBUF_DEF_SIZE;
		op->ob.buf = calloc(1, op->ob.size);
		if (!op->ob.buf)
			exit(ENOMEM);
	}

	/* no enought buf, grow it */
	if (op->ob.size - op->ob.offset < size)
	{
		while (op->ob.size - op->ob.offset < size)
			op->ob.size *= 2;
		char *grown = realloc(op->ob.buf, op->ob.size);
		if (!grown)
			exit(ENOMEM);
		op->ob.buf = grown;
	}

	memcpy(op->ob.buf + op->ob.offset, buf, size);
	op->ob.offset += size;
}

void output_samp(struct output *op, char *buf, int size)
{
	switch (op->output_type)
	{
		case OUTPUT_STDOUT:
			fwrite(buf, 1, size, stdout);
		break;

		case OUTPUT_FD:
			for (int off = 0; off < size;) {
				ssize_t written = write(op->fd, buf + off, size - off);
				if (written < 0 && errno == EINTR)
					continue;
				if (written <= 0)
					break;
				off += written;
			}
		break;

		case OUTPUT_BUF:
		output_buf(op, buf, size);
		break;

		default:
		fprintf(stderr, "Error, unknown output type\n");
		exit(EINVAL);
	}
}

void output_samp_done(struct output *op, connection *conn)
{
	if (op->done)
		op->done(op, conn);

	if (op->output_type == OUTPUT_BUF)
	{
		memset(op->ob.buf, 0x00, op->ob.offset);
		op->ob.offset = 0;
	}
}
