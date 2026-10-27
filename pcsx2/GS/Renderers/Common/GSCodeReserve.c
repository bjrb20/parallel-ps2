/* See GSCodeReserve.h. C89. */
#include "GSCodeReserve.h"
#include "../../../Memory.h"

struct GSCodeReserve g_gs_code_reserve;

void gs_code_reserve_assign(const VirtualMemoryManager* code_map)
{
	code_reserve_assign(&g_gs_code_reserve.reserve, code_map, HOSTMEM_SWREC_OFFSET, HOSTMEM_SWREC_SIZE);
	g_gs_code_reserve.used = 0;
}

void gs_code_reserve_release(void)
{
	code_reserve_release(&g_gs_code_reserve.reserve);
	g_gs_code_reserve.used = 0;
}
