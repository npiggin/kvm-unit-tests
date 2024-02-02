/*
 * Test PMU
 *
 * Copyright 2024 Nicholas Piggin, IBM Corp.
 *
 * This work is licensed under the terms of the GNU LGPL, version 2.
 */
#include <libcflat.h>
#include <util.h>
#include <migrate.h>
#include <alloc.h>
#include <asm/setup.h>
#include <asm/handlers.h>
#include <asm/hcall.h>
#include <asm/processor.h>
#include <asm/barrier.h>
#include <asm/mmu.h>
#include "alloc_phys.h"
#include "vmalloc.h"

static volatile bool got_interrupt;
static volatile struct pt_regs recorded_regs;
static volatile unsigned long recorded_mmcr0;

static void reset_mmcr0(void)
{
	mtspr(SPR_MMCR0, mfspr(SPR_MMCR0) | (MMCR0_FC | MMCR0_FC56));
	mtspr(SPR_MMCR0, mfspr(SPR_MMCR0) & ~(MMCR0_PMAE | MMCR0_PMAO));
}

static __attribute__((__noinline__)) unsigned long pmc5_count_nr_insns(unsigned long nr)
{
	reset_mmcr0();
	mtspr(SPR_PMC5, 0);
	mtspr(SPR_MMCR0, mfspr(SPR_MMCR0) & ~(MMCR0_FC | MMCR0_FC56));
	asm volatile("mtctr %0 ; 1: bdnz 1b" :: "r"(nr) : "ctr");
	mtspr(SPR_MMCR0, mfspr(SPR_MMCR0) | (MMCR0_FC | MMCR0_FC56));

	return mfspr(SPR_PMC5);
}

static void test_pmc56(void)
{
	unsigned long tmp;

	report_prefix_push("pmc56");

	reset_mmcr0();
	mtspr(SPR_PMC5, 0);
	mtspr(SPR_PMC6, 0);
	report(mfspr(SPR_PMC5) == 0, "PMC5 zeroed");
	report(mfspr(SPR_PMC6) == 0, "PMC6 zeroed");
	mtspr(SPR_MMCR0, mfspr(SPR_MMCR0) & ~MMCR0_FC);
	msleep(100);
	report(mfspr(SPR_PMC5) == 0, "PMC5 frozen");
	report(mfspr(SPR_PMC6) == 0, "PMC6 frozen");
	mtspr(SPR_MMCR0, mfspr(SPR_MMCR0) & ~MMCR0_FC56);
	mdelay(100);
	mtspr(SPR_MMCR0, mfspr(SPR_MMCR0) | (MMCR0_FC | MMCR0_FC56));
	report(mfspr(SPR_PMC5) != 0, "PMC5 counting");
	report(mfspr(SPR_PMC6) != 0, "PMC6 counting");

	/* Dynamic frequency scaling could cause to be out, so don't fail. */
	tmp = mfspr(SPR_PMC6);
	report(true, "PMC6 ratio to reported clock frequency is %ld%%", tmp * 1000 / cpu_hz);

	tmp = pmc5_count_nr_insns(100);
	tmp = pmc5_count_nr_insns(1000) - tmp;
	report(tmp == 900, "PMC5 counts instructions precisely");

	report_prefix_pop();
}

static void dec_ignore_handler(struct pt_regs *regs, void *data)
{
	mtspr(SPR_DEC, 0x7fffffff);
}

static void pmi_handler(struct pt_regs *regs, void *data)
{
	got_interrupt = true;
	memcpy((void *)&recorded_regs, regs, sizeof(struct pt_regs));
	recorded_mmcr0 = mfspr(SPR_MMCR0);
	if (mfspr(SPR_MMCR0) & MMCR0_PMAO) {
		/* This may cause infinite interrupts, so clear it. */
		mtspr(SPR_MMCR0, mfspr(SPR_MMCR0) & ~MMCR0_PMAO);
	}
}

static void test_pmi(void)
{
	report_prefix_push("pmi");
	handle_exception(0x900, &dec_ignore_handler, NULL);
	handle_exception(0xf00, &pmi_handler, NULL);
	reset_mmcr0();
	mtspr(SPR_MMCR0, mfspr(SPR_MMCR0) | MMCR0_PMAO);
	mtmsr(mfmsr() | MSR_EE);
	mtmsr(mfmsr() & ~MSR_EE);
	report(got_interrupt, "PMAO caused interrupt");
	handle_exception(0xf00, NULL, NULL);
	handle_exception(0x900, NULL, NULL);
	report_prefix_pop();
}

static void clrbhrb(void)
{
	asm volatile("clrbhrb" ::: "memory");
}

static inline unsigned long mfbhrbe(int nr)
{
	unsigned long e;

	asm volatile("mfbhrbe %0,%1" : "=r"(e) : "i"(nr) : "memory");

	return e;
}

extern unsigned char dummy_branch_1[];
extern unsigned char dummy_branch_2[];

static __attribute__((__noinline__)) void bhrb_dummy(int i)
{
	asm volatile(
	"	cmpdi %0,1	\n\t"
	"	beq 1f		\n\t"
	".global dummy_branch_1	\n\t"
	"dummy_branch_1:	\n\t"
	"	b 2f		\n\t"
	"1:	trap		\n\t"
	".global dummy_branch_2	\n\t"
	"dummy_branch_2:	\n\t"
	"2:	bne 3f		\n\t"
	"	trap		\n\t"
	"3:	nop		\n\t"
	: : "r"(i));
}

static unsigned long bhrbe[10];
static int nr_bhrbe;

static void run_and_load_bhrb(void)
{
	int i;

	mtspr(SPR_MMCR0, mfspr(SPR_MMCR0) | MMCR0_BHRBA | MMCR0_PMAE);
	mtspr(SPR_MMCR0, mfspr(SPR_MMCR0) & ~(MMCR0_FC | MMCR0_FCP | MMCR0_FCPC));
	mtspr(SPR_MMCRA, mfspr(SPR_MMCRA) & ~(MMCRA_BHRBRD | MMCRA_IFM_MASK));
	enter_usermode();
	bhrb_dummy(0);
	exit_usermode();
	mtspr(SPR_MMCR0, mfspr(SPR_MMCR0) & ~MMCR0_PMAE);

	bhrbe[0] = mfbhrbe(0);
	bhrbe[1] = mfbhrbe(1);
	bhrbe[2] = mfbhrbe(2);
	bhrbe[3] = mfbhrbe(3);
	bhrbe[4] = mfbhrbe(4);
	bhrbe[5] = mfbhrbe(5);
	bhrbe[6] = mfbhrbe(6);
	bhrbe[7] = mfbhrbe(7);
	bhrbe[8] = mfbhrbe(8);
	bhrbe[9] = mfbhrbe(9);

	for (i = 0; i < 10; i++) {
		if (!bhrbe[i])
			break;
	}
	nr_bhrbe = i;
}

static void test_bhrb(void)
{
	report_prefix_push("bhrb");
	reset_mmcr0();
	clrbhrb();
	if (vm_available()) {
		handle_exception(0x900, &dec_ignore_handler, NULL);
		setup_vm();
		enter_usermode();
		bhrb_dummy(0);
		exit_usermode();
	}
	report(mfbhrbe(0) == 0, "BHRB is frozen");

	if (vm_available()) {
		int tries = 0;
		/*
		 * BHRB may be cleared at any time (e.g., by OS or hypervisor)
		 * so this test could be occasionally incorrect. Try several
		 * times before giving up...
		 */

		/*
		 * BHRB should have 8 entries:
		 * 1. enter_usermode blr
		 * 2. enter_usermode blr target
		 * 3. bl dummy
		 * 4. dummy unconditional
		 * 5. dummy conditional
		 * 6. dummy blr
		 * 7. dummy blr target
		 * 8. exit_usermode bl
		 */
		for (tries = 0; tries < 5; tries++) {
			run_and_load_bhrb();
			if (nr_bhrbe == 8)
				break;
			clrbhrb();
		}
		report(nr_bhrbe, "BHRB has been written");
		report(nr_bhrbe == 8, "BHRB has written 8 entries");
		report(bhrbe[4] == (unsigned long)dummy_branch_1,
				"correct unconditional branch address");
		report(bhrbe[3] == (unsigned long)dummy_branch_2,
				"correct conditional branch address");
	}

	handle_exception(0x900, NULL, NULL);

	report_prefix_pop();
}

int main(int argc, char **argv)
{
	report_prefix_push("pmu");

	test_pmc56();
	test_pmi();
	if (cpu_has_bhrb)
		test_bhrb();

	report_prefix_pop();

	return report_summary();
}
