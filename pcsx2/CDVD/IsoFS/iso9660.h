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

/* The ISO 9660 filesystem of the disc in the drive, read through
 * DoCDVDreadSector: directories, and files read like a stream. C89. */
#ifndef PCSX2_ISO9660_H
#define PCSX2_ISO9660_H

#include <stddef.h>

#include "../../../common/Pcsx2Defs.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ISOFS_SECTOR 2048

/* A directory record. */
typedef struct isofs_entry
{
	u32  lba;
	u32  size;
	s32  year;
	int  flags;      /* bit 1: a directory */
	u8   month;
	u8   day;
	u8   hour;
	u8   minute;
	u8   second;
	u8   gmt_offset; /* 15-minute steps from -48 (west) to +52 (east) */
	char name[256];  /* NUL-terminated; "." and ".." for the special ones */
} isofs_entry_t;

#define isofs_entry_is_file(e) (!((e)->flags & 2))

typedef struct isofs_dir
{
	isofs_entry_t* files;
	u32            count;
	u32            cap;
} isofs_dir_t;

/* A file, read in order or after a seek. */
typedef struct isofs_file
{
	isofs_entry_t entry;
	u32           offset;  /* in the file */
	u32           length;
	u32           sector_lba;
	u32           sector_pos;
	u8            sector[ISOFS_SECTOR];
} isofs_file_t;

/* One 2048-byte sector by LBA: DoCDVDreadSector, in IsoFSCDVD.c. */
bool isofs_read_sector(u8* buffer, int lba);

void isofs_dir_init(isofs_dir_t* dir);
void isofs_dir_free(isofs_dir_t* dir);
/* The root directory of the primary volume. */
bool isofs_dir_open_root(isofs_dir_t* dir);
bool isofs_dir_open(isofs_dir_t* dir, const isofs_entry_t* entry);
/* The entry at path - separators either way, an optional "device:"
 * first - under dir. */
bool isofs_find(const isofs_dir_t* dir, const char* path, isofs_entry_t* out);

void isofs_file_init(isofs_file_t* f, const isofs_entry_t* entry);
/* The file at path under dir, or under the root when dir is NULL; a
 * directory there is no file. */
bool isofs_file_open(isofs_file_t* f, const isofs_dir_t* dir, const char* path);
/* Seeks to offset (clamped to nothing); returns the new offset. */
u32 isofs_file_seek(isofs_file_t* f, u32 offset);
/* Up to len bytes into dest; returns how many. Whole sectors are read
 * straight into dest. */
s32 isofs_file_read(isofs_file_t* f, void* dest, s32 len);
u8 isofs_file_read_byte(isofs_file_t* f);
/* The rest of the line (to \n, \r or NUL) into buf, at most size - 1
 * bytes of it, NUL-terminated; a longer line is cut and the rest of it
 * skipped. Returns the bytes stored. */
size_t isofs_file_read_line(isofs_file_t* f, char* buf, size_t size);
#define isofs_file_eof(f) ((f)->offset >= (f)->length)

#ifdef __cplusplus
}
#endif

#endif
