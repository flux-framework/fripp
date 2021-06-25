/************************************************************\
 * Copyright 2014 Lawrence Livermore National Security, LLC
 * (c.f. AUTHORS, NOTICE.LLNS, COPYING)
 *
 * This file is part of the Flux resource manager framework.
 * For details, see https://github.com/flux-framework.
 *
 * SPDX-License-Identifier: LGPL-3.0
\************************************************************/

#ifndef _FRIPP_H
#define _FRIPP_H

#include <stdarg.h>
#include <flux/core.h>

struct fripp_ctx;

struct fripp_ctx *fripp_ctx_create (flux_t *h, const char *hostname, int port);
void fripp_ctx_destroy (struct fripp_ctx *ctx);

char *fripp_format_send (struct fripp_ctx *ctx, const char *fmt, ...)
        __attribute__ ((format (printf, 2, 3)));

#endif

/*
 * vi:tabstop=4 shiftwidth=4 expandtab
 */
