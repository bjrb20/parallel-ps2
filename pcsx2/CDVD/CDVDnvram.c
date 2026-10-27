/* See CDVDnvram.h. C89. */
#include <string.h>

#include <streams/file_stream.h>

#include "CDVDnvram.h"

int cdvd_nvram_store(const char *path, const void *data, size_t size)
{
	/* One byte past size, so a longer file reads as changed. */
	uint8_t disk[CDVD_NVRAM_STORE_MAX + 1];
	RFILE  *fp;

	if (size > CDVD_NVRAM_STORE_MAX)
		return -1;
	fp = filestream_open(path, RETRO_VFS_FILE_ACCESS_READ,
			RETRO_VFS_FILE_ACCESS_HINT_NONE);
	if (fp)
	{
		const int64_t got = filestream_read(fp, disk, (int64_t)size + 1);
		filestream_close(fp);
		if (got == (int64_t)size && memcmp(disk, data, size) == 0)
			return 0;
	}
	return filestream_write_file_atomic(path, data, (int64_t)size) ? 1 : -1;
}
