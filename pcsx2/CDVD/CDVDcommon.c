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

/* The CDVD source in use, and what disc it holds. C89. */
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include <compat/strl.h>
#include "common/Pcsx2Defs.h"

#include "IsoFS/iso9660.h"
#include "cdvd_iso.h"
#include "CDVD.h"

CDVD_API* CDVD = NULL;

/* ---------------------------------------------------------------------------- */
/* diskTypeCached */
/* Internal disc type cache, to reduce the overhead of disc type checks, which are */
/* performed quite liberally by many games (perhaps intended to keep the PS2 DVD */
/* from spinning down due to idle activity?). */
/* Cache is set to -1 for init and when the disc is removed/changed, which invokes */
/* a new DiskTypeCheck.  All subsequent checks use the non-negative value here. */
/* */
static int diskTypeCached = -1;

/* ------------------------------------------------------ */
/* */
/* CDVD null interface for Run BIOS menu */
static s32 CALLBACK NODISCopen(const char* pTitle) { return 0; }
static void CALLBACK NODISCclose(void) { }
static s32 CALLBACK NODISCreadTrack(u32 lsn, int mode) { return -1; }
static s32 CALLBACK NODISCgetBuffer(u8* buffer) { return -1; }
static s32 CALLBACK NODISCreadSubQ(u32 lsn, cdvdSubQ* subq) { return -1; }
static s32 CALLBACK NODISCgetTN(cdvdTN* Buffer) { return -1; }
static s32 CALLBACK NODISCgetTD(u8 Track, cdvdTD* Buffer) { return -1; }
static s32 CALLBACK NODISCgetTOC(void* toc) { return -1; }
static s32 CALLBACK NODISCgetDiskType(void) { return CDVD_TYPE_NODISC; }
static s32 CALLBACK NODISCgetTrayStatus(void) { return CDVD_TRAY_CLOSE; }
static s32 CALLBACK NODISCdummyS32(void) { return 0; }
static void CALLBACK NODISCnewDiskCB(void (*callback)(void)) { }
static s32 CALLBACK NODISCreadSector(u8* tempbuffer, u32 lsn, int mode) { return -1; }
static s32 CALLBACK NODISCgetDualInfo(s32* dualType, u32* _layer1start) { return -1; }

static CDVD_API CDVDapi_NoDisc =
{
	NODISCclose,
	NODISCopen,
	NODISCreadTrack,
	NODISCgetBuffer,
	NODISCreadSubQ,
	NODISCgetTN,
	NODISCgetTD,
	NODISCgetTOC,
	NODISCgetDiskType,
	NODISCgetTrayStatus,
	NODISCdummyS32,
	NODISCdummyS32,

	NODISCnewDiskCB,

	NODISCreadSector,
	NODISCgetDualInfo,
};

/* //////////////////////////////////////////////////////////////////////////////////////// */
/* Disk Type detection stuff (from cdvdGigaherz) */
/* */
/* Generous: real ones are well under a kilobyte. */
#define MAX_SYSTEM_CNF_SIZE (1024 * 1024)

/* The type of a disc by what its filesystem holds. */
static int CheckDiskTypeFS(int baseType)
{
	isofs_dir_t   rootdir;
	isofs_entry_t entry;
	int           type = CDVD_TYPE_ILLEGAL; /* << Only for discs which aren't ps2 at all. */

	isofs_dir_init(&rootdir);
	if (!isofs_dir_open_root(&rootdir))
		return type;

	if (isofs_find(&rootdir, "SYSTEM.CNF;1", &entry) && isofs_entry_is_file(&entry))
	{
		/* The length is a u32 read straight out of the directory
		 * record, so it is whatever the image says it is - up to
		 * 4 GiB. A real SYSTEM.CNF is a few hundred bytes: cap what a
		 * disc can make us allocate, and terminate at the number of
		 * bytes actually read rather than the number claimed. */
		const u32 claimed = entry.size;
		if (claimed != 0 && claimed <= MAX_SYSTEM_CNF_SIZE)
		{
			/* the file's sector buffer and the text, together */
			isofs_file_t* file = (isofs_file_t*)malloc(sizeof(*file) + claimed + 1);
			if (file)
			{
				char* buffer = (char*)(file + 1);
				s32   got;
				isofs_file_init(file, &entry);
				got = isofs_file_read(file, buffer, (s32)claimed);
				buffer[(got > 0) ? got : 0] = '\0';

				if (strstr(buffer, "BOOT2"))
					type = (baseType == CDVD_TYPE_DETCTCD) ? CDVD_TYPE_PS2CD : CDVD_TYPE_PS2DVD;
				else if (strstr(buffer, "BOOT"))
					type = CDVD_TYPE_PSCD;
				free(file);
			}
		}
	}
	/* PS2 Linux disc 2, doesn't have a System.CNF or a normal ELF */
	else if (isofs_find(&rootdir, "P2L_0100.02;1", &entry))
		type = CDVD_TYPE_PS2DVD;
	else if (isofs_find(&rootdir, "PSX.EXE;1", &entry))
		type = CDVD_TYPE_PSCD;
	else if (isofs_find(&rootdir, "VIDEO_TS/VIDEO_TS.IFO;1", &entry))
		type = CDVD_TYPE_DVDV;

	isofs_dir_free(&rootdir);
	return type;
}

static int FindDiskType(int mType)
{
	int dataTracks = 0;
	int audioTracks = 0;
	int iCDType = mType;
	int i;
	cdvdTN tn;

	CDVD->getTN(&tn);

	if (tn.strack != tn.etrack) /* multitrack == CD. */
		iCDType = CDVD_TYPE_DETCTCD;
	else if (mType < 0)
	{
		static u8 bleh[CD_FRAMESIZE_RAW];
		cdvdTD td;

		CDVD->getTD(0, &td);
		if (td.lsn > 452849)
			iCDType = CDVD_TYPE_DETCTDVDS;
		else
		{
			if (DoCDVDreadSector(bleh, 16, CDVD_MODE_2048) == 0)
			{
				/* Horrible hack! in CD images position 166 and 171 have block size but not DVD's */
				/* It's not always 2048 however (can be 4096) */
				/* Test Impossible Mission if thia is changed. */
				u16 bs166_, bs171_;
				memcpy(&bs166_, bleh + 166, sizeof(u16));
				memcpy(&bs171_, bleh + 171, sizeof(u16));
				if (bs166_ == bs171_)
					iCDType = CDVD_TYPE_DETCTCD;
				else
					iCDType = CDVD_TYPE_DETCTDVDS;
			}
		}
	}

	if (iCDType == CDVD_TYPE_DETCTDVDS)
	{
		s32 dlt = 0;
		u32 l1s = 0;

		if (CDVD->getDualInfo(&dlt, &l1s) == 0)
		{
			if (dlt > 0)
				iCDType = CDVD_TYPE_DETCTDVDD;
		}
	}

	switch (iCDType)
	{
		case CDVD_TYPE_DETCTCD:
			log_cb(RETRO_LOG_INFO, " * CDVD Disk Open: CD, %d tracks (%d to %d):\n", tn.etrack - tn.strack + 1, tn.strack, tn.etrack);
			break;

		case CDVD_TYPE_DETCTDVDS:
			log_cb(RETRO_LOG_INFO, " * CDVD Disk Open: DVD, Single layer or unknown:\n");
			break;

		case CDVD_TYPE_DETCTDVDD:
			log_cb(RETRO_LOG_INFO, " * CDVD Disk Open: DVD, Double layer:\n");
			break;
	}

	audioTracks = dataTracks = 0;
	for (i = tn.strack; i <= tn.etrack; i++)
	{
		cdvdTD td, td2;
		int    tlength;

		CDVD->getTD(i, &td);

		if (tn.etrack > i)
			CDVD->getTD(i + 1, &td2);
		else
			CDVD->getTD(0, &td2);

		tlength = td2.lsn - td.lsn;

		if (td.type == CDVD_AUDIO_TRACK)
		{
			audioTracks++;
			log_cb(RETRO_LOG_INFO, " * * Track %d: Audio (%d sectors)\n", i, tlength);
		}
		else
		{
			dataTracks++;
			log_cb(RETRO_LOG_INFO, " * * Track %d: Data (Mode %d) (%d sectors)\n", i, ((td.type == CDVD_MODE1_TRACK) ? 1 : 2), tlength);
		}
	}

	if (dataTracks > 0)
		iCDType = CheckDiskTypeFS(iCDType);

	if (audioTracks > 0)
	{
		switch (iCDType)
		{
			case CDVD_TYPE_PS2CD:
				iCDType = CDVD_TYPE_PS2CDDA;
				break;
			case CDVD_TYPE_PSCD:
				iCDType = CDVD_TYPE_PSCDDA;
				break;
			default:
				iCDType = CDVD_TYPE_CDDA;
				break;
		}
	}

	return iCDType;
}

static void DetectDiskType(void)
{
	if (CDVD->getTrayStatus() == CDVD_TRAY_OPEN)
	{
		diskTypeCached = CDVD_TYPE_NODISC;
		return;
	}

	if (CDVD->getDiskType() == CDVD_TYPE_NODISC)
		diskTypeCached = CDVD_TYPE_NODISC;
	else
		diskTypeCached = FindDiskType(-1);
}

/* Three source slots (Iso, Disc, NoDisc), each holding a path. Fixed
 * buffers: the paths are bounded, and every consumer wanted a const char*
 * anyway. */
static char m_SourceFilename[3][PCSX2_PATH_MAX];
static CDVD_SourceType m_CurrentSourceType = CDVD_SOURCE_NODISC;

void CDVDsys_SetFile(CDVD_SourceType srctype, const char* newfile)
{
	char* slot = m_SourceFilename[srctype];

	if (!newfile)
	{
		slot[0] = '\0';
		return;
	}
	strlcpy(slot, newfile, PCSX2_PATH_MAX);
}

const char* CDVDsys_GetFile(CDVD_SourceType srctype)
{
	return m_SourceFilename[srctype];
}

CDVD_SourceType CDVDsys_GetSourceType(void)
{
	return m_CurrentSourceType;
}

void CDVDsys_ClearFiles(void)
{
	u32 i;
	for (i = 0; i < C89_ARRAY_SIZE(m_SourceFilename); i++)
		m_SourceFilename[i][0] = '\0';
}

void CDVDsys_ChangeSource(CDVD_SourceType type)
{
	if (CDVD)
		DoCDVDclose();

	switch (m_CurrentSourceType = type)
	{
		case CDVD_SOURCE_ISO:
			CDVD = &CDVDapi_Iso;
			break;
		case CDVD_SOURCE_NODISC:
			CDVD = &CDVDapi_NoDisc;
			break;
		case CDVD_SOURCE_DISC:
			/* TODO/FIXME - removed physical CD/DVD code */
		default:
			break;
	}
}

bool DoCDVDopen(void)
{
	const char* file = m_SourceFilename[m_CurrentSourceType];

	/* The gzip index's settings are the caller's to set first:
	 * cdvd_iso_set_gz_index. */
	CDVD->newDiskCB(cdvdNewDiskCB);
	if (CDVD->open(file[0] ? file : NULL) == -1)
		return false; /* error! (handled by caller) */

	DoCDVDdetectDiskType();
	return true;
}

void DoCDVDclose(void)
{
	if (CDVD->close)
		CDVD->close();

	DoCDVDresetDiskTypeCache();
}

s32 DoCDVDreadSector(u8* buffer, u32 lsn, int mode)
{
	return CDVD->readSector(buffer, lsn, mode);
}

s32 DoCDVDreadTrack(u32 lsn, int mode)
{
	return CDVD->readTrack(lsn, mode);
}

s32 DoCDVDgetBuffer(u8* buffer)
{
	return CDVD->getBuffer(buffer);
}

s32 DoCDVDdetectDiskType(void)
{
	if (diskTypeCached < 0)
		DetectDiskType();
	return diskTypeCached;
}

void DoCDVDresetDiskTypeCache(void)
{
	diskTypeCached = -1;
}

/* ------------------------------------------------------------------------
 *  SYSTEM.CNF
 * ------------------------------------------------------------------------ */

/* return value: */
/*   0 - Invalid or unknown disc. */
/*   1 - PS1 CD */
/*   2 - PS2 CD */
/* Disc region parsed from SYSTEM.CNF's VMODE line: -1 unknown (no disc
 * recognized), 0 NTSC, 1 PAL.  Consumed by the counters to seed timing
 * before SetGsCrt runs; on fast boot the BIOS video-mode init is
 * skipped, and a PAL game that derives cycle counts from the framerate
 * before setting its own mode must not compute them against NTSC
 * defaults. */
static int s_disc_region = -1;

int cdvdGetDiscRegion(void)
{
	return s_disc_region;
}

/* s without the whitespace around it, in place. */
static char* cdvdStrip(char* s)
{
	char* end;
	while (*s && isspace((unsigned char)*s))
		s++;
	end = s + strlen(s);
	while (end > s && isspace((unsigned char)end[-1]))
		*--end = '\0';
	return s;
}

int GetPS2ElfName(char* name, size_t size)
{
	int           retype = 0;
	isofs_file_t* file;
	char          line[512];

	if (size)
		name[0] = '\0';
	s_disc_region = -1;

	/* the file holds a sector buffer: off the stack */
	if (!(file = (isofs_file_t*)malloc(sizeof(*file))))
		return 0;
	if (!isofs_file_open(file, NULL, "SYSTEM.CNF;1") || file->length == 0)
	{
		free(file);
		return 0;
	}

	while (!isofs_file_eof(file))
	{
		char* key;
		char* value;
		char* eq;

		isofs_file_read_line(file, line, sizeof(line));
		if (!(eq = strchr(line, '=')))
			continue;
		*eq   = '\0';
		key   = cdvdStrip(line);
		value = cdvdStrip(eq + 1);

		if (!*value && !isofs_file_eof(file))
		{ /* Some games have a character on the last line of the file, don't print the error in those cases. */
			log_cb(RETRO_LOG_WARN, "(SYSTEM.CNF) Unusual or malformed entry in SYSTEM.CNF ignored:\n" );
			log_cb(RETRO_LOG_INFO, "%s =\n", key);
			continue;
		}

		if (!strcmp(key, "BOOT2"))
		{
			log_cb(RETRO_LOG_INFO, "(SYSTEM.CNF) Detected PS2 Disc = %s\n", value);
			strlcpy(name, value, size);
			retype = 2;
		}
		else if (!strcmp(key, "BOOT"))
		{
			log_cb(RETRO_LOG_INFO, "(SYSTEM.CNF) Detected PSX/PSone Disc = %s\n", value);
			strlcpy(name, value, size);
			retype = 1;
		}
		else if (!strcmp(key, "VMODE"))
		{
			log_cb(RETRO_LOG_INFO, "(SYSTEM.CNF) Disc region type = %s\n", value);
			s_disc_region = !strcmp(value, "PAL") ? 1 : 0;
		}
		else if (!strcmp(key, "VER"))
			log_cb(RETRO_LOG_INFO, "(SYSTEM.CNF) Software version = %s\n", value);
	}
	free(file);

	if( retype == 0 )
	{
		s_disc_region = -1;
		return 0;
	}

	/* Recognized disc without a VMODE line: NTSC, matching what the
	 * upstream plumbing does with an absent value. */
	if (s_disc_region < 0)
		s_disc_region = 0;

	return retype;
}
