/* The EE's view of the GS privileged registers: their storage and the
 * handlers the vtlb maps over 0x12000000 (GS.cpp). C, so Memory.c reaches
 * them; GS.h includes this. */
#pragma once

#include "MemoryTypes.h"
#include "../common/SingleRegisterTypes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* MSVC decorates C++ data symbols, so the register storage carries C
 * linkage too. */
PCSX2_ALIGN(16) extern u8 g_RealGSMem[PS2MEM_GS_REGS];

#define PS2MEM_GS g_RealGSMem
#define PS2GS_BASE(mem) (PS2MEM_GS + ((mem) & 0x13ff))

extern void gsWrite8(u32 mem, u8 value);
extern void gsWrite16(u32 mem, u16 value);
extern void gsWrite32(u32 mem, u32 value);

extern void gsWrite64_page_00(u32 mem, u64 value);
extern void gsWrite64_page_01(u32 mem, u64 value);
extern void gsWrite64_generic(u32 mem, u64 value);

#if PCSX2_MINGW_R128_BY_PTR
extern void gsWrite128_page_01(u32 mem, const r128* value);
extern void gsWrite128_generic(u32 mem, const r128* value);
#else
extern void TAKES_R128 gsWrite128_page_01(u32 mem, r128 value);
extern void TAKES_R128 gsWrite128_generic(u32 mem, r128 value);
#endif

extern u8 gsRead8(u32 mem);
extern u16 gsRead16(u32 mem);
extern u32 gsRead32(u32 mem);
extern u64 gsRead64(u32 mem);

#ifdef __cplusplus
}
#endif
