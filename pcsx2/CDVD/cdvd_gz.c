/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2014  PCSX2 Dev Team
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

/* Gzipped images, read at random through a quick-access index. C89.
 *
 * The index (zlib_indexed.h) holds an access point every span bytes of
 * the uncompressed image; a chunk is decompressed from the access point
 * before it, and a chunk that follows on from the last goes on from the
 * live stream. Built on first open and kept beside the image, or where
 * the index template puts it. */
#include <stdlib.h>
#include <string.h>

#include <compat/strl.h>
#include <file/file_path.h>

#include "cdvd_reader.h"
#include "zlib_indexed.h"

#define GZIP_ID "PCSX2.index.gzip.v1|"
#define GZIP_ID_LEN (sizeof(GZIP_ID) - 1) /* sizeof includes the \0 terminator */
/* The largest span an index may hold: 16 times the one the reader writes. */
#define GZIP_SPAN_MAX (64 * 1024 * 1024)
/* Distance between access points when building an index. */
#define GZFILE_SPAN_DEFAULT (4 * 1024 * 1024)
/* What one chunk decompresses: a read needs only its own chunk, not the
 * whole span the access point covers. The cache's slots then hold 16 MiB
 * of the image. */
#define GZFILE_READ_CHUNK_SIZE (2 * 1024 * 1024)
#define INDEX_TEMPLATE_KEY "$(f)"

typedef struct cdvd_gz
{
	cdvd_reader_t base;
	Access*       index;
	RFILE*        src;
	const char*   index_template;
	const char*   data_root;
	zstate        z;
} cdvd_gz_t;

/* File format:
 *  - GZIP_ID, without the \0
 *  - the Access, its list pointer zero
 *  - the access points */
static Access* read_index(const char* filename)
{
	char    id[GZIP_ID_LEN];
	Access* index;
	char*   list;
	s64     size, datasize;
	RFILE*  fp = filestream_open(filename, RETRO_VFS_FILE_ACCESS_READ,
			RETRO_VFS_FILE_ACCESS_HINT_NONE);
	if (!fp)
		return NULL;

	/* Everything below is derived from the file's own size, so the file
	 * has to be big enough to hold what it describes. */
	if ((size = filestream_get_size(fp)) < (s64)(GZIP_ID_LEN + sizeof(Access))
			|| filestream_read(fp, id, GZIP_ID_LEN) != (int64_t)GZIP_ID_LEN
			|| memcmp(id, GZIP_ID, 4) != 0
			|| !(index = (Access*)malloc(sizeof(Access))))
	{
		filestream_close(fp);
		return NULL;
	}

	/* The span divides every offset into an access point and the size
	 * bounds every read: an index holding values no reader writes is not
	 * used, and a new one is built from the gzip. */
	datasize = size - (s64)GZIP_ID_LEN - (s64)sizeof(Access);
	list     = NULL;
	if (filestream_read(fp, index, sizeof(Access)) != (int64_t)sizeof(Access)
			|| index->span <= 0 || index->span > GZIP_SPAN_MAX
			|| index->uncompressed_size <= 0
			|| index->have <= 0
			|| datasize != (s64)index->have * (s64)sizeof(Point)
			|| !(list = (char*)malloc((size_t)datasize))
			|| filestream_read(fp, list, datasize) != (int64_t)datasize)
	{
		free(list);
		filestream_close(fp);
		free(index);
		return NULL;
	}
	index->list = (Point*)list;
	filestream_close(fp);
	return index;
}

static void write_index(Access* index, const char* filename)
{
	Point* list = index->list;
	bool   ok;
	RFILE* fp = filestream_open(filename, RETRO_VFS_FILE_ACCESS_WRITE,
			RETRO_VFS_FILE_ACCESS_HINT_NONE);
	if (!fp)
		return;
	ok = filestream_write(fp, GZIP_ID, GZIP_ID_LEN) == (int64_t)GZIP_ID_LEN;
	index->list = NULL; /* the pointer is meaningless on disk */
	ok = ok && filestream_write(fp, index, sizeof(Access)) == (int64_t)sizeof(Access);
	index->list = list;
	ok = ok && filestream_write(fp, list, (int64_t)sizeof(Point) * index->have)
			== (int64_t)sizeof(Point) * index->have;
	filestream_close(fp);
	(void)ok;
}

/* The index file's name: the template with its one $(f) replaced by the
 * image's name - or its leaf name when the key is not at the start, as a
 * template that carries its own directory wants - joined under data_root
 * when relative. Empty when the template is malformed: no key, more than
 * one, or ending with it. */
static void index_name(char* out, size_t out_size, const char* fileTemplate,
		const char* filename, const char* base)
{
	const size_t keylen = strlen(INDEX_TEMPLATE_KEY);
	/* The trimmed template and the substituted path, off the stack. */
	char*  tmpl = (char*)malloc(2 * PCSX2_PATH_MAX);
	char*  joined;
	char*  key;
	char*  start;
	char*  end;
	size_t len;

	out[0] = '\0';
	if (!tmpl)
		return;
	joined = tmpl + PCSX2_PATH_MAX;

	strlcpy(tmpl, fileTemplate ? fileTemplate : "", PCSX2_PATH_MAX);
	start = tmpl;
	while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n')
		start++;
	end = start + strlen(start);
	while (end > start && (end[-1] == ' ' || end[-1] == '\t'
				|| end[-1] == '\r' || end[-1] == '\n'))
		end--;
	*end = '\0';
	len = (size_t)(end - start);

	key = strstr(start, INDEX_TEMPLATE_KEY);
	if (key && !strstr(key + 1, INDEX_TEMPLATE_KEY)
			&& (size_t)(key - start) != len - keylen)
	{
		const char* fname = filename;
		if (key != start)
		{
			const char* slash = strrchr(filename, '/');
#ifdef _WIN32
			const char* bslash = strrchr(filename, '\\');
			if (bslash > slash)
				slash = bslash;
#endif
			if (slash)
				fname = slash + 1;
		}
		*key = '\0';
		strlcpy(joined, start, PCSX2_PATH_MAX);
		strlcat(joined, fname, PCSX2_PATH_MAX);
		strlcat(joined, key + keylen, PCSX2_PATH_MAX);
		if (path_is_absolute(joined))
			strlcpy(out, joined, out_size);
		else
			fill_pathname_join_special(out, base ? base : "", joined, out_size);
	}
	free(tmpl);
}

static bool load_or_create_index(cdvd_gz_t* g)
{
	char*   indexfile = (char*)malloc(PCSX2_PATH_MAX);
	Access* built     = NULL;
	s64     prev;
	int     points;

	if (!indexfile)
		return false;
	index_name(indexfile, PCSX2_PATH_MAX, g->index_template, g->base.filename, g->data_root);
	if (indexfile[0] == '\0' || (g->index = read_index(indexfile)) != NULL)
	{
		const bool found = indexfile[0] != '\0';
		free(indexfile);
		return found;
	}

	log_cb(RETRO_LOG_WARN, "This may take a while (but only once). Scanning compressed file to generate a quick access index...\n");
	prev   = filestream_tell(g->src);
	points = build_index(g->src, GZFILE_SPAN_DEFAULT, &built);
	filestream_seek(g->src, prev, RETRO_VFS_SEEK_POSITION_START);

	/* build_index returns the number of access points, and zero is not
	 * success: it leaves built alone then. */
	if (points <= 0 || !built)
	{
		free_index(built);
		free(indexfile);
		return false;
	}
	g->index = built;
	write_index(g->index, indexfile);
	free(indexfile);
	return true;
}

static void gz_close(cdvd_reader_t* r)
{
	cdvd_gz_t* g = (cdvd_gz_t*)r;
	zstate_free(&g->z);
	memset(&g->z, 0, sizeof(g->z));
	if (g->src)
		filestream_close(g->src);
	g->src = NULL;
	if (g->index)
		free_index(g->index);
	g->index = NULL;
}

static bool gz_open(cdvd_reader_t* r, const char* filename)
{
	cdvd_gz_t* g = (cdvd_gz_t*)r;
	gz_close(r);
	strlcpy(r->filename, filename, sizeof(r->filename));
	if (!(g->src = filestream_open(filename, RETRO_VFS_FILE_ACCESS_READ,
			RETRO_VFS_FILE_ACCESS_HINT_NONE)) || !load_or_create_index(g))
	{
		gz_close(r);
		return false;
	}
	return true;
}

static cdvd_chunk_t gz_chunk_for_offset(cdvd_reader_t* r, u64 offset)
{
	const cdvd_gz_t* g = (const cdvd_gz_t*)r;
	cdvd_chunk_t     chunk;
	chunk.id     = -1;
	chunk.offset = 0;
	chunk.length = 0;
	if ((s64)offset < g->index->uncompressed_size)
	{
		const s64 left = g->index->uncompressed_size
				- (s64)(offset / GZFILE_READ_CHUNK_SIZE * GZFILE_READ_CHUNK_SIZE);
		chunk.id     = (s64)(offset / GZFILE_READ_CHUNK_SIZE);
		chunk.offset = (u64)chunk.id * GZFILE_READ_CHUNK_SIZE;
		chunk.length = (u32)(left < GZFILE_READ_CHUNK_SIZE ? left : GZFILE_READ_CHUNK_SIZE);
	}
	return chunk;
}

static int gz_read_chunk(cdvd_reader_t* r, void* dst, s64 id)
{
	cdvd_gz_t* g = (cdvd_gz_t*)r;
	s64        at, left;
	if (id < 0 || (at = id * GZFILE_READ_CHUNK_SIZE) >= g->index->uncompressed_size)
		return -1;
	left = g->index->uncompressed_size - at;
	return extract(g->src, g->index, at, (unsigned char*)dst,
			(int)(left < GZFILE_READ_CHUNK_SIZE ? left : GZFILE_READ_CHUNK_SIZE), &g->z);
}

static u32 gz_block_count(const cdvd_reader_t* r)
{
	const cdvd_gz_t* g = (const cdvd_gz_t*)r;
	return (u32)((g->index->uncompressed_size + (r->blocksize - 1)) / r->blocksize);
}

static const cdvd_reader_ops_t gz_ops =
{
	gz_chunk_for_offset,
	gz_read_chunk,
	NULL,
	gz_open,
	gz_close,
	gz_block_count
};

cdvd_reader_t* cdvd_gz_new(const char* index_template, const char* data_root)
{
	cdvd_gz_t* g = (cdvd_gz_t*)calloc(1, sizeof(*g));
	if (!g)
		return NULL;
	cdvd_reader_init(&g->base, &gz_ops);
	g->index_template = index_template;
	g->data_root      = data_root;
	return &g->base;
}
