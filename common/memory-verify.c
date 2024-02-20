// SPDX-License-Identifier: GPL-2.0-only
/*
 * Simple memory verification test, used to exercise migration.
 */
#include <libcflat.h>
#include <migrate.h>
#include <alloc.h>
#include <asm/page.h>

#define NR_PAGES 32

int main(int argc, char **argv)
{
	void *mem1 = malloc(NR_PAGES*PAGE_SIZE);
	void *mem2 = malloc(NR_PAGES*PAGE_SIZE);
	bool success = true;
	long i;

	report_prefix_push("memory");

	for (i = 0; i < 10000; i++) {
		memset(mem1, i, NR_PAGES*PAGE_SIZE);
		memset(mem2, i, NR_PAGES*PAGE_SIZE);
		if (memcmp(mem1, mem2, NR_PAGES*PAGE_SIZE)) {
			success = false;
			break;
		}
	}

	report(success, "memory verification stress test");

	report_prefix_pop();

	return report_summary();
}
