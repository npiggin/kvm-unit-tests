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

static volatile uint64_t recorded_heir;
static volatile int recorded_prefix;
static volatile bool got_interrupt;

static void heai_handler(struct pt_regs *regs, void *data)
{
	got_interrupt = true;
	if (cpu_has_heir())
		recorded_heir = mfspr(SPR_HEIR);
	recorded_prefix = (regs->msr >> (63-34)) & 1;
	if (recorded_prefix)
		regs->nip += 8;
	else
		regs->nip += 4;
}

static void program_handler(struct pt_regs *regs, void *data)
{
	got_interrupt = true;
	recorded_prefix = (regs->msr >> (63-34)) & 1;
	if (recorded_prefix)
		regs->nip += 8;
	else
		regs->nip += 4;
}

int main(int argc, char **argv)
{

	if (machine_is_powernv()) {
		puts("Checking Hypervisor Emulation Assistance Interrupt...\n");
		handle_exception(0xe40, &heai_handler, NULL);
	} else {
		puts("Checking Program Interrupt...\n");
		handle_exception(0x700, &program_handler, NULL);
	}

	asm volatile(".long 0x12345678" ::: "memory");
	report(got_interrupt, "interrupt on invalid instruction");
	got_interrupt = false;
	if (cpu_has_heir())
		report(recorded_heir == 0x12345678, "HEIR: 0x%08lx", recorded_heir);
	report(recorded_prefix == 0, "(H)SRR1 prefix bit: %d", recorded_prefix);

	if (cpu_has_prefix()) {
		asm volatile(".long 0x04000123; .long 0x00badc0d");
		report(got_interrupt, "interrupt on invalid prefix instruction");
		got_interrupt = false;
		if (cpu_has_heir())
			report(recorded_heir == 0x00badc0d04000123, "HEIR: 0x%08lx", recorded_heir);
		report(recorded_prefix == 1, "(H)SRR1 prefix bit: %d", recorded_prefix);
	}

	handle_exception(0xe40, NULL, NULL);
	handle_exception(0x700, NULL, NULL);

	return report_summary();
}
