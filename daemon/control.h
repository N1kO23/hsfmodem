/* SPDX-License-Identifier: MIT */
/*
 * The status socket (docs/SERIAL.md): one request line per connection,
 * answered by a handler; the reply ends with an empty line.
 */
#ifndef HSF_CONTROL_H
#define HSF_CONTROL_H

#include <stdio.h>
#include <sys/types.h>

typedef void (*hsf_control_handler)(void *ctx, const char *request, FILE *reply);

struct hsf_control;

struct hsf_control *hsf_control_start(const char *path, gid_t group, hsf_control_handler handler, void *ctx);
void hsf_control_stop(struct hsf_control *c);

#endif /* HSF_CONTROL_H */
