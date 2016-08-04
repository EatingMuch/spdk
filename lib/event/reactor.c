/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "spdk/event.h"

#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>

#ifdef __linux__
#include <sys/prctl.h>
#endif

#ifdef __FreeBSD__
#include <pthread_np.h>
#endif

#include <rte_config.h>
#include <rte_cycles.h>
#include <rte_debug.h>
#include <rte_mempool.h>
#include <rte_ring.h>
#include <rte_timer.h>

#include "reactor.h"

#include "spdk/log.h"

#define SPDK_MAX_SOCKET		64

enum spdk_reactor_state {
	SPDK_REACTOR_STATE_INVALID = 0,
	SPDK_REACTOR_STATE_INITIALIZED = 1,
	SPDK_REACTOR_STATE_RUNNING = 2,
	SPDK_REACTOR_STATE_EXITING = 3,
	SPDK_REACTOR_STATE_SHUTDOWN = 4,
};

struct spdk_reactor {
	/* Logical core number for this reactor. */
	uint32_t					lcore;

	/*
	 * Contains pollers actively running on this reactor.  Pollers
	 *  are run round-robin. The reactor takes one poller from the head
	 *  of the ring, executes it, then puts it back at the tail of
	 *  the ring.
	 */
	TAILQ_HEAD(, spdk_poller)			active_pollers;

	/**
	 * Contains pollers running on this reactor with a periodic timer.
	 */
	TAILQ_HEAD(timer_pollers_head, spdk_poller)	timer_pollers;

	struct rte_ring					*events;
};

static struct spdk_reactor g_reactors[RTE_MAX_LCORE];
static uint64_t	g_reactor_mask  = 0;
static int	g_reactor_count = 0;

static enum spdk_reactor_state	g_reactor_state = SPDK_REACTOR_STATE_INVALID;

static void spdk_reactor_construct(struct spdk_reactor *w, uint32_t lcore);

struct rte_mempool *g_spdk_event_mempool[SPDK_MAX_SOCKET];

/** \file

*/

static struct spdk_reactor *
spdk_reactor_get(uint32_t lcore)
{
	struct spdk_reactor *reactor;
	reactor = &g_reactors[lcore];
	return reactor;
}

spdk_event_t
spdk_event_allocate(uint32_t lcore, spdk_event_fn fn, void *arg1, void *arg2,
		    spdk_event_t next)
{
	struct spdk_event *event = NULL;
	int rc;
	uint8_t socket_id = rte_lcore_to_socket_id(lcore);
	RTE_VERIFY(socket_id < SPDK_MAX_SOCKET);

	rc = rte_mempool_get(g_spdk_event_mempool[socket_id], (void **)&event);
	RTE_VERIFY((rc == 0) && (event != NULL));

	event->lcore = lcore;
	event->fn = fn;
	event->arg1 = arg1;
	event->arg2 = arg2;
	event->next = next;

	return event;
}

static void
spdk_event_free(uint32_t lcore, struct spdk_event *event)
{
	uint8_t socket_id = rte_lcore_to_socket_id(lcore);
	RTE_VERIFY(socket_id < SPDK_MAX_SOCKET);

	rte_mempool_put(g_spdk_event_mempool[socket_id], (void *)event);
}

void
spdk_event_call(spdk_event_t event)
{
	int rc;
	struct spdk_reactor *reactor;

	reactor = spdk_reactor_get(event->lcore);

	RTE_VERIFY(reactor->events != NULL);
	rc = rte_ring_enqueue(reactor->events, event);
	RTE_VERIFY(rc == 0);
}

static uint32_t
spdk_event_queue_count(uint32_t lcore)
{
	struct spdk_reactor *reactor;

	reactor = spdk_reactor_get(lcore);

	if (reactor->events == NULL) {
		return 0;
	}

	return rte_ring_count(reactor->events);
}

static void
spdk_event_queue_run_single(uint32_t lcore)
{
	struct spdk_event *event = NULL;
	struct spdk_reactor *reactor;
	int rc;

	reactor = spdk_reactor_get(lcore);

	RTE_VERIFY(reactor->events != NULL);
	rc = rte_ring_dequeue(reactor->events, (void **)&event);

	if ((rc != 0) || event == NULL) {
		return;
	}

	event->fn(event);
	spdk_event_free(lcore, event);
}

static void
spdk_event_queue_run(uint32_t lcore, uint32_t count)
{
	while (count--) {
		spdk_event_queue_run_single(lcore);
	}
}

void
spdk_event_queue_run_all(uint32_t lcore)
{
	uint32_t count;

	count = spdk_event_queue_count(lcore);
	spdk_event_queue_run(lcore, count);
}

/**

\brief Set current reactor thread name to "reactor <cpu #>".

This makes the reactor threads distinguishable in top and gdb.

*/
static void set_reactor_thread_name(void)
{
	char thread_name[16];

	snprintf(thread_name, sizeof(thread_name), "reactor %d",
		 rte_lcore_id());

#if defined(__linux__)
	prctl(PR_SET_NAME, thread_name, 0, 0, 0);
#elif defined(__FreeBSD__)
	pthread_set_name_np(pthread_self(), thread_name);
#else
#error missing platform support for thread name
#endif
}

static void
spdk_poller_insert_timer(struct spdk_reactor *reactor, struct spdk_poller *poller, uint64_t now)
{
	struct spdk_poller *iter;
	uint64_t next_run_tick;

	next_run_tick = now + poller->period_ticks;
	poller->next_run_tick = next_run_tick;

	/*
	 * Insert poller in the reactor's timer_pollers list in sorted order by next scheduled
	 * run time.
	 */
	TAILQ_FOREACH_REVERSE(iter, &reactor->timer_pollers, timer_pollers_head, tailq) {
		if (iter->next_run_tick <= next_run_tick) {
			TAILQ_INSERT_AFTER(&reactor->timer_pollers, iter, poller, tailq);
			return;
		}
	}

	/* No earlier pollers were found, so this poller must be the new head */
	TAILQ_INSERT_HEAD(&reactor->timer_pollers, poller, tailq);
}

/**

\brief This is the main function of the reactor thread.

\code

while (1)
	if (new work items to be scheduled)
		dequeue work item from new work item ring
		enqueue work item to active work item ring
	else if (active work item count > 0)
		dequeue work item from active work item ring
		invoke work item function pointer
		if (work item state == RUNNING)
			enqueue work item to active work item ring
	else if (application state != RUNNING)
		# exit the reactor loop
		break
	else
		sleep for 100ms

\endcode

Note that new work items are posted to a separate ring so that the
active work item ring can be kept single producer/single consumer and
only be touched by reactor itself.  This avoids atomic operations
on the active work item ring which would hurt performance.

*/
static int
_spdk_reactor_run(void *arg)
{
	struct spdk_reactor	*reactor = arg;
	struct spdk_poller	*poller;

	set_reactor_thread_name();
	SPDK_NOTICELOG("waiting for work item to arrive...\n");

	while (1) {
		spdk_event_queue_run_all(rte_lcore_id());

		rte_timer_manage();

		poller = TAILQ_FIRST(&reactor->active_pollers);
		if (poller) {
			TAILQ_REMOVE(&reactor->active_pollers, poller, tailq);
			poller->fn(poller->arg);
			TAILQ_INSERT_TAIL(&reactor->active_pollers, poller, tailq);
		}

		poller = TAILQ_FIRST(&reactor->timer_pollers);
		if (poller) {
			uint64_t now = rte_get_timer_cycles();

			if (now >= poller->next_run_tick) {
				TAILQ_REMOVE(&reactor->timer_pollers, poller, tailq);
				poller->fn(poller->arg);
				spdk_poller_insert_timer(reactor, poller, now);
			}
		}

		if (g_reactor_state != SPDK_REACTOR_STATE_RUNNING) {
			break;
		}
	}

	return 0;
}

static void
spdk_reactor_construct(struct spdk_reactor *reactor, uint32_t lcore)
{
	char	ring_name[64];

	reactor->lcore = lcore;

	TAILQ_INIT(&reactor->active_pollers);
	TAILQ_INIT(&reactor->timer_pollers);

	snprintf(ring_name, sizeof(ring_name) - 1, "spdk_event_queue_%u", lcore);
	reactor->events =
		rte_ring_create(ring_name, 65536, rte_lcore_to_socket_id(lcore), RING_F_SC_DEQ);
	RTE_VERIFY(reactor->events != NULL);
}

static void
spdk_reactor_start(struct spdk_reactor *reactor)
{
	if (reactor->lcore != rte_get_master_lcore()) {
		switch (rte_eal_get_lcore_state(reactor->lcore)) {
		case FINISHED:
			rte_eal_wait_lcore(reactor->lcore);
		/* drop through */
		case WAIT:
			rte_eal_remote_launch(_spdk_reactor_run, (void *)reactor, reactor->lcore);
			break;
		case RUNNING:
			printf("Something already running on lcore %d\n", reactor->lcore);
			break;
		}
	} else {
		_spdk_reactor_run(reactor);
	}
}

int
spdk_app_get_core_count(void)
{
	return g_reactor_count;
}

uint32_t
spdk_app_get_current_core(void)
{
	return rte_lcore_id();
}

int
spdk_app_parse_core_mask(const char *mask, uint64_t *cpumask)
{
	unsigned int i;
	char *end;

	if (mask == NULL || cpumask == NULL) {
		return -1;
	}

	errno = 0;
	*cpumask = strtoull(mask, &end, 16);
	if (*end != '\0' || errno) {
		return -1;
	}

	for (i = 0; i < RTE_MAX_LCORE && i < 64; i++) {
		if ((*cpumask & (1ULL << i)) && !rte_lcore_is_enabled(i)) {
			*cpumask &= ~(1ULL << i);
		}
	}

	return 0;
}

static int
spdk_reactor_parse_mask(const char *mask)
{
	int i;
	int ret = 0;
	uint32_t master_core = rte_get_master_lcore();

	if (g_reactor_state >= SPDK_REACTOR_STATE_INITIALIZED) {
		SPDK_ERRLOG("cannot set reactor mask after application has started\n");
		return -1;
	}

	g_reactor_mask = 0;

	if (mask == NULL) {
		/* No mask specified so use the same mask as DPDK. */
		RTE_LCORE_FOREACH(i) {
			g_reactor_mask |= (1ULL << i);
		}
	} else {
		ret = spdk_app_parse_core_mask(mask, &g_reactor_mask);
		if (ret != 0) {
			SPDK_ERRLOG("reactor mask %s specified on command line "
				    "is invalid\n", mask);
			return ret;
		}
		if (!(g_reactor_mask & (1ULL << master_core))) {
			SPDK_ERRLOG("master_core %d must be set in core mask\n", master_core);
			return -1;
		}
	}

	return 0;
}

uint64_t
spdk_app_get_core_mask(void)
{
	return g_reactor_mask;
}


static uint64_t
spdk_reactor_get_socket_mask(void)
{
	int i;
	uint32_t socket_id;
	uint64_t socket_info = 0;

	RTE_LCORE_FOREACH(i) {
		if (((1ULL << i) & g_reactor_mask)) {
			socket_id = rte_lcore_to_socket_id(i);
			socket_info |= (1ULL << socket_id);
		}
	}

	return socket_info;
}

void
spdk_reactors_start(void)
{
	struct spdk_reactor *reactor;
	uint32_t i;

	RTE_VERIFY(rte_get_master_lcore() == rte_lcore_id());

	g_reactor_state = SPDK_REACTOR_STATE_RUNNING;

	RTE_LCORE_FOREACH_SLAVE(i) {
		if (((1ULL << i) & spdk_app_get_core_mask())) {
			reactor = spdk_reactor_get(i);
			spdk_reactor_start(reactor);
		}
	}

	/* Start the master reactor */
	reactor = spdk_reactor_get(rte_get_master_lcore());
	spdk_reactor_start(reactor);

	rte_eal_mp_wait_lcore();

	g_reactor_state = SPDK_REACTOR_STATE_SHUTDOWN;
}

void spdk_reactors_stop(void)
{
	g_reactor_state = SPDK_REACTOR_STATE_EXITING;
}

int
spdk_reactors_init(const char *mask)
{
	uint32_t i;
	int rc;
	struct spdk_reactor *reactor;
	uint64_t socket_mask = 0x0;
	uint8_t socket_count = 0;
	char mempool_name[32];

	rc = spdk_reactor_parse_mask(mask);
	if (rc < 0) {
		return rc;
	}

	printf("Occupied cpu core mask is 0x%lx\n", spdk_app_get_core_mask());

	RTE_LCORE_FOREACH(i) {
		if (((1ULL << i) & spdk_app_get_core_mask())) {
			reactor = spdk_reactor_get(i);
			spdk_reactor_construct(reactor, i);
			g_reactor_count++;
		}
	}

	socket_mask = spdk_reactor_get_socket_mask();
	printf("Occupied cpu socket mask is 0x%lx\n", socket_mask);

	for (i = 0; i < SPDK_MAX_SOCKET; i++) {
		if ((1ULL << i) & socket_mask) {
			socket_count++;
		}
	}

	for (i = 0; i < SPDK_MAX_SOCKET; i++) {
		if ((1ULL << i) & socket_mask) {
			snprintf(mempool_name, sizeof(mempool_name), "spdk_event_mempool_%d", i);
			g_spdk_event_mempool[i] = rte_mempool_create(mempool_name,
						  (262144 / socket_count),
						  sizeof(struct spdk_event), 128, 0,
						  NULL, NULL, NULL, NULL, i, 0);

			if (g_spdk_event_mempool[i] == NULL) {
				SPDK_ERRLOG("spdk_event_mempool creation failed on socket %d\n", i);

				/*
				 * Instead of failing the operation directly, try to create
				 * the mempool on any available sockets in the case that
				 * memory is not evenly installed on all sockets. If still
				 * fails, free all allocated memory and exits.
				 */
				g_spdk_event_mempool[i] = rte_mempool_create(
								  mempool_name,
								  (262144 / socket_count),
								  sizeof(struct spdk_event),
								  128, 0,
								  NULL, NULL, NULL, NULL,
								  SOCKET_ID_ANY, 0);

				/* TODO: in DPDK 16.04, free mempool API is avaialbe. */
				if (g_spdk_event_mempool[i] == NULL) {
					SPDK_ERRLOG("spdk_event_mempool creation failed\n");
					return -1;
				}
			}
		}
	}

	g_reactor_state = SPDK_REACTOR_STATE_INITIALIZED;

	return rc;
}

int
spdk_reactors_fini(void)
{
	/* TODO: free rings and mempool */
	return 0;
}

static void
_spdk_event_add_poller(spdk_event_t event)
{
	struct spdk_reactor *reactor = spdk_event_get_arg1(event);
	struct spdk_poller *poller = spdk_event_get_arg2(event);
	struct spdk_event *next = spdk_event_get_next(event);

	poller->lcore = reactor->lcore;

	if (poller->period_ticks) {
		spdk_poller_insert_timer(reactor, poller, rte_get_timer_cycles());
	} else {
		TAILQ_INSERT_TAIL(&reactor->active_pollers, poller, tailq);
	}

	if (next) {
		spdk_event_call(next);
	}
}

static void
_spdk_poller_register(struct spdk_poller *poller, uint32_t lcore,
		      struct spdk_event *complete)
{
	struct spdk_reactor *reactor;
	struct spdk_event *event;

	reactor = spdk_reactor_get(lcore);
	event = spdk_event_allocate(lcore, _spdk_event_add_poller, reactor, poller, complete);
	spdk_event_call(event);
}

void
spdk_poller_register(struct spdk_poller *poller,
		     uint32_t lcore, struct spdk_event *complete, uint64_t period_microseconds)
{
	if (period_microseconds) {
		poller->period_ticks = (rte_get_timer_hz() * period_microseconds) / 1000000ULL;
	} else {
		poller->period_ticks = 0;
	}

	_spdk_poller_register(poller, lcore, complete);
}

static void
_spdk_event_remove_poller(spdk_event_t event)
{
	struct spdk_reactor *reactor = spdk_event_get_arg1(event);
	struct spdk_poller *poller = spdk_event_get_arg2(event);
	struct spdk_event *next = spdk_event_get_next(event);

	if (poller->period_ticks) {
		TAILQ_REMOVE(&reactor->timer_pollers, poller, tailq);
	} else {
		TAILQ_REMOVE(&reactor->active_pollers, poller, tailq);
	}

	if (next) {
		spdk_event_call(next);
	}
}

void
spdk_poller_unregister(struct spdk_poller *poller,
		       struct spdk_event *complete)
{
	struct spdk_reactor *reactor;
	struct spdk_event *event;

	reactor = spdk_reactor_get(poller->lcore);
	event = spdk_event_allocate(poller->lcore, _spdk_event_remove_poller, reactor, poller, complete);

	spdk_event_call(event);
}

static void
_spdk_poller_migrate(spdk_event_t event)
{
	struct spdk_poller *poller = spdk_event_get_arg1(event);
	struct spdk_event *next = spdk_event_get_next(event);

	/* Register the poller on the current lcore. This works
	 * because we already set this event up so that it is called
	 * on the new_lcore.
	 */
	_spdk_poller_register(poller, rte_lcore_id(), next);
}

void
spdk_poller_migrate(struct spdk_poller *poller, int new_lcore,
		    struct spdk_event *complete)
{
	struct spdk_event *event;

	RTE_VERIFY(spdk_app_get_core_mask() & (1ULL << new_lcore));
	RTE_VERIFY(poller != NULL);

	event = spdk_event_allocate(new_lcore, _spdk_poller_migrate, poller, NULL, complete);

	spdk_poller_unregister(poller, event);
}
