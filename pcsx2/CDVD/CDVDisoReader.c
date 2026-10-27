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


/*
 *  Original code from libcdvd by Hiryu & Sjeep (C) 2002
 *  Modified by Florin for PCSX2 emu
 *  Fixed CdRead by linuzappz
 */

/* C89. */
#include <string.h>

#include "cdvd_iso.h"
#include "CDVD.h"

static cdvd_iso_t iso;
/* Where a gzip image's quick-access index goes; see cdvd_reader_open. */
static const char* s_gz_index_template;
static const char* s_gz_data_root;

void cdvd_iso_set_gz_index(const char* index_template, const char* data_root)
{
	s_gz_index_template = index_template;
	s_gz_data_root      = data_root;
}

static int pmode, cdtype;

static s32 layer1start = -1;
static bool layer1searched = false;

static void CALLBACK ISOclose(void)
{
	cdvd_iso_close(&iso);
}

static s32 CALLBACK ISOopen(const char* pTitle)
{
	ISOclose(); /* just in case */

	if ((pTitle == NULL) || (pTitle[0] == 0))
	{
		log_cb(RETRO_LOG_ERROR, "CDVDiso Error: No filename specified.\n");
		return -1;
	}

	if (!cdvd_iso_open(&iso, pTitle, s_gz_index_template, s_gz_data_root))
		return -1;

	switch (iso.type)
	{
		case ISOTYPE_DVD:
			cdtype = CDVD_TYPE_PS2DVD;
			break;
		case ISOTYPE_AUDIO:
			cdtype = CDVD_TYPE_CDDA;
			break;
		default:
			cdtype = CDVD_TYPE_PS2CD;
			break;
	}

	layer1start = -1;
	layer1searched = false;

	return 0;
}

static s32 CALLBACK ISOreadSubQ(u32 lsn, cdvdSubQ* subq)
{
	/* fake it */
	u8 min, sec, frm;
	subq->ctrl = 4;
	subq->mode = 1;
	subq->trackNum = itob(1);
	subq->trackIndex = itob(1);

	lba_to_msf(lsn, &min, &sec, &frm);
	subq->trackM = itob(min);
	subq->trackS = itob(sec);
	subq->trackF = itob(frm);

	subq->pad = 0;

	lba_to_msf(lsn + (2 * 75), &min, &sec, &frm);
	subq->discM = itob(min);
	subq->discS = itob(sec);
	subq->discF = itob(frm);

	return 0;
}

static s32 CALLBACK ISOgetTN(cdvdTN* Buffer)
{
	Buffer->strack = 1;
	Buffer->etrack = 1;

	return 0;
}

static s32 CALLBACK ISOgetTD(u8 Track, cdvdTD* Buffer)
{
	if (Track == 0)
		Buffer->lsn  = iso.blocks;
	else
	{
		Buffer->lsn  = 0;
		Buffer->type = CDVD_MODE1_TRACK;
	}

	return 0;
}

static bool testForPrimaryVolumeDescriptor(const u8 *buffer)
{
	static const u8 identifier[6] = {1, 'C', 'D', '0', '0', '1'};
	return memcmp(identifier, buffer + iso.blockofs, sizeof(identifier)) == 0;
}

static void FindLayer1Start(void)
{
	u8 buffer[CD_FRAMESIZE_RAW];
	int offset;
	u32 blockresult;

	if (layer1searched)
		return;

	layer1searched = true;

	/* The ISO9660 primary volume descriptor for layer 0 is located at sector 16 */
	cdvd_iso_read(&iso, buffer, 16);
	if (!testForPrimaryVolumeDescriptor(buffer))
	{
		log_cb(RETRO_LOG_ERROR, "isoFile: Invalid layer0 Primary Volume Descriptor\n");
		return;
	}

	/* The volume space size (sector count) is located at bytes 80-87 - 80-83 */
	/* is the little endian size, 84-87 is the big endian size. */
	offset      = iso.blockofs;
	blockresult = buffer[offset + 80] + (buffer[offset + 81] << 8) + (buffer[offset + 82] << 16) + ((u32)buffer[offset + 83] << 24);

	/* If the ISO sector count is larger than the volume size, then we should */
	/* have a dual layer DVD. Layer 1 is on a different volume. */
	if (blockresult < iso.blocks)
	{
		/* The layer 1 start LSN contains the primary volume descriptor for layer 1. */
		/* The check might be a bit unnecessary though. */
		if (cdvd_iso_read(&iso, buffer, blockresult) == -1)
			return;

		if (!testForPrimaryVolumeDescriptor(buffer))
		{
			log_cb(RETRO_LOG_ERROR, "isoFile: Invalid layer1 Primary Volume Descriptor\n");
			return;
		}
		layer1start = blockresult;
		log_cb(RETRO_LOG_INFO, "isoFile: second layer found at sector 0x%08x\n", layer1start);
	}
}

/* Should return 0 if no error occurred, or -1 if layer detection FAILED. */
static s32 CALLBACK ISOgetDualInfo(s32* dualType, u32* _layer1start)
{
	FindLayer1Start();

	if (layer1start < 0)
	{
		*dualType      = 0;
		*_layer1start  = iso.blocks;
	}
	else
	{
		*dualType      = 1;
		*_layer1start  = layer1start;
	}
	return 0;
}

static s32 CALLBACK ISOgetDiskType(void) { return cdtype; }

static s32 CALLBACK ISOgetTOC(void* toc)
{
	u8 type     = ISOgetDiskType();
	u8* tocBuff = (u8*)toc;

	if (type == CDVD_TYPE_DVDV || type == CDVD_TYPE_PS2DVD)
	{
		/* get dvd structure format */
		/* scsi command 0x43 */
		memset(tocBuff, 0, 2048);

		FindLayer1Start();

		if (layer1start < 0)
		{
			/* Single Layer - Values are fixed. */
			tocBuff[0]  = 0x04;
			tocBuff[1]  = 0x02;
			tocBuff[2]  = 0xF2;
			tocBuff[3]  = 0x00;
			tocBuff[4]  = 0x86;
			tocBuff[5]  = 0x72;

			/* These values are fixed on all discs, except position 14 which is the OTP/PTP flags which are 0 in single layer. */
			tocBuff[12] = 0x01;
			tocBuff[13] = 0x02;
			tocBuff[14] = 0x01;
			tocBuff[15] = 0x00;

			/* Values are fixed. */
			tocBuff[16] = 0x00;
			tocBuff[17] = 0x03;
			tocBuff[18] = 0x00;
			tocBuff[19] = 0x00;

			{
			cdvdTD trackInfo;
			s32 maxlsn;
			/* Get the max LSN for the track */
			if (ISOgetTD(0, &trackInfo) == -1)
				trackInfo.lsn = 0;

			/* Max LSN in the TOC is calculated as the blocks + 0x30000, then - 1. */
			/* same as layer 1 start. */
			maxlsn = trackInfo.lsn + (0x30000 - 1);
			tocBuff[20] = maxlsn >> 24;
			tocBuff[21] = (maxlsn >> 16) & 0xff;
			tocBuff[22] = (maxlsn >> 8) & 0xff;
			tocBuff[23] = (maxlsn >> 0) & 0xff;
			}
			return 0;
		}
		else
		{
			/* Dual sided - Values are fixed. */
			tocBuff[0] = 0x24;
			tocBuff[1] = 0x02;
			tocBuff[2] = 0xF2;
			tocBuff[3] = 0x00;
			tocBuff[4] = 0x41;
			tocBuff[5] = 0x95;

			/* These values are fixed on all discs, except position 14 which is the OTP/PTP flags. */
			tocBuff[12] = 0x01;
			tocBuff[13] = 0x02;
			tocBuff[14] = 0x21; /* PTP */
			tocBuff[15] = 0x10;

			/* Values are fixed. */
			tocBuff[16] = 0x00;
			tocBuff[17] = 0x03;
			tocBuff[18] = 0x00;
			tocBuff[19] = 0x00;

			tocBuff[20] = ((layer1start + 0x30000 - 1) >> 24);
			tocBuff[21] = ((layer1start + 0x30000 - 1) >> 16) & 0xff;
			tocBuff[22] = ((layer1start + 0x30000 - 1) >> 8) & 0xff;
			tocBuff[23] = ((layer1start + 0x30000 - 1) >> 0) & 0xff;
		}
	}
	else if ((type == CDVD_TYPE_CDDA) || (type == CDVD_TYPE_PS2CDDA) ||
			 (type == CDVD_TYPE_PS2CD) || (type == CDVD_TYPE_PSCDDA) || (type == CDVD_TYPE_PSCD))
	{
		/* cd toc */
		/* (could be replaced by 1 command that reads the full toc) */
		u8 min, sec, frm;
		s32 i, err;
		cdvdTN diskInfo;
		cdvdTD trackInfo;
		memset(tocBuff, 0, 1024);
		if (ISOgetTN(&diskInfo) == -1)
		{
			diskInfo.etrack = 0;
			diskInfo.strack = 1;
		}
		if (ISOgetTD(0, &trackInfo) == -1)
			trackInfo.lsn = 0;

		tocBuff[0] = 0x41;
		tocBuff[1] = 0x00;

		/* Number of FirstTrack */
		tocBuff[2] = 0xA0;
		tocBuff[7] = itob(diskInfo.strack);

		/* Number of LastTrack */
		tocBuff[12] = 0xA1;
		tocBuff[17] = itob(diskInfo.etrack);

		/* DiskLength */
		lba_to_msf(trackInfo.lsn, &min, &sec, &frm);
		tocBuff[22] = 0xA2;
		tocBuff[27] = itob(min);
		tocBuff[28] = itob(sec);
		tocBuff[29] = itob(frm);

		/* TODO: When cue support is added, this will need to account for pregap. */
		for (i = diskInfo.strack; i <= diskInfo.etrack; i++)
		{
			err = ISOgetTD(i, &trackInfo);
			lba_to_msf(trackInfo.lsn, &min, &sec, &frm);
			tocBuff[i * 10 + 30] = trackInfo.type;
			tocBuff[i * 10 + 32] = err == -1 ? 0 : itob(i); /* number */
			tocBuff[i * 10 + 37] = itob(min);
			tocBuff[i * 10 + 38] = itob(sec);
			tocBuff[i * 10 + 39] = itob(frm);
		}
	}
	else
		return -1;

	return 0;
}

static s32 CALLBACK ISOreadSector(u8* tempbuffer, u32 lsn, int mode)
{
	/* The raw frame the part asked for is cut from; what the image does
	 * not hold keeps whatever an earlier read left there. */
	static u8 cdbuffer[CD_FRAMESIZE_RAW] = {0};
	int _lsn = lsn;
	int pos  = 0;
	int psize = 0;

	if (_lsn < 0)
		lsn = iso.blocks + _lsn;
	if (lsn >= iso.blocks)
		return -1;

	if (mode == CDVD_MODE_2352)
	{
		cdvd_iso_read(&iso, tempbuffer, lsn);
		return 0;
	}

	switch (mode)
	{
		case CDVD_MODE_2340:
			pos   = 12;
			psize = 2340;
			break;
		case CDVD_MODE_2328:
			pos   = 24;
			psize = 2328;
			break;
		case CDVD_MODE_2048:
			pos   = 24;
			psize = 2048;
			break;
		default:
			break;
	}

	/* A part that lies wholly in the sector's own bytes is copied from
	 * where the reader holds them. */
	if (pos >= iso.blockofs && pos - iso.blockofs + psize <= (int)iso.blocksize)
	{
		const u8* p = cdvd_iso_lend(&iso, lsn);
		if (p)
		{
			memcpy(tempbuffer, p + (pos - iso.blockofs), (size_t)psize);
			return 0;
		}
	}

	cdvd_iso_read(&iso, cdbuffer, lsn);
	memcpy(tempbuffer, cdbuffer + pos, (size_t)psize);
	return 0;
}

static s32 CALLBACK ISOreadTrack(u32 lsn, int mode)
{
	int _lsn = lsn;

	if (_lsn < 0)
		lsn = iso.blocks + _lsn;

	cdvd_iso_begin_read(&iso, lsn);

	pmode = mode;

	return 0;
}

static s32 CALLBACK ISOgetBuffer(u8* buffer)
{
	return cdvd_iso_finish_read(&iso, buffer, (u32)pmode);
}

static s32 CALLBACK ISOgetTrayStatus(void)
{
	return CDVD_TRAY_CLOSE;
}

static s32 CALLBACK ISOctrlTrayOpen(void)  { return 0; }
static s32 CALLBACK ISOctrlTrayClose(void) { return 0; }
static void CALLBACK ISOnewDiskCB(void (*callback)(void))
{
	(void)callback;
}

CDVD_API CDVDapi_Iso =
{
	ISOclose,

	ISOopen,
	ISOreadTrack,
	ISOgetBuffer,
	ISOreadSubQ,
	ISOgetTN,
	ISOgetTD,
	ISOgetTOC,
	ISOgetDiskType,
	ISOgetTrayStatus, /* trayStatus */
	ISOctrlTrayOpen, /* trayOpen */
	ISOctrlTrayClose, /* trayClose */

	ISOnewDiskCB,

	ISOreadSector,
	ISOgetDualInfo,
};
