/* IsoFS on the real iso9660.c, against a disc built here: path lookup -
 * files in the root, one and two directories down, with and without the
 * device prefix, a path that is only separators, a directory whose
 * extent lies past the end of the image - and file reads: a file over
 * several sectors read in pieces that start mid-sector, take whole
 * sectors and end short, and SYSTEM.CNF read line by line.
 *
 * Then what reads the disc through it, on the real CDVDcommon.c and
 * Elfheader.c: the disc type, SYSTEM.CNF's boot path and region, and the
 * ELF a boot path names, on the disc and on the host. C89. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libretro.h>
#include <compat/strl.h>

#include "CDVD/IsoFS/iso9660.h"
#include "CDVD/CDVD.h"
#include "Elfheader.h"

#define SECTOR  2048
#define SECTORS 32
#define BIG_LBA  27
#define BIG_SIZE (3 * SECTOR + 500)
#define ELF_LBA  23
#define ELF_SIZE 602 /* the last two bytes are no word */
#define INNER_LBA 25
#define INNER_SIZE 300

static unsigned char disc[SECTORS][SECTOR];
static unsigned failures;

static void quiet_log(enum retro_log_level level, const char* fmt, ...)
{
	(void)level; (void)fmt;
}
retro_log_printf_t log_cb = quiet_log;

bool isofs_read_sector(u8* buffer, int lba)
{
	if (lba < 0 || lba >= SECTORS)
		return false;
	memcpy(buffer, disc[lba], SECTOR);
	return true;
}

/* 1 and the entry's place when the path is found, 0 when it is not, -1
 * when the root does not open. */
static int isofs_harness_find(const char* path, unsigned* lba, unsigned* size)
{
	isofs_dir_t   root;
	isofs_entry_t e;
	int           r = -1;
	isofs_dir_init(&root);
	if (isofs_dir_open_root(&root))
	{
		r = isofs_find(&root, path, &e) ? 1 : 0;
		if (r)
		{
			*lba  = e.lba;
			*size = e.size;
		}
	}
	isofs_dir_free(&root);
	return r;
}

static unsigned char big_byte(unsigned i)
{
	return (unsigned char)((i * 131u + (i >> 11) * 7u) ^ 0x5a);
}

static const char ps2_cnf[] = "BOOT2 = cdrom0:\\SLUS_123.45;1\r\nVER = 1.00\r\nVMODE = NTSC\r\n";
#define cnf ps2_cnf
/* the SYSTEM.CNF the disc is built with */
static const char* disc_cnf = ps2_cnf;

/* The bytes of the ELFs: a header with its entry point, then words. */
static unsigned char elf_byte(unsigned i, unsigned seed)
{
	if (i < 4)
		return (unsigned char)"\177ELF"[i];
	if (i >= 24 && i < 28) /* e_entry */
		return (unsigned char)((0x00100008u + seed) >> ((i - 24) * 8));
	return (unsigned char)(i * 7u + seed * 13u + (i >> 8));
}

static u32 elf_crc(unsigned size, unsigned seed)
{
	u32 crc = 0;
	unsigned i;
	for (i = 0; i + 4 <= size; i += 4)
		crc ^= (u32)elf_byte(i, seed) | ((u32)elf_byte(i + 1, seed) << 8)
			| ((u32)elf_byte(i + 2, seed) << 16) | ((u32)elf_byte(i + 3, seed) << 24);
	return crc;
}

static void put_both32(unsigned char* p, unsigned v)
{
	p[0] = (unsigned char)v;         p[1] = (unsigned char)(v >> 8);
	p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
	p[4] = p[3]; p[5] = p[2]; p[6] = p[1]; p[7] = p[0];
}

/* One directory record at p; returns its length. A one-byte name of 0
 * or 1 is "." or "..". */
static int record(unsigned char* p, const char* name, int name_len,
	unsigned lba, unsigned size, int is_dir)
{
	int len = 33 + name_len;
	if (len & 1)
		len++;
	memset(p, 0, (size_t)len);
	p[0] = (unsigned char)len;
	put_both32(p + 2, lba);
	put_both32(p + 10, size);
	p[25] = (unsigned char)(is_dir ? 2 : 0);
	p[32] = (unsigned char)name_len;
	memcpy(p + 33, name, (size_t)name_len);
	return len;
}

static void directory(int lba, int parent, const char** names, const unsigned* lbas,
	const unsigned* sizes, const int* dirs, int n)
{
	unsigned char* p = disc[lba];
	int i;
	p += record(p, "\0", 1, (unsigned)lba, SECTOR, 1);
	p += record(p, "\1", 1, (unsigned)parent, SECTOR, 1);
	for (i = 0; i < n; i++)
		p += record(p, names[i], (int)strlen(names[i]), lbas[i], sizes[i], dirs[i]);
}

static void build_disc(void)
{
	static const char* root_names[] = {"SUB", "FAR", "SYSTEM.CNF;1", "BIG.BIN;1", "SLUS_123.45;1"};
	static const unsigned root_lbas[] = {21, 1000, 24, BIG_LBA, ELF_LBA};
	unsigned root_sizes[] = {SECTOR, SECTOR, 0, BIG_SIZE, ELF_SIZE};
	static const int root_dirs[] = {1, 1, 0, 0, 0};
	static const char* sub_names[] = {"DEEP", "INNER.ELF;1"};
	static const unsigned sub_lbas[] = {22, INNER_LBA};
	static const unsigned sub_sizes[] = {SECTOR, INNER_SIZE};
	static const int sub_dirs[] = {1, 0};
	static const char* deep_names[] = {"BOTTOM.BIN;1"};
	static const unsigned deep_lbas[] = {26};
	static const unsigned deep_sizes[] = {400};
	static const int deep_dirs[] = {0};
	unsigned char* pvd = disc[16];

	root_sizes[2] = (unsigned)strlen(disc_cnf);
	memset(disc, 0, sizeof(disc));
	pvd[0] = 1;
	memcpy(pvd + 1, "CD001", 5);
	record(pvd + 156, "\0", 1, 20, SECTOR, 1);
	disc[17][0] = 0xff;
	memcpy(disc[17] + 1, "CD001", 5);

	directory(20, 20, root_names, root_lbas, root_sizes, root_dirs, 5);
	directory(21, 20, sub_names, sub_lbas, sub_sizes, sub_dirs, 2);
	directory(22, 21, deep_names, deep_lbas, deep_sizes, deep_dirs, 1);
	memcpy(disc[24], disc_cnf, strlen(disc_cnf));
	{
		unsigned i;
		for (i = 0; i < BIG_SIZE; i++)
			disc[BIG_LBA + i / SECTOR][i % SECTOR] = big_byte(i);
		for (i = 0; i < ELF_SIZE; i++)
			disc[ELF_LBA][i] = elf_byte(i, 0);
		for (i = 0; i < INNER_SIZE; i++)
			disc[INNER_LBA][i] = elf_byte(i, 1);
	}
}

static void check(int ok, const char* what)
{
	if (!ok)
	{
		printf("  FAIL: %s\n", what);
		failures++;
	}
}

/* Reads of every shape: from mid-sector, whole sectors, to the end. */
static void check_reads(void)
{
	static const unsigned starts[] = {0, 1, 100, SECTOR - 1, SECTOR, SECTOR + 7, 3 * SECTOR + 499};
	static const int lens[] = {1, 47, SECTOR, SECTOR + 1, 2 * SECTOR, 3 * SECTOR + 33, BIG_SIZE};
	static unsigned char got[BIG_SIZE + 64];
	isofs_file_t f;
	unsigned s, l;
	int bad = 0;

	if (!isofs_file_open(&f, NULL, "\\BIG.BIN;1"))
	{
		check(0, "the file does not open");
		return;
	}
	check(f.length == BIG_SIZE, "the file's length");
	for (s = 0; s < sizeof(starts) / sizeof(starts[0]); s++)
		for (l = 0; l < sizeof(lens) / sizeof(lens[0]); l++)
		{
			const unsigned want = (unsigned)lens[l] < BIG_SIZE - starts[s]
				? (unsigned)lens[l] : BIG_SIZE - starts[s];
			unsigned i;
			s32 n;
			memset(got, 0xee, sizeof(got));
			isofs_file_seek(&f, starts[s]);
			n = isofs_file_read(&f, got, lens[l]);
			if (n != (s32)want || f.offset != starts[s] + want)
				bad++;
			for (i = 0; i < want; i++)
				if (got[i] != big_byte(starts[s] + i))
				{
					bad++;
					break;
				}
			if (got[want] != 0xee)
				bad++;
		}
	check(!bad, "reads from every start, of every length, hand back the file");

	/* In order, a byte at a time and then in pieces, the way the ELF
	 * loader and the directory walk read. */
	isofs_file_seek(&f, 0);
	for (s = 0, bad = 0; s < 3000; s++)
		if (isofs_file_read_byte(&f) != big_byte(s))
			bad++;
	for (; s < BIG_SIZE; s += 300)
	{
		const unsigned n = BIG_SIZE - s < 300 ? BIG_SIZE - s : 300;
		unsigned i;
		if (isofs_file_read(&f, got, 300) != (s32)n)
			bad++;
		for (i = 0; i < n; i++)
			if (got[i] != big_byte(s + i))
				bad++;
	}
	check(!bad && isofs_file_eof(&f), "a byte at a time, then in pieces, to the end");
	check(isofs_file_read(&f, got, 10) == 0, "a read at the end reads nothing");
}

static void check_lines(void)
{
	static const char* want[] = {"BOOT2 = cdrom0:\\SLUS_123.45;1", "", "VER = 1.00", "", "VMODE = NTSC", ""};
	isofs_file_t f;
	char line[64], small[8];
	unsigned i = 0;
	int bad = 0;

	if (!isofs_file_open(&f, NULL, "cdrom0:\\SYSTEM.CNF;1"))
	{
		check(0, "SYSTEM.CNF does not open");
		return;
	}
	while (!isofs_file_eof(&f) && i < 6)
	{
		isofs_file_read_line(&f, line, sizeof(line));
		if (strcmp(line, want[i++]))
			bad++;
	}
	check(!bad && i == 6 && isofs_file_eof(&f), "SYSTEM.CNF line by line, CR LF and all");

	/* A line longer than the buffer is cut, and the rest of it skipped. */
	isofs_file_seek(&f, 0);
	isofs_file_read_line(&f, small, sizeof(small));
	check(!strcmp(small, "BOOT2 =") && isofs_file_read_byte(&f) == '\n', "a long line is cut and skipped");
}

/* ------------------------------------------------------------------------
 *  The drive, for CDVDcommon.c: one data track over the disc.
 * ------------------------------------------------------------------------ */

static s32 CALLBACK t_open(const char* name) { (void)name; return 0; }
static void CALLBACK t_close(void) { }
static s32 CALLBACK t_fail_lsn(u32 lsn, int mode) { (void)lsn; (void)mode; return -1; }
static s32 CALLBACK t_fail_buf(u8* b) { (void)b; return -1; }
static s32 CALLBACK t_subq(u32 lsn, cdvdSubQ* q) { (void)lsn; (void)q; return -1; }
static s32 CALLBACK t_tn(cdvdTN* tn) { tn->strack = 1; tn->etrack = 1; return 0; }
static s32 CALLBACK t_td(u8 track, cdvdTD* td)
{
	td->lsn  = track ? 0 : SECTORS;
	td->type = CDVD_MODE1_TRACK;
	return 0;
}
static s32 CALLBACK t_toc(void* toc) { (void)toc; return -1; }
static s32 CALLBACK t_disk_type(void) { return CDVD_TYPE_DETCTCD; }
static s32 CALLBACK t_tray(void) { return CDVD_TRAY_CLOSE; }
static s32 CALLBACK t_zero(void) { return 0; }
static void CALLBACK t_new_disk(void (*callback)(void)) { (void)callback; }
static s32 CALLBACK t_read_sector(u8* buffer, u32 lsn, int mode)
{
	(void)mode;
	return isofs_read_sector(buffer, (int)lsn) ? 0 : -1;
}
static s32 CALLBACK t_dual(s32* type, u32* start) { *type = 0; *start = 0; return 0; }

CDVD_API CDVDapi_Iso = {
	t_close, t_open, t_fail_lsn, t_fail_buf, t_subq, t_tn, t_td, t_toc,
	t_disk_type, t_tray, t_zero, t_zero, t_new_disk, t_read_sector, t_dual,
};
void cdvdNewDiskCB(void) { }

/* The disc rebuilt with text for its SYSTEM.CNF; its type, found afresh. */
static s32 disc_with(const char* text)
{
	disc_cnf = text;
	build_disc();
	DoCDVDresetDiskTypeCache();
	return DoCDVDdetectDiskType();
}

/* where the host ELF is written */
static const char* tmp_dir = ".";

static void check_identify(void)
{
	char name[64];
	u32  crc = 0, entry = 0;

	CDVDsys_ChangeSource(CDVD_SOURCE_ISO);
	check(DoCDVDopen(), "the drive opens");

	/* a PS2 disc */
	check(disc_with(ps2_cnf) == CDVD_TYPE_PS2CD, "SYSTEM.CNF with BOOT2: a PS2 CD");
	check(GetPS2ElfName(name, sizeof(name)) == 2 && !strcmp(name, "cdrom0:\\SLUS_123.45;1"),
		"BOOT2's path");
	check(cdvdGetDiscRegion() == 0, "VMODE = NTSC");

	check(elf_identify_path("cdrom0:\\SLUS_123.45;1", &crc, &entry)
		&& crc == elf_crc(ELF_SIZE, 0) && entry == 0x00100008u, "the ELF BOOT2 names: its CRC and entry");
	crc = entry = 0;
	check(elf_identify_path("cdrom0:\\SLUS_123.45;7", &crc, &entry)
		&& crc == elf_crc(ELF_SIZE, 0), "any version suffix is taken as ;1");
	crc = entry = 0;
	check(elf_identify_path("cdrom:SUB\\INNER.ELF;1", &crc, &entry)
		&& crc == elf_crc(INNER_SIZE, 1) && entry == 0x00100009u, "cdrom: without a separator, a directory down");
	check(!elf_identify_path("cdrom0:\\MISSING.ELF;1", &crc, &entry), "a missing ELF");
	check(!elf_identify_path("cdrom0:\\SUB", &crc, &entry), "a directory is no ELF");

	/* the same ELF on the host */
	{
		char   path[1024];
		FILE*  fp;
		size_t i;
		strlcpy(path, tmp_dir, sizeof(path));
		strlcat(path, "/isofs_elf.tmp", sizeof(path));
		if (!(fp = fopen(path, "wb")))
			check(0, "the host ELF cannot be written");
		else
		{
			for (i = 0; i < ELF_SIZE; i++)
				fputc(elf_byte((unsigned)i, 0), fp);
			fclose(fp);
			{
				char host[1100];
				strlcpy(host, "host:", sizeof(host));
				strlcat(host, path, sizeof(host));
				crc = entry = 0;
				check(elf_identify_path(host, &crc, &entry)
					&& crc == elf_crc(ELF_SIZE, 0) && entry == 0x00100008u, "host: an ELF on the host");
			}
			remove(path);
		}
		check(!elf_identify_path("host:/nonexistent/x.elf", &crc, &entry), "host: a missing file");
	}

	/* a PS1 disc, PAL, and a malformed line among them */
	check(disc_with("BOOT = cdrom:\\SLES_000.01;1\r\nODD =  \r\nVMODE = PAL\r\n") == CDVD_TYPE_PSCD,
		"SYSTEM.CNF with BOOT: a PS1 CD");
	check(GetPS2ElfName(name, sizeof(name)) == 1 && !strcmp(name, "cdrom:\\SLES_000.01;1"), "BOOT's path");
	check(cdvdGetDiscRegion() == 1, "VMODE = PAL");

	/* neither: no disc this knows */
	check(disc_with("NOTHING = HERE\r\n") == CDVD_TYPE_ILLEGAL, "SYSTEM.CNF without BOOT: no PS disc");
	check(GetPS2ElfName(name, sizeof(name)) == 0 && name[0] == '\0' && cdvdGetDiscRegion() == -1,
		"no boot path, no region");

	disc_with(ps2_cnf);
}

static void expect(const char* path, int found, unsigned lba, unsigned size)
{
	unsigned got_lba = 0, got_size = 0;
	const int r = isofs_harness_find(path, &got_lba, &got_size);

	if (r < 0)
	{
		printf("  FAIL: the root directory did not open\n");
		failures++;
		return;
	}
	if (r != found || (found && (got_lba != lba || got_size != size)))
	{
		printf("  FAIL: %s: found %d at %u size %u, wanted %d at %u size %u\n",
			path, r, got_lba, got_size, found, lba, size);
		failures++;
	}
}

int main(int argc, char** argv)
{
	if (argc > 1)
		tmp_dir = argv[1];
	build_disc();
	check_reads();
	check_lines();

	expect("\\SYSTEM.CNF;1", 1, 24, sizeof(cnf) - 1);
	expect("cdrom0:\\SYSTEM.CNF;1", 1, 24, sizeof(cnf) - 1);
	expect("\\SUB\\INNER.ELF;1", 1, 25, 300);
	expect("cdrom0:\\SUB\\INNER.ELF;1", 1, 25, 300);
	expect("\\SUB\\DEEP\\BOTTOM.BIN;1", 1, 26, 400);
	expect("/SUB/DEEP/BOTTOM.BIN;1", 1, 26, 400);
	expect("\\SUB\\MISSING;1", 0, 0, 0);
	expect("\\INNER.ELF;1", 0, 0, 0);
	expect("\\SYSTEM.CNF;1\\X", 0, 0, 0);
	expect("\\", 0, 0, 0);
	expect("//", 0, 0, 0);
	expect("\\FAR\\ANYTHING;1", 0, 0, 0);

	check_identify();

	if (failures)
	{
		printf("isofs: FAILED (%u)\n", failures);
		return 1;
	}
	printf("isofs: ok\n");
	return 0;
}
