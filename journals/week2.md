# Week 2: Exploration

### Collectd

Last week I took a look at statsd and brubeck, a statsd compatible aggregator. This week I took a look at one more statsd compatible aggregator in [collectd](https://collectd.org/) with the statsd plugin. Collectd is an open source system statistics collection daemon written in C, and it has a ton of plugins for various different purposes. The ones I utilized were the apache, statsd, and write_graphite plugins to see if collectd could be used as a viable option for a statsd aggregator.  Getting collectd installed is on a Debian distro is simple as it is available thru `apt`, and red hat it may be available thru `yum`, and the source is available with `makefiles` on [GitHub](https://github.com/collectd/collectd/).

```bash
# install collectd
$ sudo apt-get install collectd
```

Once collectd is installed, it's time to set up the configuration file at `/etc/collectd/collectd.conf`. The first thing I did was disable all of the default plugins since I really only cared about testing the statsd and graphite configuration. This can be done by simply commenting out the unwanted `LoadPlugin` statements along with their corresponding `<Plugin>` blocks in the config file. My final config file (minus most the comments) looked like this:

```bash
$ cat /etc/collectd/collectd.conf
# Global
Hostname "my-pc"			# arbitrary name
FQDNLookup true

Interval 10				# query values from the read plugins every 10s

MaxReadInterval 86400			# read plugins double the query interval after 
					# each failed attempt; cap it at 86400 (default)
Timeout 2				# how many iterations before a list is considered 
					# missing
ReadThreads 5
WriteThreads 5

WriteQueueLimitHigh 1000000
WriteQueueLimitLow 800000

# Logging
LoadPlugin syslog

<Plugin syslog>
	LogLevel info
</Plugin>

# LoadPlugin Section
LoadPlugin apache
LoadPlguin statsd
LoadPlugin write_graphite

<Plugin apache>				# apache config
	<Instance "Graphite">		
					# set the endpoint for the server
					# stats setup in the apache config
		URL "http://localhost/sever-status?auto"
		Server "apache"		# server type? (not documented)
	</Instance>
</Plugin>

<Plugin statsd>				# statsd config
	Port "8125"
	DeleteCounters false		# send metrics as received if unchanged
	DeleteTimers false		# ""
	DeleteGauges false		# ""
	DeleteSets false		# ""
	CounterSum false		# dont report change since last read
	TimerLower false		# dont calculate and send within a specific 
					# interval
	TimerUpper false		# ""
	TimerSum false			# ""
	TimerCount false		# ""
</Plugin>

<Plugin write_graphite>			# graphite config
	<Node "example">		# an instance with an arbitrary name
		Host "localhost"
		Port "2023"
		Protocol "tcp"		
		ReconnectInterval 0	# dont force close/reopen connecition
		LogSendErrors true	# log errors on writes
		Prefix "collectd"	# prepended to global collectd hostname
		Postfix "collectd"	# appended to global collectd hostname
		StoreRates true		# convert counter values to rates
		AlwaysAppendDS false	# dont append name of data source to metric
		SeperateInstances false	# only use one path per component

					# allows for duplicates in namespacing
					# i.e. foo.bar.bar.baz wont drop the second bar
		DropDuplicateFields false
	</Node>
</Plugin>

# get any extra config files
<Include "/etc/collectd/collectd.conf.d">
	Filter "*.conf"
</Include>
```

Then in the apache2 config for graphite, the status endpoint declared in the collectd config needs to be setup. The following 4 lines can be added to `/etc/apache2/sites-available/apache2-graphite.conf` to enable the endpoint.

```bash
<Location "/server-stats">		# enable a new endpoint /server-stats
	SetHandler server-status	# invoke the builtin stats handler
	Requrie all granted		# allows anyone to call it
</Location>
```

Now that both apache2/graphite and collectd are configured, let's go ahead an restart both daemons to reflect the new changes, and we should be able to see metrics coming thru.

```bash
$ sudo systemctl restart apache2.service
$ sudo systemctl restart collectd.service

# send some dummy data
$ for i in {1..100000}; do echo "foo.bar:1|c" >/dev/udp/<hostname>/8125; done
```

![collectd statsd](https://i.imgur.com/Gav6wG2.png)
---

### Launching Brubeck Alongside Flux
Last week, I was trying to find a good way to spin up a brubeck instance alongside a flux instance.  One way to do this is to launch brubeck from rc1 as a background job using `flux mini submit`, but this approach has the downside of brubeck being bound to a single core for the entirety it's running in the flux instance. Spinning up brubeck this way makes taking down the instance a matter of killing it with the `flux job` module while the instance is running, or if forgotten with will be torn down during the cleanup of the broker.
```bash 
$ flux job kill $(flux jobs | grep [b]rubeck | awk '{print $1}')
```

---

### Sending Builtin Flux Message Counters

Up until this point, the dummy data I had been sending was purely dummy data. Now it's time to take a look at something tangible. Each flux handle has some builtin message counters as defined in `src/common/libflux/handle.h` that we can use to see some stats per module.

```c
// from src/common/libflux/handle.h
typedef struct {
    int request_tx;
    int request_rx;
    int response_tx;
    int response_rx;
    int event_tx;
    int event_rx;
    int keepalive_tx;
    int keepalive_rx;
} flux_msgcounters_t;
```

The simplest way to get these stats per module is to tap into the `modservice.c` which is where the stats can be received already thru the `flux module stats` command. To start I needed  add some extra stuff to get the context setup. I added two new `flux_watcher_t`'s (idle and check) along with the existing prep watcher to test the performance by watcher type, a `bool` to check if the module is already running (to avoid sending lots of keepalive messages), a `flux_msgcounters_t` to keep track of whether or not the metrics had changed since last send, and a `sockaddr_in` to hold the server info. Also, a few macros were needed, `FRIPP_ENABLE_MSGCOUNT` is the bitmask needed to enable to the message counter being sent, and `FRIPP_MAX_PACKET_LEN` is the max buffer size for a single packet, although it is highly unlikely that a single packet of the counters would be nearly that big.

```c
#define  FRIPP_ENABLE_MSGCOUNT (0x1000)
// not sure on the exact max size
// this is max size used by a go
// statsd library to avoid ip fragmentation
// with an MTU of 1500 on ethernet 
#define  FRIPP_MAX_PACKET_LEN  1440

typedef  struct {
	flux_t  *h;
	module_t  *p;
	zlist_t  *handlers;
	flux_watcher_t  *w_prepare;
	flux_watcher_t  *w_idle;
	flux_watcher_t  *w_check;
	bool  running;
	flux_msgcounters_t  last_mcs;
	struct  sockaddr_in  si_server;
} modservice_ctx_t;
```

Then a couple of utility functions were needed. The first, `fripp_bind_server` sets up the server address for the brubeck instance, and the second, `fripp_send_msgcounters`, opens a socket and sends the message counters as a single packet to the server. The metrics are currently namespaced like `flux.module.rank.msg_type` and being sent as gauges which take an arbitrary number, so basically a snapshot of the current message counters. Originally they were always sent and ignored the debug flags.

```c
/* Bind a socket address to the brubeck sampler.
 * For now the address is being hardcoded and running 
 * locally at 0.0.0.0:8126.
 */
static int fripp_bind_server (modservice_ctx_t *ctx)
{
	memset (&ctx->si_server, 0, sizeof (ctx->si_server));
	ctx->si_server.sin_family  =  AF_INET;
	ctx->si_server.sin_port  =  htons ((uint16_t) 8126);

	if (!inet_aton ("0.0.0.0", &ctx->si_server.sin_addr)) {
		flux_log_error (ctx->h, "error creating server address");
		return  -1;
	}

	return  0;
}

/* Send the current message counters for the current module
 * to the bound brubeck socket address. All message counters
 * SHOULD be able to be sent as a single packet without 
 * exceeding the FRIPP_MAX_PACKET_LEN when namespaced as
 * flux.module_name.broker_rank.msg_type.
 */
static  void  fripp_send_msgcounters (modservice_ctx_t  *ctx)
{
	int  sock, sock_len  =  sizeof (ctx->si_server);
	char  packet[FRIPP_MAX_PACKET_LEN];
	
	if ((sock  =  socket(AF_INET, SOCK_DGRAM, 0)) ==  -1)
		flux_log_error (ctx->h, "failed to bind socket");

	const  char  *mod_name  =  module_get_name (ctx->p);
	uint32_t  rank;
	
	if (flux_get_rank (ctx->h, &rank) <  0)
		flux_log_error (ctx->h, "flux_get_rank");
	
	int  len  =  sprintf (packet,
		"flux.%s.%d.request_tx:%d|g\nflux.%s.%d.request_rx:%d|g\n\
flux.%s.%d.response_tx:%d|g\nflux.%s.%d.response_rx:%d|g\n\
flux.%s.%d.event_tx:%d|g\nflux.%s.%d.event_rx:%d|g\n\
flux.%s.%d.keepalive_tx:%d|g\nflux.%s.%d.keepalive_rx:%d|g",
	mod_name, rank, ctx->last_mcs.request_tx,
	mod_name, rank, ctx->last_mcs.request_rx,
	mod_name, rank, ctx->last_mcs.response_tx,
	mod_name, rank, ctx->last_mcs.response_rx,
	mod_name, rank, ctx->last_mcs.event_tx,
	mod_name, rank, ctx->last_mcs.event_rx,
	mod_name, rank, ctx->last_mcs.keepalive_tx,
	mod_name, rank, ctx->last_mcs.keepalive_rx);

	if (sendto (sock, packet, len, 0, (void  *) &ctx->si_server, sock_len) <  0)
		flux_log (ctx->h, LOG_ALERT, "packet %s dropped", packet);

	close (sock);
}
```

This was my first attempt at sending the message counters, and for some reason only 5 of the expected 8 counters were received by the graphite backend. Brubeck was not reporting any errors, so possibly the packet was misformed before being sent?

```bash
for i in {1..100}; do flux mini run hostname; done
```

![msg counter dump](https://i.imgur.com/9tnzqBp.png)

![msg counter heirarchy](https://i.imgur.com/UPS0SxL.png)
![msg counter brubeck](https://i.imgur.com/6NAnU5q.png)

But after rebuilding flux, the error went away, and sending the metrics as counters (instead of gauges) ~192 metrics/s  were when instance is just idling.

![msg counters c](https://i.imgur.com/7UkpqJE.png)

#### Sending the Metrics from Within Debug Mode

After I was able to get the message counters being sent properly, it was time to make use of the debug flags and only send them if enabled. The debug mask to enable the metrics to be set can be invoked per module with the `flux module debug` command.

```bash
# enable the fripp metrics for kvs
$ flux module debug -s 0x1000 kvs

# enable the fripp metric for all modules
$ for m in $(flux module list | awk 'FNR !~ /^1$/ {print $1}'); do 
>       flux module debug -s 0x1000 $m;
> done
```

Then, once all of the modules are dumping metrics to brubeck, it's time to generate a bunch of data by running some simple jobs.

```bash
# run 10,000 simple jobs
$ for i in {1..10000}; do flux mini run echo -n "$i " && hostname; done
```

Brubeck was configured with a flush interval of 10s (i.e. sends all metrics to graphite every 10s), and i chose to watch the kvs module for no reason in particular, but here are the final message counts  for the kvs module after running the loop and closing the instance right after.

| message type | final count | 
| :---: | :---: |
| event_rx | ~210,000 |
| event_tx | ~208,000 |
| keepalive_rx | 0 |
| keepalive_tx | 1 |
| request_rx | ~525,000 |
| request_tx | ~905,000 |
| response_rx | ~905,000 |
| response_tx | ~515,000 |

I ran this loop 3 times, once with each type of watcher (prepare/check/idle). The purpose of sending the metrics from within each of the three types of watchers was to compare the throughput of each one both while the instance is constantly running jobs and sitting waiting for jobs, and to see the consistency of each one. That is, to see if any of the watchers had large gaps in sending the metrics. 
Using the prep watcher, here is the callback and graphs associated with it. The prep watcher has a bit more code than the idle and check watchers since it was already being used to send a keepalive message when the module starts up.

#### Using the prepare watcher - took ~ 1h11m45s:
```c
static  void  prepare_cb (flux_reactor_t  *r, flux_watcher_t  *w,
			  int  revents, void  *arg)
{
	modservice_ctx_t  *ctx  =  arg;
	int  *debug_flags;

	/* Notify broker that this module has finished initialization
	* This occurs only once - we no longer send keepalive
	* messages on every entry/exit of the reactor.
	*/
	if (!ctx->running) {
		flux_msg_t  *msg  =  flux_keepalive_encode (0, FLUX_MODSTATE_RUNNING);
		if (!msg  ||  flux_send (ctx->h, msg, 0) <  0)
			flux_log_error (ctx->h, "error sending keepalive");
		flux_msg_destroy (msg);
		ctx->running  =  true;
	}  

	/* Then, send metric if enabled, or stop watcher if no longer needed.
	*/
	if ((debug_flags  =  flux_aux_get (ctx->h, "flux::debug_flags"))
		&&  *debug_flags  &  FRIPP_ENABLE_MSGCOUNT) {
		flux_msgcounters_t  mcs;
		flux_get_msgcounters (ctx->h, &mcs);

		if (memcmp (&mcs, &ctx->last_mcs, sizeof (mcs))) {
			ctx->last_mcs  =  mcs;
			fripp_send_msgcounters (ctx);
		}
	}
	else
		flux_watcher_stop (ctx->w_prepare);
}
```

KVS message counters received:

![prep watcher kvs](https://i.imgur.com/4bnLftr.png)

Brubeck metrics received:
peak metrics/s ~27,000 |
min metrics/s ~7,000

![prep watcher brubeck metrics/s](https://i.imgur.com/hzYiRfS.png)
___

 #### Using the idle watcher - took ~1h43m5s:
 
```c
static  void  idle_cb (flux_reactor_t  *r, flux_watcher_t  *w, 
		       int  revents, void  *arg)
{
	modservice_ctx_t  *ctx  =  arg;
	int  *debug_flags;

	// Send the metric if enable, or stop watcher if no longer needed.
	if ((debug_flags  =  flux_aux_get (ctx->h, "flux::debug_flags"))
		&&  *debug_flags  &  FRIPP_ENABLE_MSGCOUNT) {
		flux_msgcounters_t  mcs;
		flux_get_msgcounters (ctx->h, &mcs);
		  
		if (memcmp (&mcs, &ctx->last_mcs, sizeof (mcs))) {
			ctx->last_mcs  =  mcs;
			fripp_send_msgcounters (ctx);
		}
	}
	else
		flux_watcher_stop (ctx->w_idle);
}
```

KVS message counters received:

![idle watcher kvs](https://i.imgur.com/ZiJ9rij.png)

Brubeck metrics received:
peak metrics/s ~21,000 |
min metrics/s ~4,500

![idle watcher brubeck metrics/s](https://i.imgur.com/X9pwoql.png)

--- 

#### Using the check watcher - took ~ 1h9m34s:

```c
static  void  check_cb (flux_reactor_t  *r, flux_watcher_t  *w, 
			int  revents, void  *arg)
{
	modservice_ctx_t  *ctx  =  arg;
	int  *debug_flags;


	// Send the metric if enable, or stop watcher if no longer needed.
	if ((debug_flags  =  flux_aux_get (ctx->h, "flux::debug_flags"))
		&&  *debug_flags  &  FRIPP_ENABLE_MSGCOUNT) {
		flux_msgcounters_t  mcs;
		flux_get_msgcounters (ctx->h, &mcs);
		  
		if (memcmp (&mcs, &ctx->last_mcs, sizeof (mcs))) {
			ctx->last_mcs  =  mcs;
			fripp_send_msgcounters (ctx);
		}
	}
	else
		flux_watcher_stop (ctx->w_check);
}
```

KVS message counters received:

![check watcher kvs](https://i.imgur.com/U4OBhSW.png)

Brubeck metrics received:
peak metrics/s ~25,000 | 
min metrics/s ~5,500

![check watcher brubeck metrics/s](https://i.imgur.com/NcWOTLd.png)

Based on metrics/s during a large load of jobs the prep watcher has the highest throughput, followed by the check watcher, which in turn is followed by the idle watcher, but it is worth noting that all of the watchers had a pretty drastic drop in metrics received the longer the instance kept running.

---

After seeing how each of the different watchers performed while under load, I wanted to see how they perform while the instance was sitting idle waiting to do something. So I loaded up an instance, enable the fripp debug flag on all the modules, and let them all sit around for around 10 minutes. Below are the graphs from each module while the message counters were being sent from the check watcher; both the idle and prep watchers produced the same the curves with different graphs.

content-sqlite: 

![check watcher idle content-sqlite](https://i.imgur.com/KH95I74.png)

cron: 

![check watcher idle cron](https://i.imgur.com/rwWTr8a.png)

heartbeat:

![check watcher idle heartbeat](https://i.imgur.com/T4AoxmU.png)

job-exec:

![check watcher idle job-exec](https://i.imgur.com/Q3q9vT9.png)

job-manager:

![check watcher idle job-manager](https://i.imgur.com/24J2gsL.png)

kvs:

![check watcher idle kvs](https://i.imgur.com/N5AT9EQ.png)

sched-simple:

![check watcher idle sched-simple](https://i.imgur.com/qh5zZVu.png)

#### Check watcher - idling ~ 9m8s | ~150 metrics/s

![check watcher idle metrics](https://i.imgur.com/atqPIMF.png)

| message type | content-sqlite | cron | heartbeat | job-exec | job-manager | kvs | sched-simple | 
| :---: |  :---: | :---: |:---: | :---: |:---: | :---: | :---: |
| event_rx | 0 | ~275 |0 | 0 | 0 | ~1,150 | 0 |
| event_tx | 0 | 0 | 0 | 0 | 0 | ~850 | 0 |
| keepalive_rx | 0 | 0 | 0 | 0 | 0|  0 | 0 |
| keepalive_tx | 1 | 1 | 1 | 1 | 1 | 1 | 2 |
| request_rx | ~1750 | 2 | 2 | 3 | 8 | ~900 | 3 |
| request_tx | 8 | 3 | ~275 | 10 | 13 | ~1,800 | 11 |
| response_rx | 8 | 2 | ~275 | 10 | 7 | ~1,800 | 6 |
| response_tx | ~1750 | 1 | 1 | 2 | 12 | ~900 | 2 |


 #### Idle watcher idling ~ 10m57s | ~180 metrics/s

![idle watcher idle metrics/s](https://i.imgur.com/sIJxZd1.png)

| message type | content-sqlite | cron | heartbeat | job-exec | job-manager | kvs | sched-simple | 
| :---: |  :---: | :---: |:---: | :---: |:---: | :---: | :---: |
| event_rx | 0 | ~325 |0 | 0 | 0 | ~1,450 | 0 |
| event_tx | 0 | 0 | 0 | 0 | 0 | ~1,100 | 0 |
| keepalive_rx | 0 | 0 | 0 | 0 | 0|  0 | 0 |
| keepalive_tx | 1 | 1 | 1 | 1 | 1 | 1 | 2 |
| request_rx | ~2250 | 2 | 2 | 3 | 8 | ~1,100 | 3 |
| request_tx | 8 | 3 | ~325 | 10 | 13 | ~2,250 | 11 |
| response_rx | 8 | 2 | ~325 | 10 | 7 | ~2,250 | 6 |
| response_tx | ~2250 | 1 | 1 | 2 | 12 | ~1,100 | 2 |

#### Prep watcher idling ~9m44s | ~192 metrics/s

![prep watcher idle metrics/s](https://i.imgur.com/PGN3zf5.png)

The prep watcher's total message counters by module followed roughly the same pattern as the idle and check watcher.

After observing the metrics being sent from the three watcher types with the instance running jobs constantly and just sitting idle, the prepare watcher seems to be the best place to send them from. The prepare watcher had the highest throughput of messages in all cases I tested, and it also had a high consistency in metrics sent. While the instance was constantly running jobs, both the prep and check watchers had smooth curves (i.e. they were both getting plenty of time to run) whereas the idle watcher had a very jagged graph (i.e. would get varying time to run causing big jumps in the number of metrics sent per interval). While the flux instance was idling with no input, both the check and idle watchers were fairly consistent with not too much varyance in each interval, but the prepare watcher sent roughly the same number of metrics each time with the exception of an outlier.
