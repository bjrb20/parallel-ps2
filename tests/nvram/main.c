/* cdvd_nvram_store (pcsx2/CDVD/CDVDnvram.c): an unchanged file is left
 * alone, a changed one is replaced whole and leaves no temporary behind,
 * and a failed store creates nothing. C89. */
#include <stdio.h>
#include <string.h>

#include <streams/file_stream.h>

#include "CDVD/CDVDnvram.h"

static int failures;

#define CHECK(cond, what) do { if (!(cond)) { printf("FAIL: %s\n", what); failures++; } } while (0)

static int64_t file_bytes(const char *path, unsigned char *buf, int64_t cap)
{
	int64_t got;
	RFILE  *fp = filestream_open(path, RETRO_VFS_FILE_ACCESS_READ,
			RETRO_VFS_FILE_ACCESS_HINT_NONE);
	if (!fp)
		return -1;
	got = filestream_read(fp, buf, cap);
	filestream_close(fp);
	return got;
}

static int file_exists(const char *path)
{
	RFILE *fp = filestream_open(path, RETRO_VFS_FILE_ACCESS_READ,
			RETRO_VFS_FILE_ACCESS_HINT_NONE);
	if (!fp)
		return 0;
	filestream_close(fp);
	return 1;
}

int main(int argc, char **argv)
{
	static unsigned char a[CDVD_NVRAM_STORE_MAX];
	static unsigned char b[CDVD_NVRAM_STORE_MAX];
	static unsigned char got[CDVD_NVRAM_STORE_MAX + 16];
	char path[1024];
	char tmp[1100];
	char missing[1100];
	unsigned i;

	if (argc < 2)
	{
		printf("usage: %s <scratch dir>\n", argv[0]);
		return 2;
	}
	sprintf(path, "%s/nvram_test.nvm", argv[1]);
	sprintf(tmp, "%s.tmp", path);
	sprintf(missing, "%s/no_such_dir/nvram_test.nvm", argv[1]);
	filestream_delete(path);
	filestream_delete(tmp);

	for (i = 0; i < sizeof(a); i++)
	{
		a[i] = (unsigned char)(i * 7 + 1);
		b[i] = (unsigned char)(i * 13 + 5);
	}

	/* No file yet: written. */
	CHECK(cdvd_nvram_store(path, a, sizeof(a)) == 1, "a new file is written");
	CHECK(file_bytes(path, got, sizeof(got)) == (int64_t)sizeof(a)
			&& memcmp(got, a, sizeof(a)) == 0, "the new file holds the data");
	CHECK(!file_exists(tmp), "no temporary is left after a write");

	/* The same contents again: nothing to do, and the file still holds
	 * them. A store that empties the file before comparing never gets
	 * here. */
	CHECK(cdvd_nvram_store(path, a, sizeof(a)) == 0, "an unchanged file is left alone");
	CHECK(file_bytes(path, got, sizeof(got)) == (int64_t)sizeof(a)
			&& memcmp(got, a, sizeof(a)) == 0, "the unchanged file still holds the data");

	/* Different contents: replaced. */
	CHECK(cdvd_nvram_store(path, b, sizeof(b)) == 1, "a changed file is written");
	CHECK(file_bytes(path, got, sizeof(got)) == (int64_t)sizeof(b)
			&& memcmp(got, b, sizeof(b)) == 0, "the changed file holds the new data");

	/* A file that starts with the data but runs longer is not the data. */
	CHECK(cdvd_nvram_store(path, b, sizeof(b) / 2) == 1, "a longer file counts as changed");
	CHECK(file_bytes(path, got, sizeof(got)) == (int64_t)(sizeof(b) / 2),
			"the file is cut to the stored size");

	/* A small record (the MEC version) round-trips the same way. */
	CHECK(cdvd_nvram_store(path, a, 4) == 1, "a 4-byte record is written");
	CHECK(cdvd_nvram_store(path, a, 4) == 0, "an unchanged 4-byte record is left alone");

	/* Failures: nothing created, the existing file untouched. */
	CHECK(cdvd_nvram_store(missing, a, sizeof(a)) == -1, "an unwritable path fails");
	CHECK(!file_exists(missing), "a failed store creates nothing");
	CHECK(cdvd_nvram_store(path, a, CDVD_NVRAM_STORE_MAX + 1) == -1, "an oversized store fails");
	CHECK(file_bytes(path, got, sizeof(got)) == 4 && memcmp(got, a, 4) == 0,
			"a failed store leaves the existing file as it was");

	filestream_delete(path);
	filestream_delete(tmp);

	if (failures)
	{
		printf("nvram: %d check(s) failed\n", failures);
		return 1;
	}
	printf("nvram: all checks passed\n");
	return 0;
}
