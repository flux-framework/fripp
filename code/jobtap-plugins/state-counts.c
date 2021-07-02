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
#include <flux/jobtap.h>

#include "../fripp.h"

struct state_counts {
    int new;
    int depend;
    int priority;
    int sched;
    int run;
    int cleanup;
    int inactive;

    struct fripp_ctx *fripp;
    flux_watcher_t *w;
    flux_plugin_t *p;
};

static void count (struct state_counts *c, flux_job_state_t state, int inc)
{
    switch (state) {
        case FLUX_JOB_STATE_NEW:
            c->new += inc;
            break;
        case FLUX_JOB_STATE_DEPEND:
            c->depend += inc;
            break;
        case FLUX_JOB_STATE_PRIORITY:
            c->priority += inc;
            break;
        case FLUX_JOB_STATE_SCHED:
            c->sched += inc;
            break;
        case FLUX_JOB_STATE_RUN:
            c->run += inc;
            break;
        case FLUX_JOB_STATE_CLEANUP:
            c->cleanup += inc;
            break;
        case FLUX_JOB_STATE_INACTIVE:
            c->inactive += inc;
    }
}

static int state_cb (flux_plugin_t *p,
                      const char *topic,
                      flux_plugin_arg_t *args,
                      void *arg)
{
    struct state_counts *c = arg;
    flux_job_state_t state;
    flux_job_state_t prev_state = 4096;
    flux_t *h = flux_jobtap_get_flux (p);

    if (flux_plugin_arg_unpack (args, FLUX_PLUGIN_ARG_IN,
                               "{s:i s?i}",
                               "state", &state,
                               "prev_state", &prev_state) < 0) {
        flux_log (h,
                 LOG_ERR,
                 "flux_plugin_arg_unpack: %s",
                 flux_plugin_arg_strerror(args));
        return -1;
    }

    count (c, state, 1);
    count (c, prev_state, -1);

    return 0;
}

static void timer_cb (flux_reactor_t *r, 
                      flux_watcher_t *w, 
                      int revents, 
                      void *arg)
{
    struct state_counts *c = arg;

    fripp_format_send (c->fripp,
                      "flux.job-states.new:%d|g\nflux.job-states.depend:%d|g\n\
flux.job-states.priority:%d|g\nflux.job-states.sched:%d|g\n\
flux.job-states.run:%d|g\nflux.job-states.cleanup:%d|g\n\
flux.job-states.inactive:%d|g",
                       c->new, c->depend,
                       c->priority, c->sched,
                       c->run, c->cleanup,
                       c->inactive);
}

int flux_plugin_init(flux_plugin_t *p)
{
    flux_t *h = flux_jobtap_get_flux (p);
    struct state_counts *c;
    
    if (!(c = calloc (1, sizeof (*c)))) {
        flux_log_error (h, "state-count plugin init");
        return -1;
    }
    if (!(c->fripp = fripp_ctx_create (h, "0.0.0.0", 8126)))
        return -1;
    if (!(c->w = flux_timer_watcher_create (
              flux_get_reactor(h),
              0.1,
              0.1,
              timer_cb,
              c)))
        return -1;
    flux_watcher_start (c->w);

    const struct flux_plugin_handler tab[] = {
        { "job.state.*", state_cb, c },
        { "job.new",     state_cb, c },
        { 0 }
    };

    return flux_plugin_register (p, "state-counts", tab);
}

/*
 * vi:tabstop=4 shiftwidth=4 expandtab
 */
