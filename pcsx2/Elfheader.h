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

/* The game's ELF: its identity (a CRC of its words) and entry point, read
 * from the disc or from the host. C89. */
#pragma once

#include "MemoryTypes.h"

typedef struct ELF_HEADER {
	u8	e_ident[16];	/* 0x7f,"ELF"  (ELF file identifier) */
	u16	e_type;			/* ELF type: 0=NONE, 1=REL, 2=EXEC, 3=SHARED, 4=CORE */
	u16	e_machine;	  /* Processor: 8=MIPS R3000 */
	u32	e_version;	  /* Version: 1=current */
	u32	e_entry;		/* Entry point address */
	u32	e_phoff;		/* Start of program headers (offset from file start) */
	u32	e_shoff;		/* Start of section headers (offset from file start) */
	u32	e_flags;		/* Processor specific flags = 0x20924001 noreorder, mips */
	u16	e_ehsize;	   /* ELF header size (0x34 = 52 bytes) */
	u16	e_phentsize;	/* Program headers entry size */
	u16	e_phnum;		/* Number of program headers */
	u16	e_shentsize;	/* Section headers entry size */
	u16	e_shnum;		/* Number of section headers */
	u16	e_shstrndx;	 /* Section header stringtable index */
} ELF_HEADER;

#ifdef __cplusplus
extern "C" {
#endif

struct isofs_file;

/* MSVC decorates C++ data symbols; the ELF ABI does not, so what C reaches
 * has C linkage. */
extern u32 ElfCRC;
extern u32 ElfEntry;
/* The ELF ElfCRC and ElfEntry are of; empty when none. */
extern char LastELF[];

/* The CRC and entry point of an ELF on the disc, or at a host path. */
bool elf_identify_iso(struct isofs_file* f, u32* crc, u32* entry);
bool elf_identify_host(const char* path, u32* crc, u32* entry);
/* The same of the ELF a boot path names: "host:" and a host path, or a
 * disc path as SYSTEM.CNF gives it ("cdrom0:\\SLUS_123.45;1"), whose
 * version suffix is taken as ";1" whatever it says, as the BIOS does. */
bool elf_identify_path(const char* elfpath, u32* crc, u32* entry);

#ifdef __cplusplus
}
#endif
