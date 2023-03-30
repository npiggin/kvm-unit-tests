/*
 * Test invalid instruction interrupt handling
 *
 * This work is licensed under the terms of the GNU LGPL, version 2.
 *
 * This tests invalid instruction handling. powernv (HV) should take an
 * HEAI interrupt with the HEIR SPR set to the instruction image. pseries
 * (guest) should take a program interrupt. CPUs which support prefix
 * should report that in (H)SRR1[34] in both cases.
 */
#include <libcflat.h>
#include <util.h>
#include <migrate.h>
#include <alloc.h>
#include <asm/handlers.h>
#include <asm/hcall.h>
#include <asm/processor.h>
#include <asm/barrier.h>

#define SPR_LPCR	0x13E
#define LPCR_HDICE	0x1UL
#define SPR_DEC		0x016

#define MSR_DR		0x0010
#define MSR_IR		0x0020
#define MSR_EE		0x8000

static bool cpu_has_heir(void)
{
	uint32_t pvr = mfspr(287);	/* Processor Version Register */

	if (!machine_is_powernv())
		return false;

	/* POWER6 has HEIR, but QEMU powernv support does not go that far */
	switch (pvr >> 16) {
	case 0x4b:			/* POWER8E */
	case 0x4c:			/* POWER8NVL */
	case 0x4d:			/* POWER8 */
	case 0x4e:			/* POWER9 */
	case 0x80:			/* POWER10 */
		return true;
	default:
		return false;
	}
}

static bool cpu_has_prefix(void)
{
	uint32_t pvr = mfspr(287);	/* Processor Version Register */
	switch (pvr >> 16) {
	case 0x80:			/* POWER10 */
		return true;
	default:
		return false;
	}
}

static bool cpu_has_lev_in_srr1(void)
{
	uint32_t pvr = mfspr(287);	/* Processor Version Register */
	switch (pvr >> 16) {
	case 0x80:			/* POWER10 */
		return true;
	default:
		return false;
	}
}

static bool regs_is_prefix(volatile struct pt_regs *regs)
{
	return (regs->msr >> (63-34)) & 1;
}

static void regs_advance_insn(struct pt_regs *regs)
{
	if (regs_is_prefix(regs))
		regs->nip += 8;
	else
		regs->nip += 4;
}

static volatile bool got_interrupt;
static volatile struct pt_regs recorded_regs;

static void dseg_handler(struct pt_regs *regs, void *data)
{
	got_interrupt = true;
	memcpy((void *)&recorded_regs, regs, sizeof(struct pt_regs));
	regs_advance_insn(regs);
	regs->msr &= ~MSR_DR;
}

static void test_dseg(void)
{
	uint64_t msr, tmp;

	report_prefix_push("data segment");

	/* Radix guest (e.g. PowerVM) needs 0x300 */
	handle_exception(0x300, &dseg_handler, NULL);
	handle_exception(0x380, &dseg_handler, NULL);

	asm volatile(
"		mfmsr	%0		\n \
		ori	%0,%0,%2	\n \
		mtmsrd	%0		\n \
		lbz	%1,0(0)		"
		: "=r"(msr), "=r"(tmp) : "i"(MSR_DR): "memory");

	report(got_interrupt, "interrupt on NULL dereference");
	got_interrupt = false;

	handle_exception(0x300, NULL, NULL);
	handle_exception(0x380, NULL, NULL);

	report_prefix_pop();
}

static void dec_handler(struct pt_regs *regs, void *data)
{
	got_interrupt = true;
	memcpy((void *)&recorded_regs, regs, sizeof(struct pt_regs));
	regs_advance_insn(regs);
	regs->msr &= ~MSR_EE;
}

static void test_dec(void)
{
	uint64_t msr;

	report_prefix_push("decrementer");

	handle_exception(0x900, &dec_handler, NULL);

	asm volatile(
"		mtdec	%1		\n \
		mfmsr	%0		\n \
		ori	%0,%0,%2	\n \
		mtmsrd	%0		"
		: "=r"(msr) : "r"(10000), "i"(MSR_EE): "memory");

	while (!got_interrupt)
		;

	report(got_interrupt, "interrupt on decrementer underflow");
	got_interrupt = false;

	handle_exception(0x900, NULL, NULL);

	if (!machine_is_powernv())
		goto done;

	handle_exception(0x980, &dec_handler, NULL);

	mtspr(SPR_LPCR, mfspr(SPR_LPCR) | LPCR_HDICE);
	asm volatile(
"		mtspr	0x136,%1	\n \
		mtdec	%3		\n \
		mfmsr	%0		\n \
		ori	%0,%0,%2	\n \
		mtmsrd	%0		"
		: "=r"(msr) : "r"(10000), "i"(MSR_EE), "r"(0x7fffffff): "memory");

	while (!got_interrupt)
		;

	mtspr(SPR_LPCR, mfspr(SPR_LPCR) & ~LPCR_HDICE);

	report(got_interrupt, "interrupt on hdecrementer underflow");
	got_interrupt = false;

	handle_exception(0x980, NULL, NULL);

done:
	report_prefix_pop();
}


static volatile uint64_t recorded_heir;

static void heai_handler(struct pt_regs *regs, void *data)
{
	got_interrupt = true;
	memcpy((void *)&recorded_regs, regs, sizeof(struct pt_regs));
	regs_advance_insn(regs);
	if (cpu_has_heir())
		recorded_heir = mfspr(SPR_HEIR);
}

static void program_handler(struct pt_regs *regs, void *data)
{
	got_interrupt = true;
	memcpy((void *)&recorded_regs, regs, sizeof(struct pt_regs));
	regs_advance_insn(regs);
}

static void test_illegal(void)
{
	report_prefix_push("illegal instruction");

	if (machine_is_powernv()) {
		handle_exception(0xe40, &heai_handler, NULL);
	} else {
		handle_exception(0x700, &program_handler, NULL);
	}

	asm volatile(".long 0x12345678" ::: "memory");
	report(got_interrupt, "interrupt on invalid instruction");
	got_interrupt = false;
	if (cpu_has_heir())
		report(recorded_heir == 0x12345678, "HEIR: 0x%08lx", recorded_heir);
	report(!regs_is_prefix(&recorded_regs), "(H)SRR1 prefix bit: %d", regs_is_prefix(&recorded_regs));

	if (cpu_has_prefix()) {
		mtspr(SPR_FSCR, mfspr(SPR_FSCR) | FSCR_PREFIX);
		asm volatile(".balign 8 ; .long 0x04000123; .long 0x00badc0d");
		report(got_interrupt, "interrupt on invalid prefix instruction");
		got_interrupt = false;
		if (cpu_has_heir())
			report(recorded_heir == 0x00badc0d04000123, "HEIR: 0x%08lx", recorded_heir);
		report(regs_is_prefix(&recorded_regs), "(H)SRR1 prefix bit: %d", regs_is_prefix(&recorded_regs));
	}

	handle_exception(0xe40, NULL, NULL);
	handle_exception(0x700, NULL, NULL);

	report_prefix_pop();
}

static void sc_handler(struct pt_regs *regs, void *data)
{
	got_interrupt = true;
	memcpy((void *)&recorded_regs, regs, sizeof(struct pt_regs));
}

static void test_sc(void)
{
	report_prefix_push("syscall");

	handle_exception(0xc00, &sc_handler, NULL);

	asm volatile("sc 0" ::: "memory");

	report(got_interrupt, "interrupt on sc 0 instruction");
	got_interrupt = false;
	if (cpu_has_lev_in_srr1())
		report(((recorded_regs.msr >> 20) & 0x3) == 0, "SRR1 set LEV=0");
	if (machine_is_powernv()) {
		asm volatile("sc 1" ::: "memory");

		report(got_interrupt, "interrupt on sc 1 instruction");
		got_interrupt = false;
		if (cpu_has_lev_in_srr1())
			report(((recorded_regs.msr >> 20) & 0x3) == 1, "SRR1 set LEV=1");
	}

	handle_exception(0xc00, NULL, NULL);

	report_prefix_pop();
}


int main(int argc, char **argv)
{
	report_prefix_push("interrupts");

	test_dseg();
	test_illegal();
	test_dec();
	test_sc();

	report_prefix_pop();

	return report_summary();
}
