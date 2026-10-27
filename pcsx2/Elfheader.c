/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2010  PCSX2 Dev Team
 *
 *  PCSX2 is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU Lesser General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  PCSX2 is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with PCSX2.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

/* See Elfheader.h. C89.
 *
 * The CRC is the XOR of the image's whole 32-bit words, so it is taken as
 * the image streams past, a chunk at a time: the image is never held in
 * memory whole. */
#include <stdlib.h>
#include <string.h>

#include <streams/file_stream.h>
#include <compat/strl.h>

#include "common/Pcsx2Defs.h"
#include "Elfheader.h"
#include "CDVD/IsoFS/iso9660.h"

#define ELF_CHUNK (64 * 1024)

u32 ElfCRC;
u32 ElfEntry;
char LastELF[PCSX2_PATH_MAX];

static bool elf_size_ok(s64 size)
{
	if (size > 0xfffffff)
		log_cb(RETRO_LOG_ERROR, "Illegal ELF file size over 2GB!\n");
	else if (size == -1)
		log_cb(RETRO_LOG_ERROR, "ELF file does not exist!\n");
	else if (size <= (s64)sizeof(ELF_HEADER))
		log_cb(RETRO_LOG_ERROR, "Unexpected end of ELF file.\n");
	else
		return true;
	return false;
}

/* Folds n bytes, a whole number of words, into the CRC. */
static u32 elf_crc_fold(u32 crc, const u8* p, size_t n)
{
	size_t i;
	for (i = 0; i < n; i += 4)
	{
		u32 w;
		memcpy(&w, p + i, 4);
		crc ^= w;
	}
	return crc;
}

typedef s64 (*elf_read_t)(void* src, void* dst, u32 len);

static s64 elf_read_iso(void* src, void* dst, u32 len)
{
	return isofs_file_read((isofs_file_t*)src, dst, (s32)len);
}

static s64 elf_read_host(void* src, void* dst, u32 len)
{
	return filestream_read((RFILE*)src, dst, len);
}

/* size bytes through read: the header's entry point, and the CRC of the
 * whole words. */
static bool elf_identify(elf_read_t read, void* src, s64 length, u32* crc, u32* entry)
{
	u8* buf;
	u32 size;
	u32 done = 0;
	u32 sum  = 0;

	if (!elf_size_ok(length) || !(buf = (u8*)malloc(ELF_CHUNK)))
		return false;
	size = (u32)length;
	while (done < size)
	{
		const u32 want = size - done < ELF_CHUNK ? size - done : ELF_CHUNK;
		if (read(src, buf, want) != (s64)want)
		{
			free(buf);
			return false;
		}
		if (!done)
		{
			ELF_HEADER h;
			memcpy(&h, buf, sizeof(h));
			*entry = h.e_entry;
		}
		/* a chunk is a whole number of words but the last; its tail
		 * bytes are no word */
		sum   = elf_crc_fold(sum, buf, want & ~3u);
		done += want;
	}
	free(buf);
	*crc = sum;
	return true;
}

bool elf_identify_iso(struct isofs_file* f, u32* crc, u32* entry)
{
	return elf_identify(elf_read_iso, f, f->length, crc, entry);
}

bool elf_identify_host(const char* path, u32* crc, u32* entry)
{
	bool   ok;
	RFILE* fp = filestream_open(path, RETRO_VFS_FILE_ACCESS_READ, RETRO_VFS_FILE_ACCESS_HINT_NONE);
	if (!fp)
		return false;
	ok = elf_identify(elf_read_host, fp, filestream_get_size(fp), crc, entry);
	filestream_close(fp);
	return ok;
}

/* Sets crc and entry to those of the game's ELF at elfpath: "host:" and a
 * path on the host, or a path on the disc. */
bool elf_identify_path(const char* elfpath, u32* crc, u32* entry)
{
	char          path[PCSX2_PATH_MAX];
	char*         semi;
	isofs_file_t* file;
	bool          ok;

	if (!strncmp(elfpath, "host:", 5))
		return elf_identify_host(elfpath + 5, crc, entry);

	/* Mimic PS2 behavior! */
	/* Much trial-and-error with changing the ISOFS and BOOT2 contents of an image have shown that */
	/* the PS2 BIOS performs the peculiar task of *ignoring* the version info from the parsed BOOT2 */
	/* filename *and* the ISOFS, when loading the game's ELF image.  What this means is: */
	/* */
	/*   1. a valid PS2 ELF can have any version (ISOFS), and the version need not match the one in SYSTEM.CNF. */
	/*   2. the version info on the file in the BOOT2 parameter of SYSTEM.CNF can be missing, 10 chars long, */
	/*      or anything else.  Its all ignored. */
	/*   3. Games loading their own files do *not* exhibit this behavior; likely due to using newer IOP modules */
	/*      or lower level filesystem APIs (fortunately that doesn't affect us). */
	/* */
	/* FIXME: Properly mimicing this behavior is troublesome since we need to add support for "ignoring" */
	/* version information when doing file searches.  I'll add this later.  For now, assuming a ;1 should */
	/* be sufficient (no known games have their ELF binary as anything but version ;1) */

	/* Fix cdrom:path, the iso reader doesn't like it: room is kept for
	 * the separator and the ";1". */
	if (!strncmp(elfpath, "cdrom:", 6) && elfpath[6] != '\\' && elfpath[6] != '/')
	{
		strlcpy(path, "cdrom:\\", sizeof(path) - 2);
		strlcat(path, elfpath + 6, sizeof(path) - 2);
	}
	else
		strlcpy(path, elfpath, sizeof(path) - 2);

	semi = strrchr(path, ';');
	if (semi && strcmp(semi, ";1"))
	{
		log_cb(RETRO_LOG_INFO, "(LoadELF) Non-conforming version suffix (%s) detected and replaced.\n", elfpath);
		strcpy(semi, ";1");
	}

	/* the file holds a sector buffer: off the stack */
	if (!(file = (isofs_file_t*)malloc(sizeof(*file))))
		return false;
	ok = isofs_file_open(file, NULL, path) && elf_identify_iso(file, crc, entry);
	free(file);
	return ok;
}
