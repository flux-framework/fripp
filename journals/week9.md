# Week 9: Implementation

### Fixing some FRIPP issues

#### Fixing the zhashx Destructor problem

In the past when trying to clear entries from the `zhashx`, if I set the global destructor in `fripp_ctx_create()`, it would be called early and segfault.

```c
zhashx_set_destructor (ctx->metrics, metric_destroy);
```

The problem was that I was using the `zhashx_update()` (thanks java) function every time any of the metric functions got called (`count`, `gauge`, or `timing`), and the update function calls the destructor on the element currently stored for that hash. That is also why the metric collection and sending worked when setting the destructor on each element right before deleting it since update didn't call a destructor on the element if one wasn't set. 

```c
if (!(m = zhashx_lookup (ctx->metrics, name))) {
	if (!(m = malloc (sizeof (*m))))
		return -1;
}

...

zhashx_update (ctx->metrics, name, (void  *) m);
```

So to fix this, I instead just used the `zhashx_insert()` function if the hash wasn't found in the table. Then the metric's values can be updated. The previous value is also set to a value of `-1` on creation since `0` is a valid value to be sent, and is also very common.

```c
if (!(m = zhashx_lookup (ctx->metrics, name))) {
	if (!(m = malloc (sizeof (*m))))
		return -1;
		
	zhashx_insert (ctx->metrics, name, (void *) m);
	m->prev.l = -1;
}
```

The last piece that needed to be changed was in the `timer_cb`. Instead of setting the destructor and then destroying it, each hash in the `done` list can just be deleted right away.

```c
// original
char  *s;
while ((s =  zlist_pop (ctx->done))) {
	zhashx_freefn (ctx->metrics, s, metric_destroy);
	zhashx_delete (ctx->metrics, s);
}
```

```c
// new
char  *s;
while ((s  =  zlist_pop (ctx->done)))
	zhashx_delete (ctx->metrics, s);
```

---

#### Fixing the Gauge Increments

The gauge type is semi-special in that it can handle increments on the aggregator's end (brubeck/statsd). Originally, the gauge inside of fripp took advantage of this in the `timer_cb`, by sending the values that were incs as incs.

```c
FOREACH_ZHASHX (ctx->metrics, name, m) {
	case  BRUBECK_GAUGE:
		rc  =  fripp_packet_appendf (ctx,
					     m->inc  &&  m->cur.l  >  0  ?
					     "%s:+%zd|g\n" : "%s:%zd|g\n",
					     name,
					     m->cur.l);
		break;
		     
	...
}
```

However, there seemed to be some loss of data at some point in the aggregation which caused weird number's to be flushed to graphite (like -5 jobs running). So, now the gauge handles the increments internally and just sends the value as a standard gauge.

```c
FOREACH_ZHASHX (ctx->metrics, name, m) {
	case  BRUBECK_GAUGE:
		rc  =  fripp_packet_appendf (ctx, "%s:%zd|g\n", name, m->cur.l);
		break;
	
	...
}
```


---

#### Properly Stopping Sending Unchanged Metrics

The last major change (I think) made this week was to properly stop sending metrics when they aren't being updated. Since each metric has two `union`s of a `ssize_t` and a `double` each, comparing just one type didn't always work. I believe it may have something to do with the implicit casting in c but I'm not sure.

```c

FOREACH_ZHASHX (ctx->metrics, name, m) {
	if (m->cur.d  ==  m->prev.d) {
		zlist_append (ctx->done, (void  *) name);
		continue;
	}

	...
}
```

This caused some metrics to never stop sending as the previous value was "never equal" to the current value. For instance, the jobs in the `new` state would keep sending -10,000 and both the current and previous values were showing -10,000 but it wasn't coming out as equal in the above snippet. So, to fix this piece I added another conditional (although it could be collapsed into one) and they check the union value based on the metric type.

```c
FOREACH_ZHASHX (ctx->metrics, name, m) {
	if ((m->type  ==  BRUBECK_COUNTER  ||  m->type  ==  BRUBECK_GAUGE)
			&&  m->cur.l  ==  m->prev.l) {
		zlist_append (ctx->done, (void  *) name);
		continue;
	}
	if (m->type  ==  BRUBECK_TIMER  &&  m->cur.d  ==  m->prev.d) {
		zlist_append (ctx->done, (void  *) name);
		continue;
	}
	
	...
}
```
