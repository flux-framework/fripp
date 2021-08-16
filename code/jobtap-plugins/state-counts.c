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

static int state_cb (flux_plugin_t *p,
                      const char *topic,
                      flux_plugin_arg_t *args,
                      void *arg)
{
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

    flux_stats_gauge_inc (h, flux_job_statetostr (state, false), 1);
    flux_stats_gauge_inc (h, flux_job_statetostr (prev_state, false), -1);

    return 0;
}

int flux_plugin_init(flux_plugin_t *p)
{
    flux_t *h;
    if (!(h = flux_jobtap_get_flux (p)))
        return -1;

    flux_stats_set_prefix (h, "flux.job.state");
    return flux_plugin_add_handler (p, "job.state.*", state_cb, NULL);
}

/*
 * vi:tabstop=4 shiftwidth=4 expandtab
 */
