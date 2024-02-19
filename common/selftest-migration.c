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
#include <asm/barrier.h>

#define NR_MIGRATIONS 10000

#define noinline __attribute__((noinline))

/* QEMU TCG migration lost dirty bit reproducer */
#define SZ 8
static char mem1[SZ];
static char mem2[SZ];
static noinline int gc(void)
{
	static int i;
	int ret = -1;

	memset(mem1, i, SZ);
	memset(mem2, i, SZ);
	assert(!memcmp(mem1, mem2, SZ));

	// return -1;
	i++;
	if (i > 100000) {
		i = 0;
		ret = 1;
	}

	return ret;
}

int main(int argc, char **argv)
{
	int i;

	for (i = 0; i < NR_MIGRATIONS; i++) {
		while (gc() == -1)
			barrier();
	}
	return 0;
}
