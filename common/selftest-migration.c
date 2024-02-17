// SPDX-License-Identifier: GPL-2.0-only
/*
 * Machine independent migration tests
 *
 * This is just a very simple test that is intended to stress the migration
 * support in the test harness. This could be expanded to test more guest
 * library code, but architecture-specific tests should be used to test
 * migration of tricky machine state.
 */
#include <libcflat.h>
#include <migrate.h>

#if defined(__arm__) || defined(__aarch64__)
/* arm can only call getchar 15 times */
#define NR_MIGRATIONS 15
#else
#define NR_MIGRATIONS 100
#endif

int main(int argc, char **argv)
{
	int i = 0;

	report_prefix_push("migration");

	for (i = 0; i < NR_MIGRATIONS; i++) {
		migrate_quiet();
		printf("migration done\n");
	}

	report(true, "simple harness stress test");

	report_prefix_pop();

	return report_summary();
}
