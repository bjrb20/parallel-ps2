/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2014 PCSX2 Dev Team
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

/* CSO and ZSO images. C89.
 * https://github.com/unknownbrackets/maxcso/blob/master/README_CSO.md
 *
 * A frame is a chunk. With the image mapped, a stored frame is lent where
 * it lies and a compressed one is decoded straight from the mapping;
 * without, frames are read into a staging buffer first. */
#include <stdlib.h>
#include <string.h>

#include <compat/strl.h>
#include <encodings/deflate.h>
#include <encodings/rlz4.h>

#include "cdvd_reader.h"

#define CSO_READ_BUFFER_SIZE (256u * 1024u)
#define CSO_HEADER_SIZE      24

typedef struct cdvd_cso
{
	cdvd_reader_t    base;
	RFILE*           src;
	const u8*        map_base; /* non-NULL when mapped */
	u8*              read_buffer;
	u32*             index;
	void*            inflate;  /* raw deflate, reset per frame */
	int64_t          map_len;
	u64              total_size;
	cdvd_readahead_t map_ra;
	u32              read_buffer_size;
	u32              frame_size;
	u8               frame_shift;
	u8               index_shift;
	bool             lz4;      /* ZSO */
} cdvd_cso_t;

static u32 get32(const u8* p)
{
	return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static u32 cso_frames(const cdvd_cso_t* c)
{
	return (u32)((c->total_size + c->frame_size - 1) >> c->frame_shift);
}

/* magic, header size, total bytes, frame size, version, index shift */
static bool cso_read_header(cdvd_cso_t* c)
{
	u8  h[CSO_HEADER_SIZE];
	u64 total;
	u32 frame;
	u8  ver, align;

	if (filestream_seek(c->src, c->base.dataoffset, RETRO_VFS_SEEK_POSITION_START) != 0
			|| filestream_read(c->src, h, sizeof(h)) != (int64_t)sizeof(h))
	{
		log_cb(RETRO_LOG_ERROR, "Failed to read CSO file header.\n");
		return false;
	}
	total = (u64)get32(h + 8) | ((u64)get32(h + 12) << 32);
	frame = get32(h + 16);
	ver   = h[20];
	align = h[21];

	/* Invalid magic, definitely a bad file. */
	if ((h[0] != 'C' && h[0] != 'Z') || h[1] != 'I' || h[2] != 'S' || h[3] != 'O')
		goto bad;
	if (ver > 1)
	{
		log_cb(RETRO_LOG_ERROR, "Only CSOv1 files are supported.\n");
		goto bad;
	}
	if ((frame & (frame - 1)) != 0)
	{
		log_cb(RETRO_LOG_ERROR, "CSO frame size must be a power of two.\n");
		goto bad;
	}
	if (frame < 2048)
	{
		log_cb(RETRO_LOG_ERROR, "CSO frame size must be at least one sector.\n");
		goto bad;
	}
	/* These decide how much the header can make us allocate. Real CSO
	 * frames run 2 KiB to 64 KiB; past 16 the alignment shift describes a
	 * file no tool produces, and at 32 it is undefined; a double-layer
	 * DVD is 8.5 GB. */
	if (frame > 16u * 1024u * 1024u)
	{
		log_cb(RETRO_LOG_ERROR, "CSO frame size of %u is implausible.\n", frame);
		goto bad;
	}
	if (align > 16)
	{
		log_cb(RETRO_LOG_ERROR, "CSO index alignment shift of %u is implausible.\n", align);
		goto bad;
	}
	if (total == 0 || total > (u64)64 * 1024 * 1024 * 1024)
	{
		log_cb(RETRO_LOG_ERROR, "CSO total size is zero or implausible.\n");
		goto bad;
	}

	c->frame_size  = frame;
	c->frame_shift = 0;
	while ((1u << c->frame_shift) < frame)
		c->frame_shift++;
	c->index_shift = align;
	c->total_size  = total;
	c->lz4         = h[0] == 'Z';
	return true;

bad:
	log_cb(RETRO_LOG_ERROR, "CSO has invalid header.\n");
	return false;
}

static bool cso_init_buffers(cdvd_cso_t* c)
{
	const u64 frames64 = (c->total_size + c->frame_size - 1) / c->frame_size;
	u32       entries;

	/* Bounded by the header checks, but the cast is only safe because of
	 * them - keep the two together. */
	if (frames64 >= 0xFFFFFFFFu)
	{
		log_cb(RETRO_LOG_ERROR, "CSO frame count is implausible.\n");
		return false;
	}
	entries = (u32)frames64 + 1;

	/* A frame and its alignment, read in one piece when unmapped. */
	c->read_buffer_size = c->frame_size + (1u << c->index_shift);
	if (c->read_buffer_size < CSO_READ_BUFFER_SIZE)
		c->read_buffer_size = CSO_READ_BUFFER_SIZE;
	if (!c->map_base && !(c->read_buffer = (u8*)malloc(c->read_buffer_size)))
		return false;

	if (!(c->index = (u32*)malloc(sizeof(u32) * entries)))
		return false;
	if (filestream_read(c->src, c->index, (int64_t)(sizeof(u32) * entries))
			!= (int64_t)(sizeof(u32) * entries))
	{
		log_cb(RETRO_LOG_ERROR, "Unable to read index data from CSO.\n");
		return false;
	}

	if (!c->lz4 && !(c->inflate = rinflate_new(-15)))
	{
		log_cb(RETRO_LOG_ERROR, "Unable to initialize inflate for CSO decompression.\n");
		return false;
	}
	return true;
}

static void cso_close(cdvd_reader_t* r)
{
	cdvd_cso_t* c = (cdvd_cso_t*)r;
	r->filename[0] = '\0';
	if (c->src)
		filestream_close(c->src);
	c->src      = NULL;
	c->map_base = NULL;
	c->map_len  = 0;
	c->map_ra.next = 0;
	c->map_ra.last = 0;
	if (c->inflate)
		rinflate_free(c->inflate);
	c->inflate = NULL;
	free(c->read_buffer);
	c->read_buffer = NULL;
	free(c->index);
	c->index = NULL;
}

static bool cso_open(cdvd_reader_t* r, const char* filename)
{
	cdvd_cso_t* c = (cdvd_cso_t*)r;
	cso_close(r);
	strlcpy(r->filename, filename, sizeof(r->filename));
	c->src = filestream_open(filename, RETRO_VFS_FILE_ACCESS_READ,
			RETRO_VFS_FILE_ACCESS_HINT_FREQUENT_ACCESS);
	if (c->src)
		c->map_base = filestream_get_mapped_ptr(c->src, &c->map_len);
	if (!c->src || !cso_read_header(c) || !cso_init_buffers(c))
	{
		cso_close(r);
		return false;
	}
	return true;
}

static cdvd_chunk_t cso_chunk_for_offset(cdvd_reader_t* r, u64 offset)
{
	const cdvd_cso_t* c = (const cdvd_cso_t*)r;
	cdvd_chunk_t      chunk;
	chunk.id     = -1;
	chunk.offset = 0;
	chunk.length = 0;
	if (offset < c->total_size)
	{
		chunk.id     = (s64)(offset >> c->frame_shift);
		chunk.length = c->frame_size;
		chunk.offset = (u64)chunk.id << c->frame_shift;
	}
	return chunk;
}

/* The frame's index entries, or false for a frame past the end or one
 * whose index runs backwards (a damaged file, not a 2 GB frame). */
static bool cso_frame(const cdvd_cso_t* c, s64 id, u64* pos, u64* size, bool* compressed)
{
	u32 i0, i1;
	if (id < 0 || (u64)id >= cso_frames(c))
		return false;
	i0 = c->index[id];
	i1 = c->index[id + 1] & 0x7FFFFFFFu;
	*compressed = (i0 & 0x80000000u) == 0;
	i0 &= 0x7FFFFFFFu;
	if (i1 < i0)
	{
		log_cb(RETRO_LOG_ERROR, "CSO index for frame %u runs backwards.\n", (unsigned)id);
		return false;
	}
	*pos  = (u64)i0 << c->index_shift;
	*size = (u64)(i1 - i0) << c->index_shift;
	return true;
}

/* A stored frame in the mapping is lent where it lies. */
static int cso_lend_chunk(cdvd_reader_t* r, s64 id, const u8** ptr)
{
	cdvd_cso_t* c = (cdvd_cso_t*)r;
	u64         pos, size;
	bool        compressed;
	if (!c->map_base || !cso_frame(c, id, &pos, &size, &compressed) || compressed
			|| pos + c->frame_size > (u64)c->map_len)
		return 0;
	cdvd_readahead_note(&c->map_ra, c->src, pos, c->frame_size);
	*ptr = c->map_base + pos;
	return (int)c->frame_size;
}

static bool cso_decode(cdvd_cso_t* c, u8* dst, const u8* src, u32 len)
{
	if (c->lz4)
		return rlz4_decode(dst, c->frame_size, src, len) > 0;
	{
		size_t rd = 0, wr = 0;
		int    st;
		rinflate_set_in(c->inflate, src, len);
		rinflate_set_out(c->inflate, dst, c->frame_size);
		st = rinflate_process(c->inflate, &rd, &wr);
		rinflate_reset(c->inflate, -15);
		return st == RDEFLATE_PROCESS_END && wr == c->frame_size;
	}
}

static int cso_read_chunk(cdvd_reader_t* r, void* dst, s64 id)
{
	cdvd_cso_t* c = (cdvd_cso_t*)r;
	u64         pos, size;
	bool        compressed;
	u32         raw;

	if (!cso_frame(c, id, &pos, &size, &compressed))
		return 0;

	if (c->map_base && pos < (u64)c->map_len)
	{
		/* The frame's bytes are already addressable: the stored case is
		 * one copy, the compressed one is decoded in place. The last
		 * frame's stored payload may be shorter than its index span. */
		const u64 avail = (u64)c->map_len - pos;
		raw = (u32)(compressed ? (size < avail ? size : avail) : c->frame_size);
		cdvd_readahead_note(&c->map_ra, c->src, pos, raw);
		if (!compressed)
		{
			if (avail < c->frame_size)
				return 0;
			memcpy(dst, c->map_base + pos, c->frame_size);
			return (int)c->frame_size;
		}
		if (!cso_decode(c, (u8*)dst, c->map_base + pos, raw))
		{
			log_cb(RETRO_LOG_ERROR, "Unable to decompress CSO frame from mapped source.\n");
			return 0;
		}
		return (int)c->frame_size;
	}

	if (filestream_seek(c->src, (int64_t)pos, RETRO_VFS_SEEK_POSITION_START) != 0)
	{
		log_cb(RETRO_LOG_ERROR, "Unable to seek to CSO data.\n");
		return 0;
	}
	if (!compressed)
		return (int)filestream_read(c->src, dst, c->frame_size);
	/* A frame no larger than the read buffer, which holds a whole frame
	 * and its alignment; one that claims more is cut to it and fails to
	 * decompress. */
	if (!c->read_buffer)
		return 0;
	if (size > c->read_buffer_size)
		size = c->read_buffer_size;
	raw = (u32)filestream_read(c->src, c->read_buffer, (int64_t)size);
	if (!cso_decode(c, (u8*)dst, c->read_buffer, raw))
	{
		log_cb(RETRO_LOG_ERROR, "Unable to decompress CSO frame.\n");
		return 0;
	}
	return (int)c->frame_size;
}

static u32 cso_block_count(const cdvd_reader_t* r)
{
	const cdvd_cso_t* c = (const cdvd_cso_t*)r;
	return (u32)((c->total_size - r->dataoffset) / r->blocksize);
}

static const cdvd_reader_ops_t cso_ops =
{
	cso_chunk_for_offset,
	cso_read_chunk,
	cso_lend_chunk,
	cso_open,
	cso_close,
	cso_block_count
};

cdvd_reader_t* cdvd_cso_new(void)
{
	cdvd_cso_t* c = (cdvd_cso_t*)calloc(1, sizeof(*c));
	if (!c)
		return NULL;
	cdvd_reader_init(&c->base, &cso_ops);
	return &c->base;
}
