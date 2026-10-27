/* The GS software JIT's code buffer: the SW rec range of the code map,
 * filled front to back and started over by gs_code_reserve_reset. C89. */
#pragma once

#include "../../../VirtualMemory.h"

#ifdef __cplusplus
extern "C" {
#endif

struct GSCodeReserve
{
	struct CodeReserve reserve;
	size_t used;
};

extern struct GSCodeReserve g_gs_code_reserve;

void gs_code_reserve_assign(const VirtualMemoryManager* code_map);
void gs_code_reserve_release(void);

#define gs_code_reserve_reset() (g_gs_code_reserve.used = 0)

/* Room for n bytes of code at the end of what is in use, or NULL when
 * the reserve does not have it: gs_code_reserve_reset starts it again. */
#define gs_code_reserve_reserve(n) \
	((g_gs_code_reserve.reserve.baseptr && (n) <= g_gs_code_reserve.reserve.size - g_gs_code_reserve.used) \
		? g_gs_code_reserve.reserve.baseptr + g_gs_code_reserve.used : (u8*)NULL)
#define gs_code_reserve_commit(n) (g_gs_code_reserve.used += (n))

#ifdef __cplusplus
}
#endif
