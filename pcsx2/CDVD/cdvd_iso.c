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

/* See cdvd_iso.h. C89. */
#include <string.h>

#include "cdvd_iso.h"
#include "CDVD.h"

void cdvd_iso_init(cdvd_iso_t* iso)
{
	iso->reader          = NULL;
	iso->read_ptr        = NULL;
	iso->current_lsn     = (u32)-1;
	iso->read_lsn        = (u32)-1;
	iso->blocks          = 0;
	iso->blocksize       = 0;
	iso->offset          = 0;
	iso->blockofs        = 0;
	iso->type            = ISOTYPE_ILLEGAL;
	iso->read_result     = 0;
	iso->read_inprogress = false;
}

void cdvd_iso_close(cdvd_iso_t* iso)
{
	cdvd_reader_free(iso->reader);
	cdvd_iso_init(iso);
}

/* A sector lent by the reader stays valid only until the reader's next
 * call: before making one, a fetched sector still to be finished is kept
 * in readbuffer. */
static void hold_fetched(cdvd_iso_t* iso)
{
	if (iso->read_ptr && iso->read_ptr != iso->readbuffer)
	{
		memcpy(iso->readbuffer, iso->read_ptr, iso->blocksize);
		iso->read_ptr = iso->readbuffer;
	}
}

int cdvd_iso_read(cdvd_iso_t* iso, u8* dst, u32 lsn)
{
	if (lsn >= iso->blocks)
		return -1;
	hold_fetched(iso);
	return cdvd_reader_read(iso->reader, dst + iso->blockofs, lsn, 1);
}

const u8* cdvd_iso_lend(cdvd_iso_t* iso, u32 lsn)
{
	if (lsn >= iso->blocks)
		return NULL;
	hold_fetched(iso);
	return cdvd_reader_lend(iso->reader, lsn);
}

void cdvd_iso_begin_read(cdvd_iso_t* iso, u32 lsn)
{
	const u8* p;

	iso->current_lsn = lsn;
	/* Usually a damaged image, but some games do read past the end, so
	 * this is no error. */
	if (lsn >= iso->blocks)
		return;
	/* the same sector again */
	if (lsn == iso->read_lsn)
		return;
	iso->read_lsn = lsn;

	/* What was fetched before is replaced, so it need not be kept. */
	if ((p = cdvd_reader_lend(iso->reader, lsn)) != NULL)
	{
		iso->read_ptr    = p;
		iso->read_result = (int)iso->blocksize;
	}
	else
	{
		iso->read_ptr    = iso->readbuffer;
		iso->read_result = cdvd_reader_read(iso->reader, iso->readbuffer, lsn, 1);
	}
	iso->read_inprogress = true;
}

int cdvd_iso_finish_read(cdvd_iso_t* iso, u8* dst, u32 mode)
{
	int _offset, length, end, diff, ndiff;
	int end1, end2;

	/* Nothing for a read past the end: it keeps several games from
	 * hanging. */
	if (iso->current_lsn >= iso->blocks)
		return 0;

	if (iso->read_inprogress)
	{
		iso->read_inprogress = false;
		if (iso->read_result < 0)
		{
			iso->read_lsn = (u32)-1;
			return -1;
		}
	}

	switch (mode)
	{
		case CDVD_MODE_2352:
			_offset = 0;
			length  = 2352;
			break;
		case CDVD_MODE_2340:
			_offset = 12;
			length  = 2340;
			break;
		case CDVD_MODE_2328:
			_offset = 24;
			length  = 2328;
			break;
		case CDVD_MODE_2048:
			_offset = 24;
			length  = 2048;
			break;
		case CDVD_MODE_2368:
			/* The raw sector; the 16 bytes of subchannel Q after it are
			 * the drive's to add, the image has none. */
			_offset = 0;
			length  = 2352;
			break;
		default:
			return -1;
	}

	end1 = iso->blockofs + (int)iso->blocksize;
	end2 = _offset + length;
	end  = end1 < end2 ? end1 : end2;

	diff  = iso->blockofs - _offset;
	ndiff = 0;
	if (diff > 0)
	{
		memset(dst, 0, (size_t)diff);
		_offset = iso->blockofs;
	}
	else
	{
		ndiff = -diff;
		diff  = 0;
	}

	length = end - _offset;
	memcpy(dst + diff, iso->read_ptr + ndiff, (size_t)length);

	if (iso->type == ISOTYPE_CD && diff >= 12)
	{
		lsn_to_msf(dst + diff - 12, (s32)iso->current_lsn);
		dst[diff - 9] = 2;
	}
	return 0;
}

static bool try_type(cdvd_iso_t* iso, u32 size, u32 offset, u32 blockofs)
{
	u8 buf[2456];
	u16 bs;

	iso->blocksize = size;
	iso->offset    = (s32)offset;
	iso->blockofs  = (s32)blockofs;
	iso->reader->dataoffset = offset;
	iso->reader->blocksize  = size;

	if (cdvd_iso_read(iso, buf, 16) < 0)
		return false;
	/* ISO 9660 */
	if (strncmp((const char*)(buf + 25), "CD001", 5))
		return false;
	memcpy(&bs, buf + 190, sizeof(bs));
	iso->type = bs == 2048 ? ISOTYPE_CD : ISOTYPE_DVD;
	return true;
}

/* Based on florin's CDVDbin detection code. */
static bool detect(cdvd_iso_t* iso)
{
	iso->type = ISOTYPE_ILLEGAL;

	/* No sane image has fewer than the 16 sectors a TOC needs. */
	if (cdvd_reader_block_count(iso->reader) < 17)
		return false;

	iso->blocks = 17;
	if (try_type(iso, 2048, 0, 24)              /* ISO 2048 */
			|| try_type(iso, 2336, 0, 16)       /* RAW 2336 */
			|| try_type(iso, 2352, 0, 0)        /* RAW 2352 */
			|| try_type(iso, 2448, 0, 0)        /* RAWQ 2448 */
			|| try_type(iso, 2048, 150 * 2048, 24)  /* NERO ISO 2048 */
			|| try_type(iso, 2352, 150 * 2048, 0)   /* NERO RAW 2352 */
			|| try_type(iso, 2448, 150 * 2048, 0))  /* NERO RAWQ 2448 */
		return true;

	iso->offset    = 0;
	iso->blocksize = CD_FRAMESIZE_RAW;
	iso->blockofs  = 0;
	iso->type      = ISOTYPE_AUDIO;
	iso->reader->dataoffset = 0;
	iso->reader->blocksize  = CD_FRAMESIZE_RAW;
	/* This also takes a memory card file for an audio CD. */
	return true;
}

bool cdvd_iso_open(cdvd_iso_t* iso, const char* path,
		const char* gz_index_template, const char* data_root)
{
	cdvd_iso_close(iso);
	if (!(iso->reader = cdvd_reader_open(path, gz_index_template, data_root)))
		return false;
	if (!detect(iso))
	{
		cdvd_iso_close(iso);
		return false;
	}
	iso->blocks = cdvd_reader_block_count(iso->reader);
	return true;
}
