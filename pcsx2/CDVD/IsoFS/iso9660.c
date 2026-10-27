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

/* See iso9660.h. C89. */
#include <stdlib.h>
#include <string.h>

#include "iso9660.h"

/* ------------------------------------------------------------------------
 *  Records
 * ------------------------------------------------------------------------ */

/* A directory record of length bytes. 33 bytes is its fixed part, the
 * name follows; the name has to fit in the record it came from. */
static void entry_load(isofs_entry_t* e, const u8* data, int length)
{
	int len;

	memset(e, 0, sizeof(*e));
	if (length < 33)
		return;

	/* the fields are byte-packed little-endian */
	e->lba    = (u32)data[2] | ((u32)data[3] << 8) | ((u32)data[4] << 16) | ((u32)data[5] << 24);
	e->size   = (u32)data[10] | ((u32)data[11] << 8) | ((u32)data[12] << 16) | ((u32)data[13] << 24);
	e->year       = data[18] + 1900;
	e->month      = data[19];
	e->day        = data[20];
	e->hour       = data[21];
	e->minute     = data[22];
	e->second     = data[23];
	e->gmt_offset = data[24];
	e->flags      = data[25];

	len = data[32];
	if (len > length - 33)
		len = length - 33;
	if (len <= 0)
		return;
	if (len == 1 && data[33] <= 1)
		strcpy(e->name, data[33] ? ".." : ".");
	else
	{
		memcpy(e->name, data + 33, (size_t)len);
		e->name[len] = '\0';
	}
}

/* ------------------------------------------------------------------------
 *  Files
 * ------------------------------------------------------------------------ */

/* One sector of the image, or zeroes where the image has none: a record
 * read from a sector that is not there is a zero-length record, which
 * ends a directory, never whatever the buffer held. */
static void load_sector(u8* buffer, u32 lba)
{
	if (lba > 0x7FFFFFFFu || !isofs_read_sector(buffer, (int)lba))
		memset(buffer, 0, ISOFS_SECTOR);
}

void isofs_file_init(isofs_file_t* f, const isofs_entry_t* entry)
{
	f->entry      = *entry;
	f->offset     = 0;
	f->length     = entry->size;
	f->sector_lba = (u32)-1; /* nothing buffered */
	f->sector_pos = 0;
}

u32 isofs_file_seek(isofs_file_t* f, u32 offset)
{
	f->offset     = offset;
	f->sector_pos = offset % ISOFS_SECTOR;
	return offset;
}

/* The sector the offset lies in, in the buffer. */
static void make_available(isofs_file_t* f)
{
	const u32 lba = f->entry.lba + f->offset / ISOFS_SECTOR;
	if (f->sector_lba != lba)
	{
		load_sector(f->sector, lba);
		f->sector_lba = lba;
	}
	f->sector_pos = f->offset % ISOFS_SECTOR;
}

u8 isofs_file_read_byte(isofs_file_t* f)
{
	u8 c;
	if (f->offset >= f->length)
		return 0;
	make_available(f);
	c = f->sector[f->sector_pos];
	f->offset++;
	return c;
}

s32 isofs_file_read(isofs_file_t* f, void* dest, s32 len)
{
	u8* out  = (u8*)dest;
	s32 done = 0;

	if (len <= 0)
		return 0;
	if ((u32)len > f->length - f->offset || f->offset > f->length)
		len = f->offset > f->length ? 0 : (s32)(f->length - f->offset);

	while (done < len)
	{
		const u32 pos = f->offset % ISOFS_SECTOR;
		u32       n   = ISOFS_SECTOR - pos;
		if (n > (u32)(len - done))
			n = (u32)(len - done);
		if (pos == 0 && n == ISOFS_SECTOR)
			/* a whole sector goes straight where it is wanted */
			load_sector(out + done, f->entry.lba + f->offset / ISOFS_SECTOR);
		else
		{
			make_available(f);
			memcpy(out + done, f->sector + pos, n);
		}
		done      += (s32)n;
		f->offset += n;
	}
	f->sector_pos = f->offset % ISOFS_SECTOR;
	return done;
}

size_t isofs_file_read_line(isofs_file_t* f, char* buf, size_t size)
{
	size_t n = 0;
	while (!isofs_file_eof(f))
	{
		const u8 c = isofs_file_read_byte(f);
		if (c == '\n' || c == '\r' || c == 0)
			break;
		if (n + 1 < size)
			buf[n++] = (char)c;
	}
	if (size)
		buf[n] = '\0';
	return n;
}

bool isofs_file_open(isofs_file_t* f, const isofs_dir_t* dir, const char* path)
{
	isofs_entry_t entry;
	bool          found;

	if (dir)
		found = isofs_find(dir, path, &entry);
	else
	{
		isofs_dir_t root;
		isofs_dir_init(&root);
		found = isofs_dir_open_root(&root) && isofs_find(&root, path, &entry);
		isofs_dir_free(&root);
	}
	/* a directory is no file to read */
	if (!found || !isofs_entry_is_file(&entry))
	{
		log_cb(RETRO_LOG_ERROR, "Failed to find file %s\n", path);
		return false;
	}
	isofs_file_init(f, &entry);
	return true;
}

/* ------------------------------------------------------------------------
 *  Directories
 * ------------------------------------------------------------------------ */

void isofs_dir_init(isofs_dir_t* dir)
{
	dir->files = NULL;
	dir->count = 0;
	dir->cap   = 0;
}

void isofs_dir_free(isofs_dir_t* dir)
{
	free(dir->files);
	isofs_dir_init(dir);
}

bool isofs_dir_open(isofs_dir_t* dir, const isofs_entry_t* entry)
{
	/* The stream is the size of a sector buffer; its records are read
	 * one by one into b. */
	isofs_file_t* stream = (isofs_file_t*)malloc(sizeof(*stream));
	u8            b[257];
	u32           remaining = entry->size;

	if (!stream)
		return false;
	isofs_file_init(stream, entry);
	dir->count = 0;

	while (remaining >= 4)
	{
		b[0] = isofs_file_read_byte(stream);
		if (b[0] == 0)
			break;
		/* A record under 34 bytes cannot hold the fixed part and a
		 * one-byte name, and one longer than what is left of the
		 * directory is not one of its records. */
		if (b[0] < 34 || b[0] > remaining)
			break;
		remaining -= b[0];
		isofs_file_read(stream, b + 1, b[0] - 1);

		if (dir->count == dir->cap)
		{
			const u32      cap   = dir->cap ? dir->cap * 2 : 16;
			isofs_entry_t* files = (isofs_entry_t*)realloc(dir->files, cap * sizeof(*files));
			if (!files)
				break;
			dir->files = files;
			dir->cap   = cap;
		}
		entry_load(&dir->files[dir->count++], b, b[0]);
	}
	free(stream);
	return true;
}

bool isofs_dir_open_root(isofs_dir_t* dir)
{
	isofs_entry_t root;
	u8            sector[ISOFS_SECTOR];
	bool          valid = false;
	u32           i;

	memset(&root, 0, sizeof(root));
	for (i = 16;; i++)
	{
		/* Not an image, or a bad one. */
		if (!isofs_read_sector(sector, (int)i))
			break;
		if (memcmp(&sector[1], "CD001", 5) != 0)
		{
			sector[9] = 0;
			log_cb(RETRO_LOG_ERROR, "(IsoFS) Invalid partition descriptor encountered at block 0x%x: '%s'\n", i, &sector[1]);
			break;
		}
		if (sector[0] == 0xff)
			break; /* the volume descriptor set terminator */
		if (sector[0] == 1)
		{
			entry_load(&root, sector + 156, 38);
			valid = true;
		}
		/* 0: boot record; 2: supplementary (Joliet), not used */
		else if (sector[0] != 0 && sector[0] != 2)
			log_cb(RETRO_LOG_ERROR, "(IsoFS) Unknown partition type ID=%d, encountered at block 0x%x\n", sector[0], i);
	}

	if (!valid)
	{
		log_cb(RETRO_LOG_ERROR, "IsoFS could not find the root directory on the ISO image.\n");
		return false;
	}
	return isofs_dir_open(dir, &root);
}

/* The index of the entry named name (len bytes), or -1. */
static int dir_index_of(const isofs_dir_t* dir, const char* name, size_t len)
{
	u32 i;
	for (i = 0; i < dir->count; i++)
		if (strlen(dir->files[i].name) == len && !memcmp(dir->files[i].name, name, len))
			return (int)i;
	return -1;
}

#define ISOFS_IS_SEP(c) ((c) == '/' || (c) == '\\')

bool isofs_find(const isofs_dir_t* dir, const char* path, isofs_entry_t* out)
{
	const isofs_dir_t* at = dir;
	isofs_dir_t        sub;
	const char*        part;
	const char*        p;
	bool               found = false;
	bool               first = true;

	if (!path || !*path)
		return false;
	isofs_dir_init(&sub);

	/* A UNC prefix stays with the first part. */
	p    = path;
	part = path;
	if (ISOFS_IS_SEP(p[0]) && ISOFS_IS_SEP(p[1]) && p[2])
		p += 2;
	/* Leading separators are no part. */
	while (ISOFS_IS_SEP(*part) && part == p)
		part = ++p;

	for (;;)
	{
		size_t len;
		int    index;

		while (*p && !ISOFS_IS_SEP(*p))
			p++;
		len = (size_t)(p - part);
		while (ISOFS_IS_SEP(*p))
			p++;

		/* The device (cdrom0:) is not a directory. */
		if (first && len && part[len - 1] == ':')
		{
			first = false;
			if (!*p)
				break;
			part = p;
			continue;
		}
		first = false;
		if (!len)
			break;
		if ((index = dir_index_of(at, part, len)) < 0)
			break;

		if (!*p)
		{
			*out  = at->files[index];
			found = true;
			break;
		}
		/* The entry is copied out before the directory it lies in is
		 * replaced by the next level. */
		{
			const isofs_entry_t next = at->files[index];
			if (isofs_entry_is_file(&next) || !isofs_dir_open(&sub, &next))
				break;
		}
		at   = &sub;
		part = p;
	}
	isofs_dir_free(&sub);
	return found;
}
