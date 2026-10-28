/* The device caches' insert-only map (Granite's
 * util/insert_only_hash_map.hpp) under contention: inserting threads
 * make every key, each in an order of its own, so the same keys race on
 * the same slots, while looking-up threads read without stopping. The
 * map starts small, so it grows - seals segments and puts new ones in
 * front - many times during the run.
 *
 * Then two threads are released together on one new key at a time,
 * round after round, so they claim the same slot at once.
 *
 * Checked: every thread got the same object for a key, a lookup never
 * returns another key's object, every key is found afterwards, the map
 * holds as many objects as keys and every duplicate made was destroyed,
 * and destroying the map destroys them all. A TSan build sees the races
 * the map must not have.
 *
 * Usage: pgscache [keys] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <rthreads/rthreads.h>
#include <retro_atomic.h>

typedef unsigned long long u64;

void *pgscache_new(unsigned capacity);
void pgscache_delete(void *map);
void *pgscache_insert(void *map, u64 hash, u64 key);
void *pgscache_find(void *map, u64 hash);
u64 pgscache_key(const void *node);
unsigned long pgscache_count(void *map);
long pgscache_live(void);

#define INSERTERS 4
#define READERS   2

static void *s_map;
static unsigned s_keys;
static void **s_got[INSERTERS];
static retro_atomic_int_t s_done;
static unsigned long s_found[READERS], s_wrong[READERS];

/* A bijection, so distinct keys have distinct hashes. */
static u64 hash_of(u64 k)
{
	k ^= k >> 33;
	k *= 0xff51afd7ed558ccdULL;
	k ^= k >> 33;
	k *= 0xc4ceb9fe1a85ec53ULL;
	k ^= k >> 33;
	return k;
}

static retro_atomic_int_t s_go;

static void inserter(void *arg)
{
	unsigned id = (unsigned)(size_t)arg;
	unsigned i, k;
	/* Half the threads in one order, so they race for the same key at
	 * the same moment; the rest each in an order of their own (a stride
	 * coprime with the key count). */
	unsigned stride = (id & 1) ? 2 * id + 1 : 1;
	while (s_keys % stride == 0)
		stride += 2;
	while (!retro_atomic_load_acquire_int(&s_go))
		sthread_yield();
	for (i = 0, k = (id & 1) ? id * 7919u % s_keys : 0; i < s_keys; i++, k = (k + stride) % s_keys)
		s_got[id][k] = pgscache_insert(s_map, hash_of(k), k);
}

/* The rounds: two threads (as many as a small machine runs at once) on
 * key s_keys + round, released together by a spinning barrier. */
#define ROUNDS_MAX 20000
#define RACERS 2
static int s_rounds;
static retro_atomic_int_t s_ready, s_arrived;
static void *s_round_got[ROUNDS_MAX + 1][RACERS];

static void racer(void *arg)
{
	unsigned id = (unsigned)(size_t)arg;
	int r;
	for (r = 1; r <= s_rounds; r++)
	{
		u64 k = (u64)s_keys + (u64)r;
		retro_atomic_fetch_add_int(&s_ready, 1);
		while (retro_atomic_load_acquire_int(&s_ready) < r * RACERS)
			;
		s_round_got[r][id] = pgscache_insert(s_map, hash_of(k), k);
		retro_atomic_fetch_add_int(&s_arrived, 1);
		while (retro_atomic_load_acquire_int(&s_arrived) < r * RACERS)
			;
	}
}

static void reader(void *arg)
{
	unsigned id = (unsigned)(size_t)arg;
	unsigned k = id * 31u;
	while (!retro_atomic_load_acquire_int(&s_done))
	{
		void *n;
		k = (k * 1103515245u + 12345u) % s_keys;
		n = pgscache_find(s_map, hash_of(k));
		if (n)
		{
			s_found[id]++;
			if (pgscache_key(n) != k)
				s_wrong[id]++;
		}
	}
}

int main(int argc, char **argv)
{
	sthread_t *ins[INSERTERS], *rd[READERS];
	unsigned i, k;
	int fail = 0;
	unsigned long found = 0;

	s_keys = argc > 1 ? (unsigned)atoi(argv[1]) : 20000;
	if (s_keys < 16)
		s_keys = 16;
	if (s_keys > ROUNDS_MAX)
		s_keys = ROUNDS_MAX;
	s_rounds = (int)s_keys / 4;
	retro_atomic_int_init(&s_done, 0);
	retro_atomic_int_init(&s_go, 0);
	s_map = pgscache_new(16);
	for (i = 0; i < INSERTERS; i++)
		s_got[i] = (void **)calloc(s_keys, sizeof(void *));
	for (i = 0; i < READERS; i++)
		rd[i] = sthread_create(reader, (void *)(size_t)i);
	for (i = 0; i < INSERTERS; i++)
		ins[i] = sthread_create(inserter, (void *)(size_t)i);
	retro_atomic_store_release_int(&s_go, 1);
	for (i = 0; i < INSERTERS; i++)
		sthread_join(ins[i]);
	retro_atomic_store_release_int(&s_done, 1);
	for (i = 0; i < READERS; i++)
	{
		sthread_join(rd[i]);
		found += s_found[i];
		if (s_wrong[i])
		{
			printf("pgscache: FAIL: a lookup returned another key's object %lu times\n", s_wrong[i]);
			fail = 1;
		}
	}

	for (k = 0; k < s_keys; k++)
	{
		void *n = pgscache_find(s_map, hash_of(k));
		for (i = 0; i < INSERTERS; i++)
			if (!s_got[i][k] || s_got[i][k] != s_got[0][k])
				break;
		if (i != INSERTERS || n != s_got[0][k] || !n || pgscache_key(n) != k)
		{
			printf("pgscache: FAIL: key %u: threads got different objects, or it is not found\n", k);
			fail = 1;
			break;
		}
	}
	/* The rounds. */
	retro_atomic_int_init(&s_ready, 0);
	retro_atomic_int_init(&s_arrived, 0);
	for (i = 0; i < RACERS; i++)
		ins[i] = sthread_create(racer, (void *)(size_t)i);
	for (i = 0; i < RACERS; i++)
		sthread_join(ins[i]);
	for (k = 1; k <= (unsigned)s_rounds && !fail; k++)
	{
		for (i = 1; i < RACERS; i++)
			if (s_round_got[k][i] != s_round_got[k][0])
				break;
		if (i != RACERS || pgscache_find(s_map, hash_of((u64)s_keys + k)) != s_round_got[k][0])
		{
			printf("pgscache: FAIL: round %u: threads making the same key at once got different objects\n", k);
			fail = 1;
		}
	}

	if (pgscache_count(s_map) != s_keys + (unsigned)s_rounds
		|| pgscache_live() != (long)(s_keys + (unsigned)s_rounds))
	{
		printf("pgscache: FAIL: %lu objects in the map, %ld alive, for %u keys\n",
			pgscache_count(s_map), pgscache_live(), s_keys + (unsigned)s_rounds);
		fail = 1;
	}
	pgscache_delete(s_map);
	if (pgscache_live() != 0)
	{
		printf("pgscache: FAIL: %ld objects outlive the map\n", pgscache_live());
		fail = 1;
	}
	for (i = 0; i < INSERTERS; i++)
		free(s_got[i]);
	if (fail)
		return 1;
	printf("pgscache: %u keys by %d threads each, %lu lookups alongside, %d keys raced by %d at once: ok\n",
		s_keys, INSERTERS, found, s_rounds, RACERS);
	return 0;
}
