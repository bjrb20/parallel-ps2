/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2024 PCSX2 Dev Team
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

/* An uncompressed image (ISO, BIN). C89.
 *
 * FREQUENT_ACCESS invites the local VFS to memory-map it: an image is read
 * a sector at a time for the whole session, and with a mapping every
 * sector is read straight out of the page cache. Without one (a frontend
 * VFS, a 32-bit off_t) it is read in chunks through the reader's cache. */
#include <stdlib.h>

#include <compat/strl.h>

#include "cdvd_reader.h"

#define FLAT_CHUNK_SIZE (128u * 1024u)

typedef struct cdvd_flat
{
	cdvd_reader_t base;
	RFILE*        file;
	u64           file_size;
} cdvd_flat_t;

static cdvd_chunk_t flat_chunk_for_offset(cdvd_reader_t* r, u64 offset)
{
	const cdvd_flat_t* f = (const cdvd_flat_t*)r;
	cdvd_chunk_t       chunk;
	chunk.id     = -1;
	chunk.offset = 0;
	chunk.length = 0;
	if (offset < f->file_size)
	{
		const u64 left = f->file_size - offset / FLAT_CHUNK_SIZE * FLAT_CHUNK_SIZE;
		chunk.id     = (s64)(offset / FLAT_CHUNK_SIZE);
		chunk.offset = (u64)chunk.id * FLAT_CHUNK_SIZE;
		chunk.length = (u32)(left < FLAT_CHUNK_SIZE ? left : FLAT_CHUNK_SIZE);
	}
	return chunk;
}

static int flat_read_chunk(cdvd_reader_t* r, void* dst, s64 id)
{
	cdvd_flat_t* f = (cdvd_flat_t*)r;
	u64          at;
	u32          len;
	if (id < 0 || (at = (u64)id * FLAT_CHUNK_SIZE) >= f->file_size)
		return -1;
	if (filestream_seek(f->file, (int64_t)at, RETRO_VFS_SEEK_POSITION_START) != 0)
		return -1;
	len = (u32)(f->file_size - at < FLAT_CHUNK_SIZE ? f->file_size - at : FLAT_CHUNK_SIZE);
	return filestream_read(f->file, dst, len) == (int64_t)len ? (int)len : 0;
}

static void flat_close(cdvd_reader_t* r)
{
	cdvd_flat_t* f = (cdvd_flat_t*)r;
	if (f->file)
		filestream_close(f->file);
	f->file             = NULL;
	f->file_size        = 0;
	r->direct           = NULL;
	r->direct_size      = 0;
	r->direct_file      = NULL;
}

static bool flat_open(cdvd_reader_t* r, const char* filename)
{
	cdvd_flat_t*   f = (cdvd_flat_t*)r;
	const uint8_t* base;
	int64_t        size, maplen = 0;

	strlcpy(r->filename, filename, sizeof(r->filename));
	f->file = filestream_open(filename, RETRO_VFS_FILE_ACCESS_READ,
			RETRO_VFS_FILE_ACCESS_HINT_FREQUENT_ACCESS);
	if (!f->file)
		return false;
	if ((size = filestream_get_size(f->file)) <= 0)
	{
		/* A file that opens but cannot be sized is a VFS that cannot
		 * address it - a 32-bit file offset under a large image is what
		 * this looked like in the field. */
		log_cb(RETRO_LOG_ERROR, "CDVD: cannot determine size of %s (VFS reported %lld) - image unreadable\n",
				filename, (long long)size);
		flat_close(r);
		return false;
	}
	f->file_size = (u64)size;
	base = filestream_get_mapped_ptr(f->file, &maplen);
	if (base && maplen >= size)
	{
		r->direct      = base;
		r->direct_size = f->file_size;
		r->direct_file = f->file;
		r->direct_ra.next = 0;
		r->direct_ra.last = 0;
	}
	return true;
}

static u32 flat_block_count(const cdvd_reader_t* r)
{
	return (u32)(((const cdvd_flat_t*)r)->file_size / r->blocksize);
}

static const cdvd_reader_ops_t flat_ops =
{
	flat_chunk_for_offset,
	flat_read_chunk,
	NULL,
	flat_open,
	flat_close,
	flat_block_count
};

cdvd_reader_t* cdvd_flat_new(void)
{
	cdvd_flat_t* f = (cdvd_flat_t*)calloc(1, sizeof(*f));
	if (!f)
		return NULL;
	cdvd_reader_init(&f->base, &flat_ops);
	return &f->base;
}
