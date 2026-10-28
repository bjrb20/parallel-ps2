/* The device caches' map, Util::InsertOnlyHashMap, behind a C interface
 * for main.c: nodes that count themselves, so a duplicate an insert made
 * and threw away shows in the count. */
#include "insert_only_hash_map.hpp"
#include "intrusive_hash_map.hpp"
#include <stdint.h>

namespace
{
retro_atomic_int_t live;

struct Node : Util::IntrusiveHashMapEnabled<Node>
{
	explicit Node(uint64_t key_)
		: key(key_)
	{
		retro_atomic_fetch_add_int(&live, 1);
	}
	~Node()
	{
		retro_atomic_fetch_sub_int(&live, 1);
	}
	uint64_t key;
};

typedef Util::InsertOnlyHashMap<Node> Map;
}

extern "C" {
void *pgscache_new(unsigned capacity)
{
	return new Map(capacity);
}

void pgscache_delete(void *map)
{
	delete static_cast<Map *>(map);
}

void *pgscache_insert(void *map, uint64_t hash, uint64_t key)
{
	Map *m = static_cast<Map *>(map);
	Node *n = m->find(hash);
	return n ? n : m->emplace_yield(hash, key);
}

void *pgscache_find(void *map, uint64_t hash)
{
	return static_cast<Map *>(map)->find(hash);
}

uint64_t pgscache_key(const void *node)
{
	return static_cast<const Node *>(node)->key;
}

unsigned long pgscache_count(void *map)
{
	unsigned long n = 0;
	static_cast<Map *>(map)->for_each([&n](Node &) { n++; });
	return n;
}

long pgscache_live(void)
{
	return retro_atomic_load_acquire_int(&live);
}
}
