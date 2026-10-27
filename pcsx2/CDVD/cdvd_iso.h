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

/* A disc image as the drive sees it: its sector geometry detected, read a
 * sector at a time. C89. */
#ifndef PCSX2_CDVD_ISO_H
#define PCSX2_CDVD_ISO_H

#include "cdvd_reader.h"

#ifdef __cplusplus
extern "C" {
#endif

enum
{
	ISOTYPE_ILLEGAL = 0,
	ISOTYPE_CD,
	ISOTYPE_DVD,
	ISOTYPE_AUDIO,
	ISOTYPE_DVDDL
};

#define CD_FRAMESIZE_RAW 2448

typedef struct cdvd_iso
{
	cdvd_reader_t* reader;
	/* The sector cdvd_iso_begin_read fetched: lent by the reader, or
	 * copied into readbuffer. */
	const u8*      read_ptr;
	u32            current_lsn;
	u32            read_lsn;
	/* sectors in the image */
	u32            blocks;
	u32            blocksize;
	s32            offset;
	/* where a sector's bytes start within a raw 2352-byte frame */
	s32            blockofs;
	int            type;
	int            read_result;
	bool           read_inprogress;
	u8             readbuffer[CD_FRAMESIZE_RAW];
} cdvd_iso_t;

void cdvd_iso_init(cdvd_iso_t* iso);
/* Opens the image and detects its geometry. gz_index_template and
 * data_root are as for cdvd_reader_open. */
bool cdvd_iso_open(cdvd_iso_t* iso, const char* path,
		const char* gz_index_template, const char* data_root);
void cdvd_iso_close(cdvd_iso_t* iso);

/* The sector at lsn into a raw-frame buffer, its bytes at dst + blockofs.
 * Returns the bytes read, or -1. */
int cdvd_iso_read(cdvd_iso_t* iso, u8* dst, u32 lsn);

/* The sector at lsn, its blocksize bytes where they lie, or NULL: read it
 * instead. Valid until the next call on iso. */
const u8* cdvd_iso_lend(cdvd_iso_t* iso, u32 lsn);

/* The drive's read: begin fetches the sector, finish hands it over in the
 * mode asked for. A read past the end finishes as 0 and writes nothing -
 * several games read past the end and must not see an error. */
void cdvd_iso_begin_read(cdvd_iso_t* iso, u32 lsn);
int cdvd_iso_finish_read(cdvd_iso_t* iso, u8* dst, u32 mode);

/* CDVDapi_Iso's image: where a gzip image's quick-access index goes,
 * as for cdvd_reader_open; set before it is opened. */
void cdvd_iso_set_gz_index(const char* index_template, const char* data_root);

#ifdef __cplusplus
}
#endif

#endif
