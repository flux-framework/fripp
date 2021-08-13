# Week 11: CI

CI testing is hard.

### Jobtap Plugins

Right now, the only place in the `flux-core` tree where the `fripp/stats` code is being used is in the broker's content-cache.

```c
static  void  update_stats (struct  content_cache  *cache)
{
	flux_stats_gauge_set (cache->h, "content-cache.count",
		(int) zhashx_size (cache->entries));
	flux_stats_gauge_set (cache->h, "content-cache.valid",
		cache->acct_valid);
	flux_stats_gauge_set (cache->h, "content-cache.dirty",
		cache->acct_dirty);
	flux_stats_gauge_set (cache->h, "content-cache.size",
		cache->acct_size);
	flux_stats_gauge_set (cache->h, "content-cache.flush-batch-count",
		cache->flush_batch_count);
}
```

This caused a very low code coverage (around 30% in `fripp.c`), so to increase the coverage in the CI tests I wrote two jobtap plugins. The first one `stats-immediate` is shown below, and it tests using an aggregation period of `0s` which means each packet is formed and sent right away and there is no aggregation. The second plugin `stats-basic` is very similar, but it's using and aggregation period of `1s`. These two plugins, in addition to the stats collection in the content-cache, give much better code coverage (about 84%) and test many more of the possible uses.

```c
// stats-immediate.c
#include  <flux/core.h>
#include  <flux/jobtap.h>
#include  <time.h>

#include  "src/common/libutil/monotime.h"  

struct  cb_data {
	ssize_t  inactive;
	struct  timespec  ts;
};

static  int  state_cb (flux_plugin_t  *p,
                       const  char  *topic,
                       flux_plugin_arg_t  *args,
                       void  *arg)
{
	struct  cb_data  *data  =  arg;
	flux_job_state_t  state;
	flux_job_state_t  prev_state  =  4096;
	flux_t  *h  =  flux_jobtap_get_flux (p);

	if (flux_plugin_arg_unpack (args, FLUX_PLUGIN_ARG_IN,
                              "{s:i s?i}",
                              "state", &state,
                              "prev_state", &prev_state) <  0) {
		flux_log (h,
              LOG_ERR,
              "flux_plugin_arg_unpack: %s",
              flux_plugin_arg_strerror(args));
		return  -1;
	}

	flux_stats_gauge_inc (h, flux_job_statetostr (state, false), 1);
	flux_stats_gauge_inc (h, flux_job_statetostr (prev_state, false), -1);

	switch (state) {
		case  FLUX_JOB_STATE_CLEANUP:
			monotime (&data->ts);
			break;
		case  FLUX_JOB_STATE_INACTIVE:
			flux_stats_timing (h, "cleanup.timing", monotime_since (data->ts));
			flux_stats_count (h, "inactive.count", ++data->inactive);
			break;
		default:
			break;
	}

	return  0;
} 

int  flux_plugin_init (flux_plugin_t  *p)
{
	struct  cb_data  *data;
	flux_t  *h;

	if (!(h  =  flux_jobtap_get_flux (p)))
		return  -1;
	if (!(data  =  calloc (1, sizeof (*data))))
		return  -1; 

	flux_stats_set_prefix (h, "flux.job.state.immediate");
	flux_stats_set_period (h, 0.0);

	// try setting a prefix longer than the limit and
	// see that it remains as "flux.job.state.immediate"
	flux_stats_set_prefix (h, "aQmi173rvgumStDdMZdwtJtpLLVJOUXol2aDndev/XsH/gM\
wlPz/vMZhajJWGctwJZa1uFoAoINjwITPvezGoQDb/9DD3vkPcknN+f/u3vSc0tg/+3aFTONhUIomK\
B4qiSKSotbtZl3Ewe2Oh+wI+nuG3/ebqIXSoEXjIFOB7vvGA=="); 

	return  flux_plugin_add_handler (p, "job.state.*", state_cb, data);
}
```

---

### The UDP Listener

Since last week, a few small changes were made to the udp test server `stats-listen.py`. Instead of using a static port (`9999`) and hoping that it's open, `stats-listen` now binds to a random port, sets the `FLUX_FRIPP_STATSD` environment variable, and launches flux commands, usually `flux start` followed by a `flux jobtap load $plugin` and `flux mini run hostname`. This also forced the creation of the `--no-set-host` argument to have a way to test that without `FLUX_FRIPP_STATSD` set, 
 
```python
import  subprocess
import  os

parser.add_argument(
	"-n",
	"--no-set-host",
	help="don't set the FLUX_FRIPP_STATSD environment variable",
	action="store_true",
)
parser.add_argument("cmd", nargs=argparse.REMAINDER)

s  =  socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(("0.0.0.0", 0))

if  not  args.no_set_host:
	os.environ["FLUX_FRIPP_STATSD"] =  f"0.0.0.0:{s.getsockname()[1]}"

f  =  subprocess.Popen(args.cmd, env=dict(os.environ))
f.wait()

```

---

### Sharness Tests

The original design for these sharness tests was to create a script to be launched from `flux start` as it's initial program. The script would launch `stats-listen.py` which would write a key `stats-listen` to flux's kvs after binding to `0.0.0.0:9999`, wait for the key `stats-listen` to be created in the kvs, load the appropriate jobtap plugin, and then finally run `flux mini run hostname` as a job. This was a little bit racy since for one it was assuming that port `9999` would be free for use. So to try and remedy the problem, `stats-listen.py` is launched first under `run_timeout.py`, and then `stats-listen` runs whatever pieces of flux code are needed. These tests were also a bit racy since the timeout was being run on the entire line of commands, and with a timeout of `5s` they would sometimes timeout and fail. 

```bash
#!/bin/sh

test_description='Test stats collection and sending'

. `dirname $0`/sharness.sh

test_under_flux 2

udp=$SHARNESS_TEST_SRCDIR/scripts/stats-listen.py
timeout=$SHARNESS_TEST_SRCDIR/scripts/run_timeout.py
plugin_i=${FLUX_BUILD_DIR}/t/stats/.libs/stats-immediate.so
plugin_b=${FLUX_BUILD_DIR}/t/stats/.libs/stats-basic.so

test_expect_success 'load jobtap plugin'  '
	flux jobtap load $plugin_i && flux jobtap load $plugin_b &&
	flux jobtap list > plugins &&
	grep stats-basic plugins && grep stats-immediate plugins
'  

test_expect_success 'prefix set'  '
	$timeout 15 flux python $udp -s flux.job.state.immediate flux start \
	"flux jobtap load $plugin_i && flux mini run hostname"
'  

test_expect_success 'multiple packets recieved'  '
	$timeout 15 flux python $udp -w 3 flux start \
	"flux jobtap load $plugin_i && flux mini run hostname"
'  

test_expect_success 'validate packets immediate'  '
	$timeout 15 flux python $udp -V flux start \
	"flux jobtap load $plugin_i && flux mini run hostname"
' 

test_expect_success 'timing packets received immediate'  '
	$timeout 15 flux python $udp -s timing flux start \
	"flux jobtap load $plugin_i && flux mini run hostname"
' 

test_expect_success 'timing packets received basic'  '
	$timeout 15 flux python $udp -s timing flux start \
	"flux jobtap load $plugin_b && flux mini run hostname && sleep 1"
'  

test_expect_success 'valid content-cache packets recieved'  '
	$timeout 15 flux python $udp -s content-cache -V flux start sleep 1
'

test_expect_success 'nothing recieved with no endpoint'  '
	unset FLUX_FRIPP_STATSD && test_expect_code 137 $timeout 5 flux python $udp -n flux start
'
 

test_done
```

On the most recent push of the [pr](https://github.com/flux-framework/flux-core/pull/3806), all of the CI tests are passing yay!!

---

