/*
 * Test Special Purpose Registers
 *
 * Copyright 2017  Thomas Huth, Red Hat Inc.
 *
 * This work is licensed under the terms of the GNU LGPL, version 2.
 *
 * The basic idea of this test is to check whether the contents of the Special
 * Purpose Registers (SPRs) are preserved correctly during migration. So we
 * fill in the SPRs with a well-known value, read the values back (since not
 * all bits might be retained in the SPRs), then wait for migration to complete
 * (if the '-w' option has been specified) so that the user has a chance to
 * migrate the VM. Alternatively, the test can also simply sleep a little bit
 * with the H_CEDE hypercall, in the hope that we'll get scheduled to another
 * host CPU and thus register contents might have changed, too (in case of
 * bugs). Finally, we read back the values from the SPRs and compare them with
 * the values before the migration. Mismatches are reported as test failures.
 * Note that we do not test all SPRs since some of the registers change their
 * content automatically, and some are only accessible with hypervisor privi-
 * leges or have bad side effects, so we have to omit those registers.
 */
#include <libcflat.h>
#include <util.h>
#include <migrate.h>
#include <alloc.h>
#include <asm/handlers.h>
#include <asm/hcall.h>
#include <asm/processor.h>
#include <asm/barrier.h>

/* "Indirect" mfspr/mtspr which accept a non-constant spr number */
static uint64_t __mfspr(unsigned spr)
{
	uint64_t tmp;
	uint64_t ret;

	asm volatile(
"	bcl	20, 31, 1f		\n"
"1:	mflr	%0			\n"
"	addi	%0, %0, (2f-1b)		\n"
"	add	%0, %0, %2		\n"
"	mtctr	%0			\n"
"	bctr				\n"
"2:					\n"
".LSPR=0				\n"
".rept 1024				\n"
"	mfspr	%1, .LSPR		\n"
"	b	3f			\n"
"	.LSPR=.LSPR+1			\n"
".endr					\n"
"3:					\n"
	: "=&r"(tmp),
	  "=r"(ret)
	: "r"(spr*8) /* 8 bytes per 'mfspr ; b' block */
	: "lr", "ctr");

	return ret;
}

static void __mtspr(unsigned spr, uint64_t val)
{
	uint64_t tmp;

	asm volatile(
"	bcl	20, 31, 1f		\n"
"1:	mflr	%0			\n"
"	addi	%0, %0, (2f-1b)		\n"
"	add	%0, %0, %2		\n"
"	mtctr	%0			\n"
"	bctr				\n"
"2:					\n"
".LSPR=0				\n"
".rept 1024				\n"
"	mtspr	.LSPR, %1		\n"
"	b	3f			\n"
"	.LSPR=.LSPR+1			\n"
".endr					\n"
"3:					\n"
	: "=&r"(tmp)
	: "r"(val),
	  "r"(spr*8) /* 8 bytes per 'mfspr ; b' block */
	: "lr", "ctr", "xer");
}

static uint64_t before[1024], after[1024];

#define SPR_PR_READ	0x0001
#define SPR_PR_WRITE	0x0002
#define SPR_OS_READ	0x0010
#define SPR_OS_WRITE	0x0020
#define SPR_HV_READ	0x0100
#define SPR_HV_WRITE	0x0200

#define RW		0x333
#define RO		0x111
#define WO		0x222
#define OS_RW		0x330
#define OS_RO		0x110
#define OS_WO		0x220
#define HV_RW		0x300
#define HV_RO		0x100
#define HV_WO		0x200

#define SPR_ASYNC	0x1000	/* May be updated asynchronously */
#define SPR_INT		0x2000	/* May be updated by synchronous interrupt */
#define SPR_HARNESS	0x4000	/* Test harness uses the register */

struct spr {
	const char	*name;
	uint8_t		width;
	uint16_t	access;
	uint16_t	type;
};

/* SPRs common denominator back to PowerPC Operating Environment Architecture */
static const struct spr sprs_common[1024] = {
  [1] = {"XER",		64,	RW,		SPR_HARNESS, }, /* Compiler */
  [8] = {"LR", 		64,	RW,		SPR_HARNESS, }, /* Compiler, mfspr/mtspr */
  [9] = {"CTR",		64,	RW,		SPR_HARNESS, }, /* Compiler, mfspr/mtspr */
 [18] = {"DSISR",	32,	OS_RW,		SPR_INT, },
 [19] = {"DAR",		64,	OS_RW,		SPR_INT, },
 [26] = {"SRR0",	64,	OS_RW,		SPR_INT, },
 [27] = {"SRR1",	64,	OS_RW,		SPR_INT, },
[268] = {"TB",		64,	RO	,	SPR_ASYNC, },
[269] = {"TBU",		32,	RO,		SPR_ASYNC, },
[272] = {"SPRG0",	64,	OS_RW,		SPR_HARNESS, }, /* Int stack */
[273] = {"SPRG1",	64,	OS_RW,		SPR_HARNESS, }, /* Scratch */
[274] = {"SPRG2",	64,	OS_RW, },
[275] = {"SPRG3",	64,	OS_RW, },
[287] = {"PVR",		32,	OS_RO, },
};

/* SPRs from PowerPC Operating Environment Architecture, Book III, Vers. 2.01 */
static const struct spr sprs_201[1024] = {
 [22] = {"DEC",		32,	OS_RW,		SPR_ASYNC, },
 [25] = {"SDR1",	64,	HV_RW | OS_RO, },
 [29] = {"ACCR",	64,	OS_RW, },
[136] = {"CTRL",	32,	RO, },
[152] = {"CTRL",	32,	OS_WO, },
[259] = {"SPRG3",	64,	RO, },
/* ASR, EAR omitted */
[268] = {"TB",		64,	RO, },
[269] = {"TBU",		32,	RO, },
[284] = {"TBL",		32,	HV_WO, },
[285] = {"TBU",		32,	HV_WO, },
[310] = {"HDEC",	32,	HV_RW, },
[1013]= {"DABR",	64,	HV_RW | OS_RO, },
[1023]= {"PIR",		32,	OS_RO, },
};

static const struct spr sprs_970_pmu[1024] = {
/* POWER4+ PMU, should find PPC970 and confirm */
[770] = {"MMCRA",	64,	RO, },
[771] = {"PMC1",	32,	RO, },
[772] = {"PMC2",	32,	RO, },
[773] = {"PMC3",	32,	RO, },
[774] = {"PMC4",	32,	RO, },
[775] = {"PMC5",	32,	RO, },
[776] = {"PMC6",	32,	RO, },
[777] = {"PMC7",	32,	RO, },
[778] = {"PMC8",	32,	RO, },
[779] = {"MMCR0",	64,	RO, },
[780] = {"SIAR",	64,	RO, },
[781] = {"SDAR",	64,	RO, },
[782] = {"MMCR1",	64,	RO, },
[786] = {"MMCRA",	64,	OS_RW, },
[787] = {"PMC1",	32,	OS_RW, },
[788] = {"PMC2",	32,	OS_RW, },
[789] = {"PMC3",	32,	OS_RW, },
[790] = {"PMC4",	32,	OS_RW, },
[791] = {"PMC5",	32,	OS_RW, },
[792] = {"PMC6",	32,	OS_RW, },
[793] = {"PMC7",	32,	OS_RW, },
[794] = {"PMC8",	32,	OS_RW, },
[795] = {"MMCR0",	64,	OS_RW, },
[796] = {"SIAR",	64,	OS_RW, },
[797] = {"SDAR",	64,	OS_RW, },
[798] = {"MMCR1",	64,	OS_RW, },
};

/* These are common SPRs from 2.07S onward (POWER CPUs that support KVM HV) */
static const struct spr sprs_power_common[1024] = {
  [3] = {"DSCR",	64,	RW, },
 [13] = {"AMR",		64,	RW, },
 [17] = {"DSCR",	64,	OS_RW, },
 [28] = {"CFAR",	64,	OS_RW,		SPR_ASYNC, }, /* Effectively async */
 [29] = {"AMR",		64,	OS_RW, },
 [61] = {"IAMR",	64,	OS_RW, },
[136] = {"CTRL",	32,	RO, },
[152] = {"CTRL",	32,	OS_WO, },
[153] = {"FSCR",	64,	OS_RW, },
[157] = {"UAMOR",	64,	OS_RW, },
[159] = {"PSPB",	32,	OS_RW, },
[176] = {"DPDES",	64,	HV_RW | OS_RO, },
[180] = {"DAWR0",	64,	HV_RW, },
[186] = {"RPR",		64,	HV_RW, },
[187] = {"CIABR",	64,	HV_RW, },
[188] = {"DAWRX0",	32,	HV_RW, },
[190] = {"HFSCR",	64,	HV_RW, },
[256] = {"VRSAVE",	32,	RW, },
[259] = {"SPRG3",	64,	RO, },
[284] = {"TBL",		32,	HV_WO, }, /* Things can go a bit wonky with */
[285] = {"TBU",		32,	HV_WO, }, /* Timebase changing. Should save */
[286] = {"TBU40",	64,	HV_WO, }, /* and restore it. */
[304] = {"HSPRG0",	64,	HV_RW, },
[305] = {"HSPRG1",	64,	HV_RW, },
[306] = {"HDSISR",	32,	HV_RW,		SPR_INT, },
[307] = {"HDAR",	64,	HV_RW,		SPR_INT, },
[308] = {"SPURR",	64,	HV_RW | OS_RO,	SPR_ASYNC, },
[309] = {"PURR",	64,	HV_RW | OS_RO,	SPR_ASYNC, },
[313] = {"HRMOR",	64,	HV_RW,		SPR_HARNESS, }, /* Harness can't cope with HRMOR changing */
[314] = {"HSRR0",	64,	HV_RW,		SPR_INT, },
[315] = {"HSRR1",	64,	HV_RW,		SPR_INT, },
[318] = {"LPCR",	64,	HV_RW, },
[319] = {"LPIDR",	32,	HV_RW, },
[336] = {"HMER",	64,	HV_RW, },
[337] = {"HMEER",	64,	HV_RW, },
[338] = {"PCR",		64,	HV_RW, },
[349] = {"AMOR",	64,	HV_RW, },
[446] = {"TIR",		64,	OS_RO, },
[800] = {"BESCRS",	64,	RW, },
[801] = {"BESCRSU",	32,	RW, },
[802] = {"BESCRR",	64,	RW, },
[803] = {"BESCRRU",	32,	RW, },
[804] = {"EBBHR",	64,	RW, },
[805] = {"EBBRR",	64,	RW, },
[806] = {"BESCR",	64,	RW, },
[815] = {"TAR",		64,	RW, },
[848] = {"IC",		64,	HV_RW | OS_RO,	SPR_ASYNC, },
[849] = {"VTB",		64,	HV_RW | OS_RO,	SPR_ASYNC, },
[896] = {"PPR",		64,	RW, },
[898] = {"PPR32",	32,	RW, },
[1023]= {"PIR",		32,	OS_RO, },
};

static const struct spr sprs_tm[1024] = {
#if 0
	/* XXX: leave these out until enabling TM facility (and more testing) */
[128] = {"TFHAR",	64,	RW, },
[129] = {"TFIAR",	64,	RW, },
[130] = {"TEXASR",	64,	RW, },
[131] = {"TEXASRU",	32,	RW, },
#endif
};

/* SPRs from PowerISA 2.07 Book III-S */
static const struct spr sprs_207[1024] = {
 [22] = {"DEC",		32,	OS_RW,		SPR_ASYNC, },
 [25] = {"SDR1",	64,	HV_RW, },
[177] = {"DHDES",	64,	HV_RW, },
[283] = {"CIR",		32,	OS_RO, },
[310] = {"HDEC",	32,	HV_RW,		SPR_ASYNC, },
[312] = {"RMOR",	64,	HV_RW, },
[339] = {"HEIR",	32,	HV_RW, },
};

/* SPRs from PowerISA 3.00 Book III */
static const struct spr sprs_300[1024] = {
 [22] = {"DEC",		64,	OS_RW,		SPR_ASYNC, },
 [48] = {"PIDR",	32,	OS_RW, },
[144] = {"TIDR",	64,	OS_RW, },
[283] = {"CIR",		32,	OS_RO, },
[310] = {"HDEC",	64,	HV_RW,		SPR_ASYNC, },
[339] = {"HEIR",	32,	HV_RW, },
[464] = {"PTCR",	64,	HV_RW, },
[816] = {"ASDR",	64,	HV_RW,		SPR_INT},
[823] = {"PSSCR",	64,	OS_RW, },
[855] = {"PSSCR",	64,	HV_RW, },
};

/* SPRs from PowerISA 3.1B Book III */
static const struct spr sprs_31[1024] = {
 [22] = {"DEC",		64,	OS_RW,		SPR_ASYNC, },
 [48] = {"PIDR",	32,	OS_RW, },
[181] = {"DAWR1",	64,	HV_RW, },
[189] = {"DAWRX1",	32,	HV_RW, },
[310] = {"HDEC",	64,	HV_RW,		SPR_ASYNC, },
[339] = {"HEIR",	64,	HV_RW, },
[455] = {"HDEXCR",	32,	RO, },
[464] = {"PTCR",	64,	HV_RW, },
[468] = {"HASHKEYR",	64,	OS_RW, },
[469] = {"HASHPKEYR",	64,	HV_RW, },
[471] = {"HDEXCR",	64,	HV_RW, },
[812] = {"DEXCR",	32,	RO, },
[816] = {"ASDR",	64,	HV_RW,		SPR_INT, },
[823] = {"PSSCR",	64,	OS_RW, },
[828] = {"DEXCR",	64,	OS_RW, },
[855] = {"PSSCR",	64,	HV_RW, },
};

/* SPRs POWER9, POWER10 User Manual */
static const struct spr sprs_power9_10[1024] = {
[276] = {"SPRC",	64,	HV_RW, },
[266] = {"SPRD",	64,	HV_RW, },
[317] = {"TFMR",	64,	HV_RW, },
[799] = {"IMC",		64,	HV_RW, },
[850] = {"LDBAR",	64,	HV_RO, },
[851] = {"MMCRC",	32,	HV_RW, },
[853] = {"PMSR",	32,	HV_RO, },
[861] = {"L2QOSR",	64,	HV_WO, },
[881] = {"TRIG1",	64,	OS_WO, },
[882] = {"TRIG2",	64,	OS_WO, },
[884] = {"PMCR",	64,	HV_RW, },
[885] = {"RWMR",	64,	HV_RW, },
[895] = {"WORT",	64,	OS_RW, }, /* UM says 18-bits! */
[921] = {"TSCR",	32,	HV_RW, },
[922] = {"TTR",		64,	HV_RW, },
[1006]= {"TRACE",	64,	WO, },
[1008]= {"HID",		64,	HV_RW,		SPR_HARNESS, }, /* At least HILE would be unhelpful to change */
};

/* This covers POWER8 and POWER9 PMUs */
static const struct spr sprs_power_common_pmu[1024] = {
[768] = {"SIER",	64,	RO, },
[769] = {"MMCR2",	64,	RW, },
[770] = {"MMCRA",	64,	RW, },
[771] = {"PMC1",	32,	RW, },
[772] = {"PMC2",	32,	RW, },
[773] = {"PMC3",	32,	RW, },
[774] = {"PMC4",	32,	RW, },
[775] = {"PMC5",	32,	RW, },
[776] = {"PMC6",	32,	RW, },
[779] = {"MMCR0",	64,	RW, },
[780] = {"SIAR",	64,	RO, },
[781] = {"SDAR",	64,	RO, },
[782] = {"MMCR1",	64,	RO, },
[784] = {"SIER",	64,	OS_RW, },
[785] = {"MMCR2",	64,	OS_RW, },
[786] = {"MMCRA",	64,	OS_RW, },
[787] = {"PMC1",	32,	OS_RW, },
[788] = {"PMC2",	32,	OS_RW, },
[789] = {"PMC3",	32,	OS_RW, },
[790] = {"PMC4",	32,	OS_RW, },
[791] = {"PMC5",	32,	OS_RW, },
[792] = {"PMC6",	32,	OS_RW, },
[795] = {"MMCR0",	64,	OS_RW, },
[796] = {"SIAR",	64,	OS_RW, },
[797] = {"SDAR",	64,	OS_RW, },
[798] = {"MMCR1",	64,	OS_RW, },
};

static const struct spr sprs_power10_pmu[1024] = {
[736] = {"SEIR2",	64,	RO, },
[737] = {"SEIR3",	64,	RO, },
[738] = {"MMCR3",	64,	RO, },
[752] = {"SEIR2",	64,	OS_RW, },
[753] = {"SEIR3",	64,	OS_RW, },
[754] = {"MMCR3",	64,	OS_RW, },
};

static struct spr sprs[1024];

static bool spr_read_perms(int spr)
{
	if (machine_is_powernv())
		return !!(sprs[spr].access & SPR_HV_READ);
	else
		return !!(sprs[spr].access & SPR_OS_READ);
}

static bool spr_write_perms(int spr)
{
	if (machine_is_powernv())
		return !!(sprs[spr].access & SPR_HV_WRITE);
	else
		return !!(sprs[spr].access & SPR_OS_WRITE);
}

static void setup_sprs(void)
{
	uint32_t pvr = mfspr(287);	/* Processor Version Register */
	int i;

	for (i = 0; i < 1024; i++) {
		if (sprs_common[i].name) {
			memcpy(&sprs[i], &sprs_common[i], sizeof(struct spr));
		}
	}

	switch (pvr >> 16) {
	case 0x39:			/* PPC970 */
	case 0x3C:			/* PPC970FX */
	case 0x44:			/* PPC970MP */
		for (i = 0; i < 1024; i++) {
			if (sprs_power_common[i].name) {
				assert(!sprs[i].name);
				memcpy(&sprs[i], &sprs_power_common[i], sizeof(struct spr));
			}
			if (sprs_201[i].name) {
				assert(!sprs[i].name);
				memcpy(&sprs[i], &sprs_201[i], sizeof(struct spr));
			}
			if (sprs_970_pmu[i].name) {
				assert(!sprs[i].name);
				memcpy(&sprs[i], &sprs_power_common_pmu[i], sizeof(struct spr));
			}
		}
		break;

	case 0x4b:			/* POWER8E */
	case 0x4c:			/* POWER8NVL */
	case 0x4d:			/* POWER8 */
		for (i = 0; i < 1024; i++) {
			if (sprs_power_common[i].name) {
				assert(!sprs[i].name);
				memcpy(&sprs[i], &sprs_power_common[i], sizeof(struct spr));
			}
			if (sprs_207[i].name) {
				assert(!sprs[i].name);
				memcpy(&sprs[i], &sprs_207[i], sizeof(struct spr));
			}
			if (sprs_tm[i].name) {
				assert(!sprs[i].name);
				memcpy(&sprs[i], &sprs_tm[i], sizeof(struct spr));
			}
			if (sprs_power_common_pmu[i].name) {
				assert(!sprs[i].name);
				memcpy(&sprs[i], &sprs_power_common_pmu[i], sizeof(struct spr));
			}
		}
		break;

	case 0x4e:			/* POWER9 */
		for (i = 0; i < 1024; i++) {
			if (sprs_power_common[i].name) {
				assert(!sprs[i].name);
				memcpy(&sprs[i], &sprs_power_common[i], sizeof(struct spr));
			}
			if (sprs_300[i].name) {
				assert(!sprs[i].name);
				memcpy(&sprs[i], &sprs_300[i], sizeof(struct spr));
			}
			if (sprs_tm[i].name) {
				assert(!sprs[i].name);
				memcpy(&sprs[i], &sprs_tm[i], sizeof(struct spr));
			}
			if (sprs_power9_10[i].name) {
				assert(!sprs[i].name);
				memcpy(&sprs[i], &sprs_power9_10[i], sizeof(struct spr));
			}
			if (sprs_power_common_pmu[i].name) {
				assert(!sprs[i].name);
				memcpy(&sprs[i], &sprs_power_common_pmu[i], sizeof(struct spr));
			}
		}
		break;

	case 0x80:			/* POWER10 */
		for (i = 0; i < 1024; i++) {
			if (sprs_power_common[i].name) {
				assert(!sprs[i].name);
				memcpy(&sprs[i], &sprs_power_common[i], sizeof(struct spr));
			}
			if (sprs_31[i].name) {
				assert(!sprs[i].name);
				memcpy(&sprs[i], &sprs_31[i], sizeof(struct spr));
			}
			if (sprs_power9_10[i].name) {
				assert(!sprs[i].name);
				memcpy(&sprs[i], &sprs_power9_10[i], sizeof(struct spr));
			}
			if (sprs_power_common_pmu[i].name) {
				assert(!sprs[i].name);
				memcpy(&sprs[i], &sprs_power_common_pmu[i], sizeof(struct spr));
			}
			if (sprs_power10_pmu[i].name) {
				assert(!sprs[i].name);
				memcpy(&sprs[i], &sprs_power10_pmu[i], sizeof(struct spr));
			}
		}
		break;

	default:
		memcpy(sprs, sprs_common, sizeof(sprs));
		puts("Warning: Unknown processor version, falling back to common SPRs!\n");
		break;
	}
}

static void get_sprs(uint64_t *v)
{
	int i;

	for (i = 0; i < 1024; i++) {
		if (!spr_read_perms(i))
			continue;
		v[i] = __mfspr(i);
	}
}

static void set_sprs(uint64_t val)
{
	int i;

	for (i = 0; i < 1024; i++) {
		if (!spr_write_perms(i))
			continue;

		if (sprs[i].type & SPR_HARNESS)
			continue;
		if (!strcmp(sprs[i].name, "MMCR0")) {
			/* XXX: could use a comment or better abstraction! */
			__mtspr(i, (val & 0xfffffffffbab3fffULL) | 0xfa0b2070);
		} else {
			__mtspr(i, val);
		}
	}
}

int main(int argc, char **argv)
{
	int i;
	bool pause = false;
	uint64_t pat = 0xcafefacec0debabeULL;
	const uint64_t patterns[] = {
		0xcafefacec0debabeULL, ~0xcafefacec0debabeULL,
		0xAAAA5555AAAA5555ULL, 0x5555AAAA5555AAAAULL,
		0x1234567890ABCDEFULL, 0xFEDCBA0987654321ULL,
		-1ULL,
	};

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-w")) {
			pause = true;
		} else if (!strcmp(argv[i], "-p")) {
			i += 1;
			if (i >= argc || *argv[i] < '0'
			    || *argv[i] >= '0' + ARRAY_SIZE(patterns))
				report_abort("Error: bad value for -p");
			pat ^= patterns[*argv[i] - '0'];
		} else if (!strcmp(argv[i], "-t")) {
			/* Randomize with timebase register */
			asm volatile("mftb %0" : "=r"(i));
			pat ^= i;
			asm volatile("mftb %0" : "=r"(i));
			pat ^= ~(uint64_t)i << 32;
		} else {
			report_abort("Warning: Unsupported argument: %s",
			             argv[i]);
		}
	}

	setup_sprs();

	printf("Setting SPRs to 0x%lx...\n", pat);
	set_sprs(pat);

	memset(before, 0, sizeof(before));
	memset(after, 0, sizeof(after));

	get_sprs(before);

	if (pause) {
		migrate_once();
	} else {
		msleep(2000);

		/* Taking a dec updates SRR0, SRR1, SPRG1, so don't fail. */
		sprs[26].type |= SPR_ASYNC;
		sprs[27].type |= SPR_ASYNC;
		sprs[273].type |= SPR_ASYNC;
	}

	get_sprs(after);

	puts("Checking SPRs...\n");
	for (i = 0; i < 1024; i++) {
		bool pass = true;

		if (!spr_read_perms(i))
			continue;

		if (sprs[i].width == 32) {
			if (before[i] >> 32)
				pass = false;
		}
		if (!(sprs[i].type & SPR_ASYNC) && (before[i] != after[i]))
			pass = false;

		if (sprs[i].width == 32 && !(before[i] >> 32) && !(after[i] >> 32))
			report(pass, "%-10s(%4d):\t        0x%08lx <==>         0x%08lx",
				sprs[i].name, i,
				before[i], after[i]);
		else
			report(pass, "%-10s(%4d):\t0x%016lx <==> 0x%016lx",
				sprs[i].name, i,
				before[i], after[i]);
	}

	return report_summary();
}
