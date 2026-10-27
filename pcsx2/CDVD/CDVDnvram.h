/* The mechacon NVRAM file (CDVD.c): loaded after each BIOS load, saved on
 * shutdown. C, so Memory.c reaches it; CDVD.h includes this. */
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

extern void cdvdLoadNVRAM(void);
extern void cdvdSaveNVRAM(void);

#define CDVD_NVRAM_STORE_MAX 1024

/* Writes the size bytes at data to path, unless path already holds
 * exactly them. The file is replaced whole (filestream_write_file_atomic),
 * so an interrupted save leaves the previous contents. Returns 0 when it
 * was unchanged, 1 when written, -1 on failure or size above
 * CDVD_NVRAM_STORE_MAX. */
int cdvd_nvram_store(const char *path, const void *data, size_t size);

#ifdef __cplusplus
}
#endif
