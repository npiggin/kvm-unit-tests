/*
 * Initialize machine setup information and I/O.
 *
 * After running setup() unit tests may query how many cpus they have
 * (nr_cpus_present), how much memory they have (PHYSICAL_END - PHYSICAL_START),
 * may use dynamic memory allocation (malloc, etc.), printf, and exit.
 * Finally, argc and argv are also ready to be passed to main().
 *
 * Copyright (C) 2016, Red Hat Inc, Andrew Jones <drjones@redhat.com>
 *
 * This work is licensed under the terms of the GNU LGPL, version 2.
 */
#include <libcflat.h>
#include <libfdt/libfdt.h>
#include <devicetree.h>
#include <alloc.h>
#include <alloc_phys.h>
#include <argv.h>
#include <asm/setup.h>
#include <asm/smp.h>
#include <asm/page.h>
#include <asm/ptrace.h>
#include <asm/processor.h>
#include <asm/hcall.h>
#include "io.h"

extern unsigned long stacktop;

char *initrd;
u32 initrd_size;

u32 cpu_to_hwid[NR_CPUS] = { [0 ... NR_CPUS-1] = (~0U) };
int nr_cpus_present;
uint64_t tb_hz;

struct mem_region mem_regions[NR_MEM_REGIONS];
phys_addr_t __physical_start, __physical_end;
unsigned __icache_bytes, __dcache_bytes;

struct cpu_set_params {
	unsigned icache_bytes;
	unsigned dcache_bytes;
	uint64_t tb_hz;
};

static void cpu_set(int fdtnode, u64 regval, void *info)
{
	const struct fdt_property *prop;
	u32 *threads;
	static bool read_common_info = false;
	struct cpu_set_params *params = info;
	int nr_threads;
	int len, i;

	/* Get the id array of threads on this node */
	prop = fdt_get_property(dt_fdt(), fdtnode,
				"ibm,ppc-interrupt-server#s", &len);
	assert(prop);

	nr_threads = len >> 2; /* Divide by 4 since 4 bytes per thread */
	threads = (u32 *)prop->data; /* Array of valid ids */

	for (i = 0; i < nr_threads; i++) {
		if (nr_cpus_present >= NR_CPUS) {
			static bool warned = false;
			if (!warned) {
				printf("Warning: Number of present CPUs exceeds maximum supported (%d).\n", NR_CPUS);
				warned = true;
			}
			break;
		}
		cpu_to_hwid[nr_cpus_present++] = fdt32_to_cpu(threads[i]);
	}

	if (!read_common_info) {
		const struct fdt_property *prop;
		u32 *data;

		prop = fdt_get_property(dt_fdt(), fdtnode,
					"i-cache-line-size", NULL);
		assert(prop != NULL);
		data = (u32 *)prop->data;
		params->icache_bytes = fdt32_to_cpu(*data);

		prop = fdt_get_property(dt_fdt(), fdtnode,
					"d-cache-line-size", NULL);
		assert(prop != NULL);
		data = (u32 *)prop->data;
		params->dcache_bytes = fdt32_to_cpu(*data);

		prop = fdt_get_property(dt_fdt(), fdtnode,
					"timebase-frequency", NULL);
		assert(prop != NULL);
		data = (u32 *)prop->data;
		params->tb_hz = fdt32_to_cpu(*data);

		read_common_info = true;
	}
}

bool cpu_has_hv;
bool cpu_has_power_mce; /* POWER CPU machine checks */
bool cpu_has_siar;
bool cpu_has_heai;
bool cpu_has_prefix;
bool cpu_has_sc_lev; /* sc interrupt has LEV field in SRR1 */
bool cpu_has_pause_short;

static void cpu_init_params(void)
{
	struct cpu_set_params params;
	int ret;

	nr_cpus_present = 0;
	ret = dt_for_each_cpu_node(cpu_set, &params);
	assert(ret == 0);
	__icache_bytes = params.icache_bytes;
	__dcache_bytes = params.dcache_bytes;
	tb_hz = params.tb_hz;

	switch (mfspr(SPR_PVR) & PVR_VERSION_MASK) {
	case PVR_VER_POWER10:
		cpu_has_prefix = true;
		cpu_has_sc_lev = true;
		cpu_has_pause_short = true;
	case PVR_VER_POWER9:
	case PVR_VER_POWER8E:
	case PVR_VER_POWER8NVL:
	case PVR_VER_POWER8:
		cpu_has_power_mce = true;
		cpu_has_heai = true;
		cpu_has_siar = true;
		break;
	default:
		break;
	}

	if (!cpu_has_hv) /* HEIR is HV register */
		cpu_has_heai = false;
}

static void mem_init(phys_addr_t freemem_start)
{
	struct dt_pbus_reg regs[NR_MEM_REGIONS];
	struct mem_region primary, mem = {
		.start = (phys_addr_t)-1,
	};
	int nr_regs, i;

	nr_regs = dt_get_memory_params(regs, NR_MEM_REGIONS);
	assert(nr_regs > 0);

	primary.end = 0;

	for (i = 0; i < nr_regs; ++i) {
		mem_regions[i].start = regs[i].addr;
		mem_regions[i].end = regs[i].addr + regs[i].size;

		/*
		 * pick the region we're in for our primary region
		 */
		if (freemem_start >= mem_regions[i].start
				&& freemem_start < mem_regions[i].end) {
			mem_regions[i].flags |= MR_F_PRIMARY;
			primary = mem_regions[i];
		}

		/*
		 * set the lowest and highest addresses found,
		 * ignoring potential gaps
		 */
		if (mem_regions[i].start < mem.start)
			mem.start = mem_regions[i].start;
		if (mem_regions[i].end > mem.end)
			mem.end = mem_regions[i].end;
	}
	assert(primary.end != 0);
//	assert(!(mem.start & ~PHYS_MASK) && !((mem.end - 1) & ~PHYS_MASK));

	__physical_start = mem.start;	/* PHYSICAL_START */
	__physical_end = mem.end;	/* PHYSICAL_END */

	phys_alloc_init(freemem_start, primary.end - freemem_start);
	phys_alloc_set_minimum_alignment(__icache_bytes > __dcache_bytes
					 ? __icache_bytes : __dcache_bytes);
}

#define EXCEPTION_STACK_SIZE	SZ_64K

static char boot_exception_stack[EXCEPTION_STACK_SIZE];
struct cpu cpus[NR_CPUS];

void cpu_init(struct cpu *cpu, int cpu_id)
{
	cpu->server_no = cpu_id;

	cpu->stack = (unsigned long)memalign(SZ_4K, SZ_64K);
	cpu->stack += SZ_64K - 64;
	cpu->exception_stack = (unsigned long)memalign(SZ_4K, SZ_64K);
	cpu->exception_stack += SZ_64K - 64;
}

void setup(const void *fdt)
{
	void *freemem = &stacktop;
	const char *bootargs, *tmp;
	struct cpu *cpu;
	u32 fdt_size;
	int ret;

	cpu_has_hv = !!(mfmsr() & (1ULL << MSR_HV_BIT));

	memset(cpus, 0xff, sizeof(cpus));

	cpu = &cpus[0];
	cpu->server_no = fdt_boot_cpuid_phys(fdt);
	cpu->exception_stack = (unsigned long)boot_exception_stack;
	cpu->exception_stack += SZ_64K - 64;

	mtspr(SPR_SPRG0, (unsigned long)cpu);
	__current_cpu = cpu;

	enable_mcheck();

	/*
	 * Before calling mem_init we need to move the fdt and initrd
	 * to safe locations. We move them to construct the memory
	 * map illustrated below:
	 *
	 * +----------------------+   <-- top of physical memory
	 * |                      |
	 * ~                      ~
	 * |                      |
	 * +----------------------+   <-- top of initrd
	 * |                      |
	 * +----------------------+   <-- top of FDT
	 * |                      |
	 * +----------------------+   <-- top of cpu0's stack
	 * |                      |
	 * +----------------------+   <-- top of text/data/bss/toc sections,
	 * |                      |       see powerpc/flat.lds
	 * |                      |
	 * +----------------------+   <-- load address
	 * |                      |
	 * +----------------------+
	 */
	fdt_size = fdt_totalsize(fdt);
	ret = fdt_move(fdt, freemem, fdt_size);
	assert(ret == 0);
	ret = dt_init(freemem);
	assert(ret == 0);
	freemem += fdt_size;

	ret = dt_get_initrd(&tmp, &initrd_size);
	assert(ret == 0 || ret == -FDT_ERR_NOTFOUND);
	if (ret == 0) {
		initrd = freemem;
		memmove(initrd, tmp, initrd_size);
		freemem += initrd_size;
	}

	assert(STACK_INT_FRAME_SIZE % 16 == 0);

	/* set parameters from dt */
	cpu_init_params();

	/* Interrupt Endianness */
	if (machine_is_pseries()) {
#if  __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
		hcall(H_SET_MODE, 1, 4, 0, 0);
#else
		hcall(H_SET_MODE, 0, 4, 0, 0);
#endif
	}

	cpu_init_ipis();

	/* cpu_init must be called before mem_init */
	mem_init(PAGE_ALIGN((unsigned long)freemem));

	/* mem_init must be called before io_init */
	io_init();

	/* finish setup */
	ret = dt_get_bootargs(&bootargs);
	assert(ret == 0 || ret == -FDT_ERR_NOTFOUND);
	setup_args_progname(bootargs);

	if (initrd) {
		/* environ is currently the only file in the initrd */
		char *env = malloc(initrd_size);
		memcpy(env, initrd, initrd_size);
		setup_env(env, initrd_size);
	}
}
