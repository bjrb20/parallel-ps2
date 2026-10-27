/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2020 PCSX2 Dev Team
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

/* CHD images, on libretro-common's rchd decoder. C89.
 *
 * rchd does no I/O of its own: every byte it needs is asked of this
 * reader, which hands it over from the mapped image in place, or reads it
 * into a feed buffer when the image is not mapped. A hunk is a chunk.
 * Parent images difference-chain the same way. */
#include <stdlib.h>
#include <string.h>

#include <compat/strl.h>
#include <file/file_path.h>
#include <formats/rchd.h>
#include <retro_miscellaneous.h>
#include <retro_dirent.h>
#include <string/stdstring.h>

#include "cdvd_reader.h"

/* Feed size for unmapped images. Requests are re-issued on short supply,
 * so a fixed chunk does. */
#define CHD_FEED_CHUNK (64u * 1024u)
/* Fetches gathered per decode step. */
#define CHD_PIPELINE_DEPTH 4
/* index 0 the image, 1.. its parents outward */
#define CHD_MAX_CHAIN 8

typedef struct chd_source
{
	RFILE*           fp;
	const u8*        base; /* non-NULL when mapped */
	int64_t          len;
	cdvd_readahead_t ra;
} chd_source_t;

typedef struct cdvd_chd
{
	cdvd_reader_t base;
	rchd_t*       chds[CHD_MAX_CHAIN];
	chd_source_t  srcs[CHD_MAX_CHAIN];
	u8*           feed; /* CHD_FEED_CHUNK, made when first needed */
	u64           file_size;
	u32           hunk_size;
	u32           count;
} cdvd_chd_t;

static bool chd_feed_ensure(cdvd_chd_t* c)
{
	return c->feed || (c->feed = (u8*)malloc(CHD_FEED_CHUNK)) != NULL;
}

/* Satisfies every fetch the read can use, then decodes, until the read is
 * complete. Mapped sources hand their bytes over in place; unmapped ones
 * are read into the feed buffer, and a short feed is legal - rchd asks
 * again for the rest. */
static bool chd_drive_read(cdvd_chd_t* c)
{
	rchd_request_t reqs[CHD_PIPELINE_DEPTH];
	rchd_request_t req;
	rchd_t*        chd = c->chds[0];

	for (;;)
	{
		const uint32_t n = rchd_read_pending(chd, reqs, CHD_PIPELINE_DEPTH);
		uint32_t       i;
		int            err;
		for (i = 0; i < n; i++)
		{
			chd_source_t* src;
			if (reqs[i].source == RCHD_SOURCE_PARENT)
			{
				if (c->count < 2)
					return false;
				src = &c->srcs[1];
			}
			else
				src = &c->srcs[0];
			if (src->base)
			{
				u64    avail;
				size_t len;
				if (reqs[i].offset >= (u64)src->len)
					return false;
				avail = (u64)src->len - reqs[i].offset;
				len   = (size_t)(reqs[i].length < avail ? reqs[i].length : avail);
				cdvd_readahead_note(&src->ra, src->fp, reqs[i].offset, len);
				if (rchd_feed_at(chd, reqs[i].offset, reqs[i].source, src->base + reqs[i].offset, len) < 0)
					return false;
			}
			else
			{
				size_t  want;
				int64_t got;
				if (!src->fp || !chd_feed_ensure(c))
					return false;
				if (filestream_seek(src->fp, (int64_t)reqs[i].offset, RETRO_VFS_SEEK_POSITION_START) != 0)
					return false;
				want = (size_t)(reqs[i].length < CHD_FEED_CHUNK ? reqs[i].length : CHD_FEED_CHUNK);
				if ((got = filestream_read(src->fp, c->feed, (int64_t)want)) <= 0)
					return false;
				if (rchd_feed_at(chd, reqs[i].offset, reqs[i].source, c->feed, (size_t)got) < 0)
					return false;
			}
		}

		err = rchd_read_step(chd, &req);
		if (err == RCHD_OK)
			return true;
		if (err != RCHD_PENDING)
		{
			log_cb(RETRO_LOG_ERROR, "CDVD: rchd read error %d\n", err);
			return false;
		}
	}
}

/* Opens one image: its header, map and metadata, pulled through the same
 * loop a read uses, from the file itself. */
static rchd_t* chd_open_one(cdvd_chd_t* c, const char* path, chd_source_t* src)
{
	rchd_request_t req;
	rchd_t*        chd;

	memset(src, 0, sizeof(*src));
	if (!(src->fp = filestream_open(path, RETRO_VFS_FILE_ACCESS_READ,
			RETRO_VFS_FILE_ACCESS_HINT_FREQUENT_ACCESS)))
		return NULL;
	src->base = filestream_get_mapped_ptr(src->fp, &src->len);
	if (src->base && src->len <= 0)
		src->base = NULL;
	if (!(chd = rchd_new()))
		goto fail;

	for (;;)
	{
		const int err = rchd_open_step(chd, &req);
		if (err == RCHD_OK)
			return chd;
		if (err != RCHD_PENDING)
			break;
		if (src->base)
		{
			u64 avail;
			if (req.offset >= (u64)src->len)
				break;
			avail = (u64)src->len - req.offset;
			rchd_feed(chd, src->base + req.offset,
					(size_t)(req.length < avail ? req.length : avail));
		}
		else
		{
			size_t  want;
			int64_t got;
			if (filestream_seek(src->fp, (int64_t)req.offset, RETRO_VFS_SEEK_POSITION_START) != 0
					|| !chd_feed_ensure(c))
				break;
			want = (size_t)(req.length < CHD_FEED_CHUNK ? req.length : CHD_FEED_CHUNK);
			if ((got = filestream_read(src->fp, c->feed, (int64_t)want)) <= 0)
				break;
			rchd_feed(chd, c->feed, (size_t)got);
		}
	}
	rchd_free(chd);
fail:
	filestream_close(src->fp);
	memset(src, 0, sizeof(*src));
	return NULL;
}

/* The parent of the last image in the chain: a .chd in the same directory
 * whose SHA-1 the child names. rchd checks the match, so a wrong parent
 * is refused here rather than decoding to plausible garbage. */
static bool chd_find_parent(cdvd_chd_t* c)
{
	char*        dir;
	char*        path;
	struct RDIR* rdir;
	bool         found = false;

	if (!(dir = (char*)malloc(2 * PATH_MAX_LENGTH)))
		return false;
	path = dir + PATH_MAX_LENGTH;
	fill_pathname_basedir(dir, c->base.filename, PATH_MAX_LENGTH);
	if (!(rdir = retro_opendir_include_hidden(dir, true)))
	{
		free(dir);
		return false;
	}
	while (!found && retro_readdir(rdir))
	{
		const char*  name = retro_dirent_get_name(rdir);
		chd_source_t src;
		rchd_t*      cand;
		if (!name || retro_dirent_is_dir(rdir, NULL)
				|| !string_is_equal_noncase(path_get_extension(name), "chd"))
			continue;
		fill_pathname_join_special(path, dir, name, PATH_MAX_LENGTH);
		if (!(cand = chd_open_one(c, path, &src)))
			continue;
		if (rchd_parent_sha1_matches(c->chds[c->count - 1], rchd_info(cand)->sha1))
		{
			rchd_set_parent(c->chds[c->count - 1], cand);
			c->chds[c->count] = cand;
			c->srcs[c->count] = src;
			c->count++;
			found = true;
		}
		else
		{
			rchd_free(cand);
			filestream_close(src.fp);
		}
	}
	retro_closedir(rdir);
	free(dir);
	return found;
}

static void chd_close(cdvd_reader_t* r)
{
	cdvd_chd_t* c = (cdvd_chd_t*)r;
	u32         i;
	/* Children reference parents; free outward-in. */
	for (i = 0; i < c->count; i++)
		rchd_free(c->chds[i]);
	for (i = 0; i < c->count; i++)
		if (c->srcs[i].fp)
			filestream_close(c->srcs[i].fp);
	memset(c->chds, 0, sizeof(c->chds));
	memset(c->srcs, 0, sizeof(c->srcs));
	c->count = 0;
	free(c->feed);
	c->feed      = NULL;
	c->file_size = 0;
	c->hunk_size = 0;
}

static bool chd_open(cdvd_reader_t* r, const char* filename)
{
	cdvd_chd_t*        c = (cdvd_chd_t*)r;
	const rchd_info_t* info;
	u64                frames;

	chd_close(r);
	strlcpy(r->filename, filename, sizeof(r->filename));
	if (!(c->chds[0] = chd_open_one(c, filename, &c->srcs[0])))
	{
		log_cb(RETRO_LOG_ERROR, "CDVD: failed to open CHD: %s\n", filename);
		return false;
	}
	c->count = 1;

	while (rchd_info(c->chds[c->count - 1])->has_parent)
	{
		if (c->count >= CHD_MAX_CHAIN)
		{
			log_cb(RETRO_LOG_ERROR, "CDVD: CHD parent chain hit recursion limit\n");
			chd_close(r);
			return false;
		}
		if (!chd_find_parent(c))
		{
			log_cb(RETRO_LOG_ERROR, "CDVD: no parent found for: %s\n", filename);
			chd_close(r);
			return false;
		}
	}

	info         = rchd_info(c->chds[0]);
	c->hunk_size = info->hunk_bytes;
	/* Units are whole 2448-byte frames on a CD; a sector is the first
	 * blocksize bytes of its unit. */
	r->internal_block_size = info->unit_bytes;
	/* rchd reconstructs the track layout itself - pregaps and padding
	 * resolved, a DVD one run - so the frame count is track-accurate. */
	frames = rchd_total_frames(c->chds[0]);
	c->file_size = frames > 0 ? frames * (u64)info->unit_bytes : info->logical_bytes;
	return true;
}

static cdvd_chunk_t chd_chunk_for_offset(cdvd_reader_t* r, u64 offset)
{
	const cdvd_chd_t* c = (const cdvd_chd_t*)r;
	cdvd_chunk_t      chunk;
	chunk.id     = -1;
	chunk.offset = 0;
	chunk.length = 0;
	if (offset < c->file_size)
	{
		chunk.id     = (s64)(offset / c->hunk_size);
		chunk.length = c->hunk_size;
		chunk.offset = (u64)chunk.id * c->hunk_size;
	}
	return chunk;
}

static int chd_read_chunk(cdvd_reader_t* r, void* dst, s64 id)
{
	cdvd_chd_t* c = (cdvd_chd_t*)r;
	if (id < 0 || !c->count)
		return -1;
	if (rchd_read_hunk_begin(c->chds[0], (uint32_t)id, dst) != RCHD_OK
			|| !chd_drive_read(c))
		return 0;
	return (int)c->hunk_size;
}

static u32 chd_block_count(const cdvd_reader_t* r)
{
	const cdvd_chd_t* c = (const cdvd_chd_t*)r;
	return (u32)((c->file_size - r->dataoffset) / r->internal_block_size);
}

static const cdvd_reader_ops_t chd_ops =
{
	chd_chunk_for_offset,
	chd_read_chunk,
	NULL,
	chd_open,
	chd_close,
	chd_block_count
};

cdvd_reader_t* cdvd_chd_new(void)
{
	cdvd_chd_t* c = (cdvd_chd_t*)calloc(1, sizeof(*c));
	if (!c)
		return NULL;
	cdvd_reader_init(&c->base, &chd_ops);
	return &c->base;
}
