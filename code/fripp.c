/************************************************************\
 * Copyright 2014 Lawrence Livermore National Security, LLC
 * (c.f. AUTHORS, NOTICE.LLNS, COPYING)
 *
 * This file is part of the Flux resource manager framework.
 * For details, see https://github.com/flux-framework.
 *
 * SPDX-License-Identifier: LGPL-3.0
\************************************************************/

#include <flux/core.h>
#include <unistd.h>
#include <stdbool.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#define FRIPP_MAX_PACKET_LEN 1440
#define DEFAULT_BUFFSIZE 32

struct fripp_ctx {
    flux_t *h;
    struct sockaddr_in si_server;
    int sock;
    char *buf;
    int buf_len;
};


/* Split a packet that is longer than FRIPP_MAX_PACKET_LEN
 * by the last '\n' character, and replace it with a '\0'.
 */
static int split_packet (char *packet)
{
    for (int i = FRIPP_MAX_PACKET_LEN - 1; i >= 0; --i) {
        if (packet[i] == '\n' || packet[i] == '\0') {
            packet[i] = '\0';
            return i;
        }
    }

    return -1;
}

/* Send the packet string stored in 'ctx-buf' in one or more
 * udp packets to the bound server within 'ctx'.
 */
static int fripp_send_metrics (struct fripp_ctx *ctx)
{
    int len, sock_len = sizeof (ctx->si_server);
    bool split;
    char *packet = ctx->buf;

    do {
        split = false;
        len = strlen (packet);

        if (len > FRIPP_MAX_PACKET_LEN) {
            split = true;

            if ((len = split_packet (packet)) == -1)
                return -1;
        }

        if (sendto (ctx->sock, packet, len, 0, 
                    (void *) &ctx->si_server, sock_len) < 0)
            flux_log (ctx->h, LOG_ALERT, "packet %s dropped", packet);
    } while (split && (packet = &packet[len + 1]));

    return 0;
}

void fripp_format_send (struct fripp_ctx *ctx, const char *fmt, ...)
{
    va_list ap, cpy;
    va_start (ap, fmt);
    va_copy (cpy, ap);

    int len;
    if ((len = vsnprintf (ctx->buf, ctx->buf_len, fmt, ap)) 
         >= ctx->buf_len) {
        
        char *tmp;
        if (!(tmp = realloc (ctx->buf, (len + 1) * sizeof (char)))) {
            flux_log_error (ctx->h, "error reallocating buffer");
            goto done;
        }

        ctx->buf = tmp;
        ctx->buf_len = len + 1;
        (void) vsnprintf (ctx->buf, ctx->buf_len, fmt, cpy);
    }
    fripp_send_metrics (ctx);

done:
    va_end (ap);
    va_end (cpy);
}

void fripp_ctx_destroy (struct fripp_ctx *ctx)
{
    close (ctx->sock);
    free (ctx->buf);
    free (ctx);
}

struct fripp_ctx *fripp_ctx_create (flux_t *h, const char *hostname, uint16_t port)
{
    struct fripp_ctx *ctx;
    if (!(ctx = malloc (sizeof (*ctx)))) {
        flux_log_error (h, "fripp_ctx_create");
        goto error;
    }
    
    memset (&ctx->si_server, 0, sizeof (ctx->si_server));
    ctx->si_server.sin_family = AF_INET;
    ctx->si_server.sin_port = htons (port);

    if (!inet_aton (hostname, &ctx->si_server.sin_addr)) {
        flux_log_error (ctx->h, "error creating server address");
        goto error;
    }
    if ((ctx->sock = socket(AF_INET, SOCK_DGRAM, 0)) == -1) {
        flux_log_error (ctx->h, "error opening socket");
        goto error;
    }
    if (!(ctx->buf = calloc (1, DEFAULT_BUFFSIZE))) {
        errno = ENOMEM;
        flux_log_error (ctx->h, "calloc");
        close (ctx->sock);
        goto error;
    }

    ctx->buf_len = DEFAULT_BUFFSIZE;
    return ctx;

error:
    if (ctx)
        free (ctx);
    return NULL;
}

/*
 * vi:tabstop=4 shiftwidth=4 expandtab
 */
