/* The mechacon NVRAM file (CDVD.cpp): loaded after each BIOS load, saved on
 * shutdown. C, so Memory.c reaches it; CDVD.h includes this. */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

extern void cdvdLoadNVRAM(void);
extern void cdvdSaveNVRAM(void);

#ifdef __cplusplus
}
#endif
