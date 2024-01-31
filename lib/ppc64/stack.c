#include <libcflat.h>
#include <asm/ptrace.h>
#include <stack.h>

extern char exception_stack_marker[];

int backtrace_frame(const void *frame, const void **return_addrs, int max_depth)
{
	static int walking;
	int depth = 0;
	const unsigned long *bp = (unsigned long *)frame;
	void *return_addr;

	asm volatile("" ::: "lr"); /* Force it to save LR */

	if (walking) {
		printf("RECURSIVE STACK WALK!!!\n");
		return 0;
	}
	walking = 1;

	bp = (unsigned long *)bp[0];
	return_addr = (void *)bp[2];

	for (depth = 0; bp && depth < max_depth; depth++) {
		return_addrs[depth] = return_addr;
		if (return_addrs[depth] == 0)
			break;
		if (return_addrs[depth] == exception_stack_marker) {
			struct pt_regs *regs;

			regs = (void *)bp + STACK_FRAME_OVERHEAD;
			bp = (unsigned long *)bp[0];
			/* Represent interrupt frame with vector number */
			return_addr = (void *)regs->trap;
			if (depth + 1 < max_depth) {
				depth++;
				return_addrs[depth] = return_addr;
				return_addr = (void *)regs->nip;
			}
		} else {
			bp = (unsigned long *)bp[0];
			return_addr = (void *)bp[2];
		}
	}

	walking = 0;
	return depth;
}

int backtrace(const void **return_addrs, int max_depth)
{
	return backtrace_frame(__builtin_frame_address(0), return_addrs,
			       max_depth);
}
