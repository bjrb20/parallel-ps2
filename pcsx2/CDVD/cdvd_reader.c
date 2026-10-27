/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2021 PCSX2 Dev Team
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

/* See cdvd_reader.h. C89. */
#include <stdlib.h>
#include <string.h>

#include <file/file_path.h>
#include <string/stdstring.h>

#include "cdvd_reader.h"

bool cdvd_readahead_step(cdvd_readahead_t* ra, u64 pos, u64 len)
{
	const u64  end = pos + len;
	const bool seq = pos >= ra->last && pos - ra->last <= CDVD_RA_GAP;
	ra->last = end;
	if (!seq)
		ra->next = 0;
	if (!seq || end <= ra->next)
		return false;
	ra->next = end + CDVD_RA_AHEAD / 2;
	return true;
}

#ifdef _WIN32
void cdvd_readahead_note(cdvd_readahead_t* ra, RFILE* fp, u64 pos, u64 len)
{
	/* Past the read: the read faults its own bytes in. The hint returns
	 * at once; the file's read-ahead thread does the rest. */
	if (fp && cdvd_readahead_step(ra, pos, len))
		filestream_prefetch(fp, pos + len, CDVD_RA_AHEAD);
}
#endif

void cdvd_reader_init(cdvd_reader_t* r, const cdvd_reader_ops_t* ops)
{
	memset(r, 0, sizeof(*r));
	r->ops       = ops;
	r->blocksize = 2048;
}

void cdvd_reader_drop_cache(cdvd_reader_t* r)
{
	unsigned i;
	for (i = 0; i < CDVD_CACHE_SLOTS; i++)
	{
		free(r->cache[i].buf);
		r->cache[i].buf  = NULL;
		r->cache[i].data = NULL;
		r->cache[i].size = 0;
		r->cache[i].cap  = 0;
	}
}

/* The slot holding [offset, offset + len), or NULL. */
static cdvd_cache_slot_t* cache_find(cdvd_reader_t* r, u64 offset, u32 len)
{
	unsigned i;
	for (i = 0; i < CDVD_CACHE_SLOTS; i++)
	{
		cdvd_cache_slot_t* s = &r->cache[i];
		if (s->size && offset >= s->offset && offset + len <= s->offset + s->size)
		{
			s->used = ++r->clock;
			return s;
		}
	}
	return NULL;
}

/* The chunk, produced into the least recently used slot; NULL when the
 * format cannot produce it. */
static cdvd_cache_slot_t* cache_load(cdvd_reader_t* r, const cdvd_chunk_t* chunk)
{
	cdvd_cache_slot_t* s = NULL;
	const u8*          lent;
	unsigned           i;
	int                amt;

	/* An empty slot, else the least recently used. */
	for (i = 0; i < CDVD_CACHE_SLOTS; i++)
	{
		if (!r->cache[i].size)
		{
			s = &r->cache[i];
			break;
		}
	}
	if (!s)
	{
		s = &r->cache[0];
		for (i = 1; i < CDVD_CACHE_SLOTS; i++)
			if (r->cache[i].used < s->used)
				s = &r->cache[i];
	}
	s->size = 0;

	if (r->ops->lend_chunk && (amt = r->ops->lend_chunk(r, chunk->id, &lent)) > 0)
		s->data = lent;
	else
	{
		if (s->cap < chunk->length)
		{
			u8* p = (u8*)realloc(s->buf, chunk->length);
			if (!p)
				return NULL;
			s->buf = p;
			s->cap = chunk->length;
		}
		amt = r->ops->read_chunk(r, s->buf, chunk->id);
		if (amt <= 0)
			return NULL;
		s->data = s->buf;
	}
	s->offset = chunk->offset;
	s->size   = (u32)amt;
	s->used   = ++r->clock;
	return s;
}

/* len bytes from offset, where they lie in one chunk; NULL otherwise. */
static const u8* chunked_span(cdvd_reader_t* r, u64 offset, u32 len)
{
	cdvd_cache_slot_t* s = cache_find(r, offset, len);
	if (!s)
	{
		const cdvd_chunk_t chunk = r->ops->chunk_for_offset(r, offset);
		if (chunk.id < 0 || offset + len > chunk.offset + chunk.length)
			return NULL;
		if (!(s = cache_load(r, &chunk)) || offset + len > s->offset + s->size)
			return NULL;
	}
	return s->data + (u32)(offset - s->offset);
}

/* Copies up to len bytes from offset chunk by chunk, for a span that
 * crosses chunks or runs past the end. Returns the bytes copied, or -1. */
static int chunked_copy(cdvd_reader_t* r, u8* dst, u64 offset, u32 len)
{
	u32 done = 0;
	while (done < len)
	{
		const cdvd_chunk_t chunk = r->ops->chunk_for_offset(r, offset);
		cdvd_cache_slot_t* s;
		u32                at, n;
		if (chunk.id < 0)
			break;
		if (!(s = cache_find(r, offset, 1)) && !(s = cache_load(r, &chunk)))
			return -1;
		at = (u32)(offset - s->offset);
		if (at >= s->size)
			break;
		n = s->size - at;
		if (n > len - done)
			n = len - done;
		memcpy(dst + done, s->data + at, n);
		done   += n;
		offset += n;
	}
	return (int)done;
}

const u8* cdvd_reader_lend(cdvd_reader_t* r, u32 sector)
{
	const u32 stride = r->internal_block_size ? r->internal_block_size : r->blocksize;
	const u64 offset = (u64)sector * stride + r->dataoffset;

	if (r->direct)
	{
		if (offset + r->blocksize > r->direct_size)
			return NULL;
		cdvd_readahead_note(&r->direct_ra, r->direct_file, offset, r->blocksize);
		return r->direct + offset;
	}
	return chunked_span(r, offset, r->blocksize);
}

int cdvd_reader_read(cdvd_reader_t* r, void* dst, u32 sector, u32 count)
{
	const u32 stride = r->internal_block_size ? r->internal_block_size : r->blocksize;
	u8*       out    = (u8*)dst;
	u32       i;

	for (i = 0; i < count; i++)
	{
		const u64 offset = (u64)(sector + i) * stride + r->dataoffset;
		const u8* p      = cdvd_reader_lend(r, sector + i);
		if (p)
			memcpy(out, p, r->blocksize);
		else
		{
			/* Split across chunks, or the end of the image. */
			int n;
			if (r->direct)
			{
				n = 0;
				if (offset < r->direct_size)
				{
					n = (int)(r->direct_size - offset);
					memcpy(out, r->direct + offset, (size_t)n);
				}
			}
			else if ((n = chunked_copy(r, out, offset, r->blocksize)) < 0)
				return -1;
			if ((u32)n < r->blocksize)
				return (int)(out - (u8*)dst) + n;
		}
		out += r->blocksize;
	}
	return (int)(out - (u8*)dst);
}

cdvd_reader_t* cdvd_reader_open(const char* path,
		const char* gz_index_template, const char* data_root)
{
	const char*    ext = path_get_extension(path);
	cdvd_reader_t* r;

	if (string_is_equal_noncase(ext, "chd"))
		r = cdvd_chd_new();
	else if (string_is_equal_noncase(ext, "cso") || string_is_equal_noncase(ext, "zso"))
		r = cdvd_cso_new();
	else if (string_is_equal_noncase(ext, "gz"))
		r = cdvd_gz_new(gz_index_template, data_root);
	else
		r = cdvd_flat_new();
	if (!r)
		return NULL;
	if (!r->ops->open(r, path))
	{
		cdvd_reader_free(r);
		return NULL;
	}
	return r;
}

void cdvd_reader_free(cdvd_reader_t* r)
{
	if (!r)
		return;
	r->ops->close(r);
	cdvd_reader_drop_cache(r);
	free(r);
}
